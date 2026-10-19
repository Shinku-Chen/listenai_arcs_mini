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

uint32_t arcade_input_mask(void)
{
    if (s_pulse != 0 && xTaskGetTickCount() >= s_pulse_deadline) {
        s_pulse = 0;
    }

    return s_hold | s_pulse;
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
