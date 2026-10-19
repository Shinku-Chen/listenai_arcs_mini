#include <string.h>
#include <strings.h>

#include "FreeRTOS.h"
#include "task.h"

#include "arcade_input.h"
#include "arcade_core.h"

/* 注入点按的保持时长：够长能被游戏的一帧读到，又不会粘住 */
#define ARCADE_INPUT_PULSE_MS 180

static uint32_t s_hold;      /* 持续按住的按键 */
static uint32_t s_pulse;     /* 点按中的按键 */
static TickType_t s_pulse_deadline;

static const struct {
    const char *name;
    uint32_t key;
} s_key_table[] = {
    {"up", ARCADE_KEY_UP},
    {"down", ARCADE_KEY_DOWN},
    {"left", ARCADE_KEY_LEFT},
    {"right", ARCADE_KEY_RIGHT},
    {"a", ARCADE_KEY_A},
    {"b", ARCADE_KEY_B},
    {"start", ARCADE_KEY_START},
    {"select", ARCADE_KEY_SELECT},
};

/*
 * USB HID keyboard usage ID 到 GB 按键。
 *
 * 方向：方向键或 WASD；A/B：Z/X；Start：Enter；Select：右 Shift 或左 Ctrl。
 */
static const struct {
    uint8_t usage;
    uint32_t key;
} s_hid_key_table[] = {
    {0x52, ARCADE_KEY_UP},
    {0x51, ARCADE_KEY_DOWN},
    {0x50, ARCADE_KEY_LEFT},
    {0x4F, ARCADE_KEY_RIGHT},
    {0x1A, ARCADE_KEY_UP},    /* W */
    {0x16, ARCADE_KEY_DOWN},  /* S */
    {0x04, ARCADE_KEY_LEFT},  /* A */
    {0x07, ARCADE_KEY_RIGHT}, /* D */
    {0x1D, ARCADE_KEY_A},     /* Z */
    {0x1B, ARCADE_KEY_B},     /* X */
    {0x28, ARCADE_KEY_START}, /* Enter */
    {0xE5, ARCADE_KEY_SELECT},/* 右 Shift */
    {0xE0, ARCADE_KEY_SELECT},/* 左 Ctrl */
};

/*
 * USB 键盘报告存到这两个变量里，由 arcade 任务在下一帧取走。
 * 报告回调跑在 USB host 线程，所以这里只做“写快照”，不做其它事情。
 */
static volatile uint32_t s_kbd_mask;
static volatile uint8_t s_kbd_exit_request;

/*
 * USB 手柄（HID 非 boot 设备）。
 * 报告回调在 USB host 线程，这里只保存整份状态，不调 malloc/LVGL。
 * 手柄发的是“全量状态”（而不是键鼠那种增量），所以直接覆盖。
 */
#define ARCADE_PAD_RAW_MAX 24

static volatile uint32_t s_pad_mask;
static volatile uint32_t s_pad_reports;
static volatile uint8_t s_pad_raw[ARCADE_PAD_RAW_MAX];
static volatile uint8_t s_pad_raw_len;
static volatile uint8_t s_pad_xinput;

uint32_t arcade_input_mask(void)
{
    if (s_pulse != 0 && xTaskGetTickCount() >= s_pulse_deadline) {
        s_pulse = 0;
    }

    return s_hold | s_pulse | s_pad_mask;
}

void arcade_input_hold(uint32_t keys, bool pressed)
{
    if (pressed) {
        s_hold |= keys;
    } else {
        s_hold &= ~keys;
    }
}

void arcade_input_tap(uint32_t keys)
{
    s_pulse |= keys;
    s_pulse_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ARCADE_INPUT_PULSE_MS);
}

void arcade_input_release_all(void)
{
    s_hold = 0;
    s_pulse = 0;
    s_pad_mask = 0;
    s_kbd_mask = 0;
}

uint32_t arcade_input_key_from_name(const char *name)
{
    if (name == NULL) {
        return 0;
    }

    for (size_t i = 0; i < sizeof(s_key_table) / sizeof(s_key_table[0]); i++) {
        if (strcasecmp(name, s_key_table[i].name) == 0) {
            return s_key_table[i].key;
        }
    }

    return 0;
}

const char *arcade_input_name(uint32_t key)
{
    for (size_t i = 0; i < sizeof(s_key_table) / sizeof(s_key_table[0]); i++) {
        if (s_key_table[i].key == key) {
            return s_key_table[i].name;
        }
    }

    return "?";
}

/* ---------------- USB 键盘 ---------------- */

/*
 * src/middleware/usb/app_usb_cherry_hid.c 里的弱符号，插上 USB 键盘后由
 * HID host 报告回调调用（USB host 线程上下文），这里只做快照。
 */
void app_usb_hid_keyboard_report(const uint8_t *keys)
{
    uint32_t mask = 0;

    if (keys == NULL) {
        return;
    }

    for (int i = 1; i < 8; i++) {
        uint8_t usage = keys[i];

        if (usage == 0) {
            continue;
        }

        if (usage == 0x29) { /* ESC：退出游戏回到列表 */
            s_kbd_exit_request = 1;
            continue;
        }

        for (size_t k = 0; k < sizeof(s_hid_key_table) / sizeof(s_hid_key_table[0]); k++) {
            if (s_hid_key_table[k].usage == usage) {
                mask |= s_hid_key_table[k].key;
                break;
            }
        }
    }

    s_kbd_mask = mask;
}

uint32_t arcade_input_take_keyboard(bool *exit_request)
{
    if (exit_request != NULL) {
        *exit_request = s_kbd_exit_request != 0;
    }
    s_kbd_exit_request = 0;

    return s_kbd_mask;
}

/* ---------------- USB 手柄 ---------------- */

/* 摇杆当作方向键的阈值（int16 量程的 1/2） */
#define ARCADE_PAD_DEADZONE 16384

static int16_t pad_axis(const uint8_t *r, int off)
{
    return (int16_t)(uint16_t)(r[off] | (r[off + 1] << 8));
}

/**
 * XInput 报告（Xbox 360 有线 / 8BitDo 等兼容手柄），20 字节：
 *   [0]=0x00 [1]=0x14 [2..3]=按键 LE [4]=LT [5]=RT
 *   [6..7]=LX [8..9]=LY [10..11]=RX [12..13]=RY
 * 按键位：bit0 Up, bit1 Down, bit2 Left, bit3 Right, bit4 Start,
 *         bit5 Back, bit12 A, bit13 B, bit14 X, bit15 Y
 * 注意 LY 是反的：0x0000 = 上，0xFFFF = 下。
 */
static uint32_t pad_mask_xinput(const uint8_t *r, int nbytes)
{
    uint16_t btn;
    int16_t lx;
    int16_t ly;
    uint32_t mask = 0;

    if (nbytes < 14) {
        return 0;
    }

    btn = (uint16_t)(r[2] | (r[3] << 8));

    if (btn & 0x0001) {
        mask |= ARCADE_KEY_UP;
    }
    if (btn & 0x0002) {
        mask |= ARCADE_KEY_DOWN;
    }
    if (btn & 0x0004) {
        mask |= ARCADE_KEY_LEFT;
    }
    if (btn & 0x0008) {
        mask |= ARCADE_KEY_RIGHT;
    }
    if (btn & 0x0010) {
        mask |= ARCADE_KEY_START;
    }
    if (btn & 0x0020) {
        mask |= ARCADE_KEY_SELECT;
    }
    if (btn & 0x1000) { /* A */
        mask |= ARCADE_KEY_A;
    }
    if (btn & 0x2000) { /* B */
        mask |= ARCADE_KEY_B;
    }
    if (btn & 0x4000) { /* X：别名到 A */
        mask |= ARCADE_KEY_A;
    }
    if (btn & 0x8000) { /* Y：别名到 B */
        mask |= ARCADE_KEY_B;
    }

    /* 左摇杆也能当方向键 */
    if (nbytes >= 10) {
        lx = pad_axis(r, 6);
        ly = pad_axis(r, 8);

        if (lx <= -ARCADE_PAD_DEADZONE) {
            mask |= ARCADE_KEY_LEFT;
        } else if (lx >= ARCADE_PAD_DEADZONE) {
            mask |= ARCADE_KEY_RIGHT;
        }
        if (ly <= -ARCADE_PAD_DEADZONE) {
            mask |= ARCADE_KEY_UP;
        } else if (ly >= ARCADE_PAD_DEADZONE) {
            mask |= ARCADE_KEY_DOWN;
        }
    }

    return mask;
}

/**
 * 通用 HID 手柄：没有固定布局，这里用最常见的 8 字节约定：
 *   [0] bit0 Up, bit1 Down, bit2 Left, bit3 Right,
 *       bit4 A, bit5 B, bit6 Select, bit7 Start
 * 布局对不上时用 `arcade pad` 看原始报告，再改这里。
 */
static uint32_t pad_mask_generic(const uint8_t *r, int nbytes)
{
    uint8_t btn;
    uint32_t mask = 0;

    if (nbytes < 1) {
        return 0;
    }

    btn = r[0];

    if (btn & 0x01) {
        mask |= ARCADE_KEY_UP;
    }
    if (btn & 0x02) {
        mask |= ARCADE_KEY_DOWN;
    }
    if (btn & 0x04) {
        mask |= ARCADE_KEY_LEFT;
    }
    if (btn & 0x08) {
        mask |= ARCADE_KEY_RIGHT;
    }
    if (btn & 0x10) {
        mask |= ARCADE_KEY_A;
    }
    if (btn & 0x20) {
        mask |= ARCADE_KEY_B;
    }
    if (btn & 0x40) {
        mask |= ARCADE_KEY_SELECT;
    }
    if (btn & 0x80) {
        mask |= ARCADE_KEY_START;
    }

    return mask;
}

/*
 * src/middleware/usb/app_usb_cherry_hid.c 的弱符号：非键鼠的 HID 输入报告
 * （手柄）原样送到这里，跑在 USB host 线程上下文。
 */
void app_usb_hid_generic_report(const uint8_t *report, int nbytes)
{
    int copy;

    if (report == NULL || nbytes <= 0) {
        s_pad_mask = 0;
        return;
    }

    copy = nbytes < ARCADE_PAD_RAW_MAX ? nbytes : ARCADE_PAD_RAW_MAX;
    for (int i = 0; i < copy; i++) {
        s_pad_raw[i] = report[i];
    }
    s_pad_raw_len = (uint8_t)copy;
    s_pad_reports++;

    /* XInput：20 字节报告，头部 00 14（type=0x00, len=0x14） */
    if (nbytes >= 14 && report[0] == 0x00 && report[1] == 0x14) {
        s_pad_xinput = 1;
        s_pad_mask = pad_mask_xinput(report, nbytes);
    } else {
        s_pad_xinput = 0;
        s_pad_mask = pad_mask_generic(report, nbytes);
    }
}

uint32_t arcade_input_pad_info(uint8_t *raw, int raw_max, int *raw_len, bool *is_xinput)
{
    uint8_t len = s_pad_raw_len;
    int copy;

    if (raw_len != NULL) {
        *raw_len = (int)len;
    }
    if (is_xinput != NULL) {
        *is_xinput = s_pad_xinput != 0;
    }
    if (raw != NULL && raw_max > 0) {
        copy = len < raw_max ? (int)len : raw_max;
        for (int i = 0; i < copy; i++) {
            raw[i] = s_pad_raw[i];
        }
    }

    return s_pad_reports;
}
