/*
 * 模拟器应用：页面状态机 + 按键 + shell 调试命令。
 *
 * 整个应用只有一个任务：模拟器帧循环和 LVGL 刷新都在这里串行执行，
 * 避免 LVGL 多线程访问问题。shell 命令只投递请求，实际动作在主循环里做。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"

#include "lisa_log.h"
#include "shell.h"
#include "cmd/cmd.h"
#include "sys_init.h"

#include "voice_msg.h"
#include "service_button.h"
#include "service_brightness.h"

#include "arcade_app.h"
#include "arcade_audio.h"
#include "arcade_core.h"
#include "arcade_gb.h"
#include "arcade_input.h"
#include "arcade_rom.h"
#include "arcade_save.h"
#include "arcade_ui.h"
#include "arcade_video.h"

#include "lsfs.h"
#include "esp_heap_caps.h"

/* POWER_EN 的 AON 强制输出位（`arcade pwrho1d`，开发期刷机用） */
#include "arcs_ap.h"
#include "pinmux.h"

#define TAG "arcade"

#define ARCADE_TASK_STACK     4096
#define ARCADE_TASK_PRIORITY  4

/* fs 自检用的临时目录/文件（放在 ROM 目录下，确认卡真的可写） */
#define ARCADE_FS_TEST_DIR    CONFIG_ARCADE_ROM_DIR "/.arcade-fs-test"
#define ARCADE_FS_TEST_FILE   ARCADE_FS_TEST_DIR "/probe.bin"
#define ARCADE_FS_PROBE_LEN   32

/* saves 列表最多打印的存档数 */
#define ARCADE_SAVES_MAX 32

typedef enum {
    ARCADE_MODE_LIST = 0,
    ARCADE_MODE_PLAY,
} arcade_mode_t;

typedef enum {
    ARCADE_REQ_NONE = 0,
    ARCADE_REQ_PLAY,
    ARCADE_REQ_EXIT,
    ARCADE_REQ_RESCAN,
    ARCADE_REQ_SAVE,
    ARCADE_REQ_FB_REPORT,
    ARCADE_REQ_FB_FILL,
    ARCADE_REQ_FB_RESUME,
    ARCADE_REQ_SPI,
} arcade_req_t;

static arcade_mode_t s_mode = ARCADE_MODE_LIST;
static int s_sel;
/* 按键任务写、模拟器任务读，必须 volatile，否则可能被优化成只读一次的循环 */
static volatile uint8_t s_pending_action = 0xFF;

static volatile arcade_req_t s_req;
static volatile int s_req_arg;
/* arcade spi 的结果（在模拟器任务里改频，不能直接从 shell 任务碰 SPI） */
static volatile int s_spi_result;
static volatile uint32_t s_spi_freq_req;
/* shell 的 arcade save 命令等这个结果 */
static volatile int s_save_result;

/* arcade fb：冻结画面并把帧缓冲/刷屏链路的状态回报给 shell */
static volatile bool s_frozen;
static volatile bool s_fb_ready;
static char s_fb_report[1024];

static uint32_t s_frames;
static TickType_t s_fps_tick;
static uint32_t s_fps_x10;

/*
 * 帧节奏（固定节拍）。
 *
 * 一帧的预算是 CONFIG_ARCADE_FRAME_PERIOD_MS（16ms）。以前那套「自适应步长」
 * 把步长抬到「工作量 + 2ms」，回落条件又是 target + 3 < 步长，稳态就落在
 * 工作量 + 5ms —— 实测干 18ms 的活却按 23ms 一拍跑，白丢 20% 帧率
 * （43.5fps，而同样的工作本可以跑 54fps）。
 *
 * 现在只做一件事：干完活如果还有富余就睡到 16ms 的固定节拍，超预算就不睡
 * （超多少跑多少，这才是最快的）。连续超预算时每 8 帧让一个 tick 给 IDLE。
 *   - `arcade period <ms>` 可以手动钉住节拍做对比实验，0 = 回到固定节拍。
 */
#define ARCADE_MIN_FRAME_PERIOD_MS  CONFIG_ARCADE_FRAME_PERIOD_MS
#define ARCADE_MAX_FRAME_PERIOD_MS  60
static uint32_t s_period_ms = CONFIG_ARCADE_FRAME_PERIOD_MS;
static uint32_t s_work_ema = CONFIG_ARCADE_FRAME_PERIOD_MS;
/* 连续超预算的帧数（每 8 帧让一个 tick 给 IDLE，别把它饿死） */
static uint32_t s_overrun_count;
static bool s_period_auto = true;

/*
 * 自动缩放。
 *
 * 口径（用户要求）：大画面优先，但速度永远优先于画面 —— 帧率不够时自己
 * 降到 1x，不让用户手动切。用帧周期（rdcycle 实测）判定，不用 fps
 * （fps 是 1 秒窗口的平均值，反映太慢）。
 *
 *   1.5x 的额外开销只有 1ms 左右（缩放任务单独跑），所以常态应该是 1.5x；
 *   只有帧周期持续超过 17.5ms 才降级。回切需要连续 5 秒都很宽裕。
 */
#define SCALE_DOWN_US       17500U
#define SCALE_UP_US         16200U
#define SCALE_DOWN_FRAMES   60U
#define SCALE_UP_FRAMES     300U

static bool s_scale_auto = true;
static uint32_t s_frame_us;          /* 帧周期 EMA（µs） */
static uint32_t s_scale_down_cnt;
static uint32_t s_scale_up_cnt;
static uint32_t s_last_frame_cyc;
/* PLAY 模式下把 LVGL 定时器降频后，用来数帧 */
static uint32_t s_lvgl_skip;

/*
 * 跳帧保速（skip / render decoupling）。
 *
 * 背景：GBC 模拟一帧路逻辑要 ~13.7ms，再加上 PPU 画像素 ~4.5ms 就到 18.2ms，
 * 超过 GB 机的真实帧周期 16.74ms（70224 tick / 4.194304MHz）—— 帧率只能到
 * ~55fps，于是「游戏变慢、音高偏低」。
 *
 * 但这 60Hz 里真正不能慢的是**逻辑与音频**，画面可以少画几张。所以：
 *   - 模拟（CPU/PPU 状态机/APU/定时器）每一帧必跑，按 16.74ms 节拍锁住；
 *   - 像素渲染/送屏按「预算信用」挑着做，不够就跳（保留上一帧画面）。
 *
 * 信用账：每帧进账一个帧预算，花掉这一帧的实测耗时（rdcycle）。
 * 只要信用 ≥ 渲染的额外开销就渲染。稳态会自动收敛成 2/3 帧渲染（~40fps）、
 * 逻辑恒定 59.7fps。
 */
#define ARCADE_GB_FRAME_US       16743U   /* GB 真实帧周期 */
#define ARCADE_RENDER_EXTRA_INIT 4500U    /* 渲染额外开销初值（µs） */

static bool s_skip_auto = true;                       /* 自动跳帧保速 */
static arcade_render_mode_t s_skip_mech = ARCADE_RENDER_SKIP;  /* 跳过的实现方式 */
static int32_t s_skip_credit_us;                      /* 渲染余量信用（µs） */
static uint32_t s_render_extra_us = ARCADE_RENDER_EXTRA_INIT;
static uint32_t s_render_cost_us = 18200;             /* 渲染帧耗时 EMA */
static uint32_t s_skip_cost_us = 13700;               /* 跳过帧耗时 EMA */
static bool s_last_render = true;
static uint32_t s_logic_frames;
static uint32_t s_render_frames;
static uint32_t s_skipped_frames;
static uint32_t s_logic_fps_x10;
static uint32_t s_render_fps_x10;
static TickType_t s_rate_tick;

/* 最近若干帧的帧间隔（ms），用来回答「抖不抖」 */
#define FRAME_STAT_SLOTS 64
static uint8_t s_frame_ms[FRAME_STAT_SLOTS];
static uint32_t s_frame_pos;
static uint32_t s_frame_cnt;

extern int boot_watchdog_feed(void);
extern struct lsfs_mount_t *platform_sd_mount_get(void);
/* 时钟频率（MHz 级即可），用来把耗时换算成 cycles 参考 */
extern volatile uint32_t SystemCoreClock;

/*
 * 启动探针：平台的 voice_platform_init() 在 PRE_APPLICATION/60 才打印第一行日志，
 * 如果在这之前就死了，串口是完全没有输出的。这里在 PRE_APPLICATION/1 打一条，
 * 把“死在设备驱动层”和“死在应用层”区分开。
 */
static int arcade_boot_probe(void)
{
    LISA_LOGI(TAG, "boot probe: pre-application reached");
    return 0;
}
SYS_INIT(arcade_boot_probe, SYS_INIT_LEVEL_PRE_APPLICATION, 1);

/* ------------- 按键 ------------- */

static void arcade_button_cb(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    const voice_msg_button_evt_t *evt = (const voice_msg_button_evt_t *)data;

    (void)unused;
    (void)msg_id;
    (void)len;
    (void)user_data;

    if (evt == NULL) {
        return;
    }

    switch (evt->action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        s_pending_action = (uint8_t)evt->action;
        break;
    default:
        break;
    }
}

/* ------------- 帧缓冲自检（只在 arcade 任务里跑） ------------- */

/*
 * 区分「数据错」还是「刷屏错」：
 *   - 采样帧缓冲若干点的 RGB565，如果采样值全是调色板里的值，
 *     说明 Peanut-GB 的扫描线回调确实写进去了；
 *   - 再配合 `arcade fb fill <n>` 把整块填成纯色并刷一帧：
 *     如果屏上变成该颜色 → 刷屏链路 OK，问题在数据；
 *     如果屏上没变 → canvas / flush 链路有问题。
 */
static void arcade_fb_report(void)
{
    int w = arcade_video_fb_width();
    int h = arcade_video_fb_height();
    int k_sx[3];
    int k_sy[3];
    lv_color_t *fb = arcade_video_get_fb();
    int off = 0;
    int uniq[5] = {0};

    k_sx[0] = 0;
    k_sx[1] = w / 2;
    k_sx[2] = w - 1;
    k_sy[0] = 0;
    k_sy[1] = h / 2;
    k_sy[2] = h - 1;
    int uniq_n = 0;

    if (fb == NULL) {
        snprintf(s_fb_report, sizeof(s_fb_report), "fb=(null)");
        return;
    }

    for (int i = 0; i < w * h; i++) {
        uint16_t v = fb[i].full;
        int j;

        for (j = 0; j < uniq_n; j++) {
            if (uniq[j] == (int)v) {
                break;
            }
        }
        if (j == uniq_n && uniq_n < 5) {
            uniq[uniq_n++] = (int)v;
        }
    }

    off += snprintf(s_fb_report + off, sizeof(s_fb_report) - off,
                    "pal=%04X,%04X,%04X,%04X uniq=%d(",
                    (unsigned)arcade_video_palette_raw(0),
                    (unsigned)arcade_video_palette_raw(1),
                    (unsigned)arcade_video_palette_raw(2),
                    (unsigned)arcade_video_palette_raw(3),
                    uniq_n);
    for (int i = 0; i < uniq_n && off < (int)sizeof(s_fb_report) - 8; i++) {
        off += snprintf(s_fb_report + off, sizeof(s_fb_report) - off, "%s%04X",
                        i ? "," : "", (unsigned)uniq[i]);
    }
    off += snprintf(s_fb_report + off, sizeof(s_fb_report) - off, ")\n");

    for (int r = 0; r < 3 && off < (int)sizeof(s_fb_report) - 40; r++) {
        off += snprintf(s_fb_report + off, sizeof(s_fb_report) - off, "y=%-3d", k_sy[r]);
        for (int c = 0; c < 3; c++) {
            int idx = k_sy[r] * w + k_sx[c];
            off += snprintf(s_fb_report + off, sizeof(s_fb_report) - off, " %04X",
                            (unsigned)fb[idx].full);
        }
        off += snprintf(s_fb_report + off, sizeof(s_fb_report) - off, "\n");
    }

    if (off < (int)sizeof(s_fb_report) - 200) {
        arcade_ui_player_debug(s_fb_report + off, (uint32_t)(sizeof(s_fb_report) - off));
    }
}

/* ------------- 页面切换（只在 arcade 任务里调用） ------------- */

static void arcade_show_list(void)
{
    s_mode = ARCADE_MODE_LIST;
    arcade_input_release_all();
    arcade_ui_show_list();
    arcade_ui_list_refresh(s_sel);
}

static void arcade_start_selected(void)
{
    const char *path = arcade_rom_path(s_sel);

    if (path == NULL) {
        arcade_ui_show_list();
        arcade_ui_list_set_hint("没有可运行的 ROM");
        return;
    }

    LISA_LOGI(TAG, "loading %s", path);

    /* 2MB 的 ROM 从 TF 卡读进 PSRAM 需要一两秒，先给个可见提示，
     * 否则这段时间屏幕一片黑，看起来像卡死。 */
    arcade_ui_show_player();
    arcade_ui_player_message("载入中…");
    lv_task_handler();

    if (arcade_core_start(path) != 0) {
        arcade_show_list();
        arcade_ui_list_set_hint("加载失败（格式不支持或校验错误）");
        return;
    }

    arcade_ui_player_message(NULL);
    arcade_video_fill_black();
    lv_task_handler();

    /* 进入游戏才开始出声（列表页保持静音） */
    arcade_audio_set_active(true);

    s_mode = ARCADE_MODE_PLAY;
    s_frames = 0;
    s_fps_tick = xTaskGetTickCount();
    s_fps_x10 = 0;
    /* 自动缩放从 1.5x（大画面）开始，跑不动再自己降 */
    s_last_frame_cyc = 0;
    s_frame_us = 0;
    s_scale_down_cnt = 0;
    s_scale_up_cnt = 0;
    /* 跳帧保速的信用与成本估计重新学（不同 ROM 的开销差很多） */
    s_skip_credit_us = 0;
    s_render_extra_us = ARCADE_RENDER_EXTRA_INIT;
    s_render_cost_us = 18200;
    s_skip_cost_us = 13700;
    s_last_render = true;
    s_logic_frames = 0;
    s_render_frames = 0;
    s_skipped_frames = 0;
    s_rate_tick = 0;
    if (s_scale_auto) {
        arcade_video_set_scale(2);
    }
}

static void arcade_stop_game(void)
{
    arcade_audio_set_active(false);
    arcade_core_stop();
    /* 先把在途的游戏帧送完，否则最后一帧会盖掉 LVGL 刚画好的列表页 */
    arcade_video_present_drain();
    arcade_show_list();
}

static void arcade_rescan(void)
{
    arcade_core_stop();
    arcade_rom_invalidate();
    arcade_ui_show_list();
    arcade_ui_list_set_hint("正在读取 TF 卡…");
    arcade_ui_list_refresh(s_sel);
    lv_task_handler();

    int count = arcade_rom_scan();

    if (s_sel >= count) {
        s_sel = count > 0 ? count - 1 : 0;
    }

    arcade_show_list();
}

/* ------------- 按键处理 ------------- */

static void arcade_handle_action(uint8_t action)
{
    int count = arcade_rom_count();

    LISA_LOGI(TAG, "action=%d mode=%s", (int)action,
              s_mode == ARCADE_MODE_LIST ? "list" : "play");

    if (s_mode == ARCADE_MODE_LIST) {
        switch (action) {
        case VOICE_MSG_BUTTON_ACTION_CLICK:
            if (count > 0) {
                s_sel = (s_sel + 1) % count;
                arcade_ui_list_refresh(s_sel);
            }
            break;
        case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
            if (count > 0) {
                s_sel = (s_sel + count - 1) % count;
                arcade_ui_list_refresh(s_sel);
            }
            break;
        case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
            arcade_start_selected();
            break;
        case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
            arcade_rescan();
            break;
        default:
            break;
        }
        return;
    }

    /* 游戏页 */
    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
        arcade_input_tap(ARCADE_KEY_A);
        break;
    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
        arcade_input_tap(ARCADE_KEY_B);
        break;
    case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
        arcade_input_tap(ARCADE_KEY_START);
        break;
    case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
        arcade_input_tap(ARCADE_KEY_SELECT);
        break;
    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        LISA_LOGI(TAG, "long press, back to ROM list");
        arcade_stop_game();
        break;
    default:
        break;
    }
}

static void arcade_handle_request(void)
{
    arcade_req_t req = s_req;
    int arg = s_req_arg;

    if (req == ARCADE_REQ_NONE) {
        return;
    }

    s_req = ARCADE_REQ_NONE;

    switch (req) {
    case ARCADE_REQ_PLAY:
        if (arg >= 0 && arg < arcade_rom_count()) {
            s_sel = arg;
            arcade_start_selected();
        }
        break;
    case ARCADE_REQ_EXIT:
        arcade_stop_game();
        break;
    case ARCADE_REQ_RESCAN:
        arcade_rescan();
        break;
    case ARCADE_REQ_SAVE:
        s_save_result = arcade_save_flush();
        break;
    case ARCADE_REQ_FB_REPORT:
        arcade_fb_report();
        s_fb_ready = true;
        break;
    case ARCADE_REQ_FB_FILL:
        s_frozen = true;
        arcade_video_fill(arg);
        arcade_video_present_now();
        break;
    case ARCADE_REQ_FB_RESUME:
        s_frozen = false;
        arcade_video_fill_black();
        arcade_video_present_now();
        break;
    case ARCADE_REQ_SPI:
        /* 改 SPI 时钟必须在没有在途传输的时候做：先把流水线清空
         * （present_drain 会等到两块目标缓冲都回到空闲队列），
         * 然后直接把新频率写进 lisa_spi 的当前配置。
         * bus_spi4_write_pixels() 每帧都会 get_config 再 configure，
         * 所以下一帧就会按新频率跑，不用重烧。 */
        arcade_video_present_drain();
        s_spi_result = arcade_video_set_spi_freq(s_spi_freq_req);
        break;
    default:
        break;
    }
}

/* ------------- 主循环 ------------- */

/* 记一帧的间隔（ms tick）；只统计播放状态的帧 */
static void frame_stat_push(TickType_t spent)
{
    if (s_mode != ARCADE_MODE_PLAY) {
        return;
    }

    s_frame_ms[s_frame_pos] = (uint8_t)(spent > 255 ? 255 : spent);
    s_frame_pos = (s_frame_pos + 1) % FRAME_STAT_SLOTS;
    if (s_frame_cnt < FRAME_STAT_SLOTS) {
        s_frame_cnt++;
    }
}

/* 打印帧间隔统计：min/avg/p95/max，抖动看 p95 和 max 的差 */
static void frame_stat_report(char *out, uint32_t len)
{
    uint8_t sorted[FRAME_STAT_SLOTS];
    uint32_t n = s_frame_cnt;
    uint32_t sum = 0;

    if (out == NULL || len == 0) {
        return;
    }

    if (n == 0) {
        snprintf(out, len, "frame n=0");
        return;
    }

    memcpy(sorted, s_frame_ms, n);
    for (uint32_t i = 1; i < n; i++) {
        uint8_t key = sorted[i];
        int32_t j = (int32_t)i - 1;

        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    for (uint32_t i = 0; i < n; i++) {
        sum += sorted[i];
    }

    /* p95 用「第 90% 分位」的写法，n<20 时基本等于 max */
    uint32_t p95 = sorted[(n * 95U) / 100U >= n ? n - 1 : (n * 95U) / 100U];

    snprintf(out, len, "frame n=%u min=%u avg=%u p95=%u max=%u",
             (unsigned)n, (unsigned)sorted[0], (unsigned)(sum / n),
             (unsigned)p95, (unsigned)sorted[n - 1]);
}

static void arcade_frame_tick(void)
{
    TickType_t now = xTaskGetTickCount();

    s_frames++;

    if ((now - s_fps_tick) >= pdMS_TO_TICKS(1000)) {
        uint32_t elapsed = (uint32_t)(now - s_fps_tick);
        s_fps_x10 = (uint32_t)((s_frames * 10000U) / (elapsed > 0 ? elapsed : 1));
        s_frames = 0;
        s_fps_tick = now;
    }
}

/* 逻辑帧 / 渲染帧分开数（跳帧保速后两者不再相等） */
static void arcade_rate_tick(bool rendered)
{
    TickType_t now = xTaskGetTickCount();

    s_logic_frames++;
    if (rendered) {
        s_render_frames++;
    } else {
        s_skipped_frames++;
    }

    if (s_rate_tick == 0) {
        s_rate_tick = now;
        return;
    }

    if ((uint32_t)(now - s_rate_tick) >= pdMS_TO_TICKS(1000)) {
        uint32_t elapsed = (uint32_t)(now - s_rate_tick);

        s_logic_fps_x10 = (s_logic_frames * 10000U) / elapsed;
        s_render_fps_x10 = (s_render_frames * 10000U) / elapsed;
        s_logic_frames = 0;
        s_render_frames = 0;
        s_rate_tick = now;
    }
}

/* 这一帧要不要画？信用不够就跳（保留上一帧画面） */
static bool arcade_should_render(void)
{
    if (!s_skip_auto) {
        return true;
    }

    /*
     * 门槛取 render_extra 的 3/4：睡觉是按整 tick 向下取整的，留 1/4 的余量
     * 才不会每次睡完都刚好差一点、永远轮不到渲染（实测那会把渲染帧率压到个位数）。
     */
    return s_skip_credit_us >= (int32_t)((s_render_extra_us * 3U) / 4U);
}

/*
 * 信用账：每帧进账一个 GB 帧周期（16.74ms），花掉这一帧的实测耗时。
 * 同时用 EMA 学会「画」与「不画」各自的成本差，作为下一帧的准入门槛。
 */
static void arcade_skip_account(uint32_t cost_us, bool rendered)
{
    int32_t pace;
    uint32_t extra;

    if (cost_us < 2000U) {
        cost_us = 2000U;
    } else if (cost_us > 100000U) {
        /* 切页面 / 写卡那种长停帧不参与统计，否则 EMA 会被带飞 */
        return;
    }

    if (rendered) {
        s_render_cost_us = (s_render_cost_us * 7U + cost_us) / 8U;
    } else {
        s_skip_cost_us = (s_skip_cost_us * 7U + cost_us) / 8U;
    }

    extra = (s_render_cost_us > s_skip_cost_us)
                ? (s_render_cost_us - s_skip_cost_us) : 0U;
    s_render_extra_us = (s_render_extra_us * 3U + extra) / 4U;

    /* pace > 0 = 比 59.7Hz 节拍快，可以先花在渲染上；< 0 = 已经落后 */
    pace = s_skip_credit_us + (int32_t)ARCADE_GB_FRAME_US - (int32_t)cost_us;
    if (pace > (int32_t)(2U * ARCADE_GB_FRAME_US)) {
        pace = (int32_t)(2U * ARCADE_GB_FRAME_US);
    } else if (pace < -(int32_t)ARCADE_GB_FRAME_US) {
        pace = -(int32_t)ARCADE_GB_FRAME_US;
    }
    s_skip_credit_us = pace;
}

/*
 * 自动缩放：每帧量一次真实帧周期，持续不达标就降 1x。
 * 必须在 arcade 任务里调（arcade_video_set_scale 会排空送屏流水线）。
 */
static void arcade_auto_scale_tick(void)
{
    uint32_t now = arcade_cycles();
    uint32_t delta;

    if (s_last_frame_cyc == 0) {
        s_last_frame_cyc = now;
        return;
    }

    delta = arcade_cyc_to_us((uint32_t)(now - s_last_frame_cyc));
    s_last_frame_cyc = now;

    /* 切页面 / 载入 / 写盘那种几十上百毫秒的长停帧不参与统计 */
    if (delta > 200000U) {
        return;
    }

    s_frame_us = (s_frame_us * 7U + delta) / 8U;

    if (!s_scale_auto) {
        return;
    }

    /*
     * 跳帧保速下，渲染帧率已经被解耦出来 ——— 降分辨率换不来速度，只会变小画面，
     * 所以直接钉 1.5x（用户口径：速度优先，但画面能大就大）。
     */
    if (s_skip_auto) {
        if (arcade_video_scale() == 1) {
            s_scale_up_cnt = 0;
            arcade_video_set_scale(2);
        }
        return;
    }

    if (arcade_video_scale() != 1) {
        if (s_frame_us > SCALE_DOWN_US) {
            if (++s_scale_down_cnt >= SCALE_DOWN_FRAMES) {
                uint32_t avg = s_frame_us;

                s_scale_down_cnt = 0;
                s_scale_up_cnt = 0;
                s_frame_us = 0;
                arcade_video_set_scale(1);
                LISA_LOGW(TAG, "auto scale -> 1x (frame %u us, 跑不满 60fps)",
                          (unsigned)avg);
            }
        } else {
            s_scale_down_cnt = 0;
        }
        return;
    }

    if (s_frame_us < SCALE_UP_US) {
        if (++s_scale_up_cnt >= SCALE_UP_FRAMES) {
            s_scale_up_cnt = 0;
            s_frame_us = 0;
            arcade_video_set_scale(2);
            LISA_LOGI(TAG, "auto scale -> 1.5x (帧周期有余量)");
        }
    } else {
        s_scale_up_cnt = 0;
    }
}

static void arcade_task(void *arg)
{
    (void)arg;

    LISA_LOGI(TAG, "task started");

    if (arcade_video_init() != 0) {
        LISA_LOGE(TAG, "video init failed");
    }
    LISA_LOGI(TAG, "video ready");

    if (arcade_ui_init() != 0) {
        LISA_LOGE(TAG, "ui init failed");
    }
    LISA_LOGI(TAG, "ui ready");

    /* 音频链路（binjgb APU → DAC）：失败也只降级成静音，不影响玩游戏 */
    if (arcade_audio_init() != 0) {
        LISA_LOGW(TAG, "audio init failed, running silent");
    } else {
        LISA_LOGI(TAG, "audio ready");
    }

    arcade_ui_show_list();
    arcade_ui_list_set_hint("正在读取 TF 卡…");
    arcade_ui_list_refresh(0);
    lv_task_handler();

    LISA_LOGI(TAG, "scanning ROMs");
    arcade_rom_scan();
    arcade_ui_list_refresh(s_sel);
    LISA_LOGI(TAG, "list ready, %d ROM(s)", arcade_rom_count());

    for (;;) {
        TickType_t start = xTaskGetTickCount();
        uint32_t frame_t0 = arcade_cycles();
        uint32_t idle;

        if (s_pending_action != 0xFF) {
            uint8_t action = s_pending_action;
            s_pending_action = 0xFF;
            arcade_handle_action(action);
        }

        arcade_handle_request();

        /* 每 30 秒（有改动才写）把 cart RAM 落盘，写盘期间允许掉帧 */
        arcade_save_tick();

        if (s_mode == ARCADE_MODE_PLAY && !s_frozen) {
            bool kbd_exit = false;
            uint32_t kbd = arcade_input_take_keyboard(&kbd_exit);

            if (kbd_exit) {
                LISA_LOGI(TAG, "ESC from keyboard, back to ROM list");
                arcade_stop_game();
            } else {
                bool render = arcade_should_render();

                arcade_core_set_input(arcade_input_mask() | kbd);
                /*
                 * 逻辑每帧必跑；画面按预算选。跳过的帧 panel 上保留上一帧，
                 * 因为 present 也一并跳过了（不会重复送同一块内容）。
                 */
                arcade_gb_set_render_mode(render ? ARCADE_RENDER_FULL : s_skip_mech);
                arcade_core_run_frame();
                s_last_render = render;
                arcade_frame_tick();
                arcade_rate_tick(render);
                arcade_auto_scale_tick();
            }
        }

        /*
         * 游戏页的帧是 arcade_video 直接写面板的，LVGL 在这段时间没有任何
         * 东西要画。每帧调 lv_timer_handler() 纯属白花时间（它要跑定时器链表、
         * 判脏区、再调驱动的 flush），所以播放中降到每 8 帧一次；
         * 有暂停/提示文字盖在画面上时照旧每帧跑。
         */
        if (s_mode == ARCADE_MODE_PLAY && !s_frozen &&
            !arcade_ui_player_message_visible()) {
            if ((s_lvgl_skip++ & 7U) == 0) {
                idle = arcade_ui_handler();
            } else {
                idle = 0;
            }
        } else {
            idle = arcade_ui_handler();
        }

        TickType_t spent = xTaskGetTickCount() - start;
        uint32_t cost_us = arcade_cyc_to_us((uint32_t)(arcade_cycles() - frame_t0));

        /*
         * 只跟踪工作量（留给 `arcade period` 报告），不再拿它去抬步长。
         */
        if (s_mode == ARCADE_MODE_PLAY) {
            s_work_ema = (s_work_ema * 7U + (cost_us / 1000U)) / 8U;
            arcade_skip_account(cost_us, s_last_render);
        }

        frame_stat_push(spent);

        /*
         * 帧节拍：按 rdcycle 的微秒数算，而不是按 1ms 的 tick 算。
         * tick 粒度向下取整会把 16.74ms 的预算变成 16ms（62.5fps，音频偏快
         * 4.5%），四舍五入到最近的 tick 才能压在 59.7fps 附近。
         */
        uint32_t period = s_period_ms;
        uint32_t budget_us = (s_mode == ARCADE_MODE_PLAY && s_period_auto)
                                 ? ARCADE_GB_FRAME_US : (period * 1000U);

        if (s_mode != ARCADE_MODE_PLAY) {
            /* 不在玩游戏时把步长复位 */
            if (s_period_auto) {
                s_period_ms = CONFIG_ARCADE_FRAME_PERIOD_MS;
            }
            period = s_period_ms;
            budget_us = period * 1000U;
            if (idle > period) {
                budget_us = idle * 1000U;
            }
        }

        if (s_mode == ARCADE_MODE_PLAY) {
            /*
             * 跳帧保速的节拍控制：pace 是「领先于 59.7Hz 节拍的微秒数」。
             * 领先部分先留给渲染（所以阈值至少是 render_extra），剩下的才睡——
             * 这样既不会把渲染机会睡掉，也不会让逻辑跑快（逻辑快了音高就偏高）。
             */
            int32_t reserve = s_skip_auto ? (int32_t)((s_render_extra_us * 3U) / 4U) : 0;
            int32_t ahead = s_skip_credit_us - reserve;

            if (ahead > 0) {
                /* 向下取整到 tick：多睡就少一次渲染机会，少睡只是让 IDLE 少跑一会 */
                uint32_t wait_ms = (uint32_t)ahead / 1000U;

                if (wait_ms > 0) {
                    vTaskDelay(pdMS_TO_TICKS(wait_ms));
                    s_skip_credit_us -= (int32_t)(wait_ms * 1000U);
                }
                s_overrun_count = 0;
            } else if (++s_overrun_count >= 8U) {
                /* 一直超预算时也别把 IDLE 饿死：每 8 帧让一个 tick */
                s_overrun_count = 0;
                vTaskDelay(1);
                s_skip_credit_us -= 1000;
            } else {
                taskYIELD();
            }
        } else if (cost_us < budget_us) {
            /* 不在玩游戏（列表 / 菜单）：按固定周期填满，不参与跳帧控制 */
            uint32_t wait_ms = (budget_us - cost_us + 500U) / 1000U;

            if (wait_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
            }
        } else {
            taskYIELD();
        }

        boot_watchdog_feed();
    }
}

int arcade_app_main(void)
{
    BaseType_t ret;

    LISA_LOGI(TAG, "app main entered");

    /* main.c 提前返回，语音助手的初始化链路不会跑，按键需要自己拉起 */
    if (service_button_init() != 0) {
        LISA_LOGW(TAG, "button init failed");
    }
    LISA_LOGI(TAG, "button ready");

    /*
     * 必须自己拉起显示服务：面板（st7789p3）的 bus 是在
     * service_brightness_init() 里 attach 的，不跑这一步 display 的 priv->panel
     * 一直是 NULL，后面的 lv_port_disp_init() / blanking_on() 会直接解引用空
     * 指针（Load access fault, MTVAL = NULL + offset）。
     * 语音助手固件里这一步在 main.c 的 service_brightness_init()，模拟器分支
     * 提前 return 了，所以得在这里补上。
     */
    service_brightness_init();
    LISA_LOGI(TAG, "display service ready");

    voice_msg_sub(VOICE_MSG_BUTTON_CHANGE, arcade_button_cb, NULL);

    ret = xTaskCreate(arcade_task, "arcade", ARCADE_TASK_STACK, NULL, ARCADE_TASK_PRIORITY, NULL);
    if (ret != pdPASS) {
        LISA_LOGE(TAG, "create arcade task failed");
        /* 任务起不来也不能直接 return：返回会让 main 任务退出，系统静默变砖。
         * 这里保持看守狗并继续报活，至少串口能看出卡在哪一步。 */
    } else {
        LISA_LOGI(TAG, "arcade task created");
    }

    uint32_t ticks = 0;

    for (;;) {
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(200));

        if (++ticks % 25 == 0) {
            LISA_LOGI(TAG, "alive (main loop), mode=%d roms=%d", (int)s_mode, arcade_rom_count());
        }
    }

    return 0;
}

/* ------------- shell 调试命令 ------------- */

static int arcade_request(arcade_req_t req, int arg)
{
    int waited = 0;

    while (s_req != ARCADE_REQ_NONE && waited < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }

    s_req_arg = arg;
    s_req = req;

    /* 等主循环取走请求，最多 500 ms，让随后的 stat 能看到结果 */
    while (s_req != ARCADE_REQ_NONE && waited < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }

    return waited < 500 ? 0 : -1;
}

static int cmd_stat(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    (void)argc;
    (void)argv;

    shellPrint(shell, "mode=%s rom=%s sel=%d count=%d fps=%d.%d input=0x%02X\r\n",
               s_mode == ARCADE_MODE_LIST ? "list" : "play",
               arcade_core_running() ? (arcade_rom_name(s_sel) ? arcade_rom_name(s_sel) : "?") : "-",
               s_sel, arcade_rom_count(), (int)(s_fps_x10 / 10), (int)(s_fps_x10 % 10),
               (unsigned)arcade_input_mask());
    shellPrint(shell, "sys=%s scanned=%d dir=%s\r\n", arcade_core_sys_name(),
               arcade_rom_scanned() ? 1 : 0, CONFIG_ARCADE_ROM_DIR);
    shellPrint(shell, "save=%s dirty=%d\r\n",
               arcade_save_path() != NULL ? arcade_save_path() : "-",
               arcade_save_pending() ? 1 : 0);
    shellPrint(shell, "psram_free=%u KB\r\n",
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    {
        char line[160];

        frame_stat_report(line, sizeof(line));
        shellPrint(shell, "%s (period=%u ms%s)\r\n", line, (unsigned)s_period_ms,
                   s_period_auto ? ", auto" : ", fixed");
    }
    shellPrint(shell, "logic=%d.%d render=%d.%d fps skipped=%u skip=%s(%s)\r\n",
               (int)(s_logic_fps_x10 / 10), (int)(s_logic_fps_x10 % 10),
               (int)(s_render_fps_x10 / 10), (int)(s_render_fps_x10 % 10),
               (unsigned)s_skipped_frames,
               s_skip_auto ? "auto" : "off",
               s_skip_mech == ARCADE_RENDER_DUMMY ? "dummy" : "full");
    shellPrint(shell, "cost: render=%u.%ums skip=%u.%ums extra=%u.%ums credit=%d.%ums\r\n",
               (unsigned)(s_render_cost_us / 1000),
               (unsigned)((s_render_cost_us / 100) % 10),
               (unsigned)(s_skip_cost_us / 1000),
               (unsigned)((s_skip_cost_us / 100) % 10),
               (unsigned)(s_render_extra_us / 1000),
               (unsigned)((s_render_extra_us / 100) % 10),
               (int)(s_skip_credit_us / 1000),
               (int)((s_skip_credit_us / 100) % 10));
    return 0;
}

/*
 * arcade period [ms]：显示/钉住定步长的帧周期。
 *
 * 0 表示恢复自适应（默认）：慢就把步长抬上去，求恒定帧率。
 * 给定数值则固定成那个周期，方便对比「33ms 恒定」和「自适应」的观感。
 */
static int cmd_period(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc < 1) {
        shellPrint(shell, "period=%u ms (%s) work≈%u ms\r\n", (unsigned)s_period_ms,
                   s_period_auto ? "auto" : "fixed", (unsigned)s_work_ema);
        return 0;
    }

    int v = atoi(argv[0]);

    if (v <= 0) {
        s_period_auto = true;
        s_period_ms = CONFIG_ARCADE_FRAME_PERIOD_MS;
        s_work_ema = CONFIG_ARCADE_FRAME_PERIOD_MS;
        shellPrint(shell, "period: auto (%u ms 节拍)\r\n", (unsigned)s_period_ms);
        return 0;
    }

    if (v < 8) {
        v = 8;
    } else if (v > ARCADE_MAX_FRAME_PERIOD_MS) {
        v = ARCADE_MAX_FRAME_PERIOD_MS;
    }

    s_period_auto = false;
    s_period_ms = (uint32_t)v;
    shellPrint(shell, "period: 固定 %d ms (%d fps)\r\n", v, 1000 / v);
    return 0;
}

/*
 * arcade clock：量真实的核频。
 *
 * SystemCoreClock 在本 SoC 上只是个常量（system_RISCVN300.c 的
 * SystemCoreClockUpdate() 直接赋 SYSTEM_CLOCK），串口里打出来的 24MHz
 * 不是真实频率。用 rdcycle 在一个已知的 tick 间隔里数一下才是真的。
 */
static inline uint32_t arcade_rdcycle(void)
{
    uint32_t v;

    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static int cmd_clock(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    uint32_t c0;
    uint32_t c1;
    TickType_t t0;
    TickType_t t1;
    uint32_t ms;

    (void)argc;
    (void)argv;

    c0 = arcade_rdcycle();
    t0 = xTaskGetTickCount();
    vTaskDelay(pdMS_TO_TICKS(200));
    t1 = xTaskGetTickCount();
    c1 = arcade_rdcycle();

    ms = (uint32_t)(t1 - t0);
    if (ms == 0) {
        ms = 1;
    }

    shellPrint(shell, "wall=%u ms cycles=%u -> %u MHz (SystemCoreClock=%u MHz)\r\n",
               (unsigned)ms, (unsigned)(c1 - c0),
               (unsigned)((uint32_t)(c1 - c0) / (ms * 1000U)),
               (unsigned)(SystemCoreClock / 1000000U));
    return 0;
}

static int cmd_list(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    int count = arcade_rom_count();

    (void)argc;
    (void)argv;

    if (!arcade_rom_scanned()) {
        shellPrint(shell, "尚未扫描 TF 卡\r\n");
        return 0;
    }

    shellPrint(shell, "%s: %d 个 ROM\r\n", CONFIG_ARCADE_ROM_DIR, count);
    for (int i = 0; i < count; i++) {
        shellPrint(shell, "  [%02d] %s (%u bytes)\r\n", i,
                   arcade_rom_name(i) ? arcade_rom_name(i) : "?",
                   (unsigned)arcade_rom_size(i));
    }

    return 0;
}

static int cmd_rescan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    arcade_request(ARCADE_REQ_RESCAN, 0);
    shellPrint(shellGetCurrent(), "扫描完成: %d\r\n", arcade_rom_count());
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    int index;

    if (argc < 1) {
        shellPrint(shell, "用法: arcade play <序号>\r\n");
        return 0;
    }

    index = atoi(argv[0]);
    if (index < 0 || index >= arcade_rom_count()) {
        shellPrint(shell, "序号越界\r\n");
        return 0;
    }

    if (arcade_request(ARCADE_REQ_PLAY, index) != 0) {
        shellPrint(shell, "请求超时\r\n");
        return 0;
    }

    shellPrint(shell, "已载入 [%d] %s (%d)\r\n", index,
               arcade_rom_name(index) ? arcade_rom_name(index) : "?",
               (int)s_mode);
    return 0;
}

static int cmd_exit(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    arcade_request(ARCADE_REQ_EXIT, 0);
    shellPrint(shellGetCurrent(), "已返回列表\r\n");
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    uint32_t key;

    if (argc < 1) {
        shellPrint(shell, "用法: arcade key <up|down|left|right|a|b|start|select>\r\n");
        return 0;
    }

    key = arcade_input_key_from_name(argv[0]);
    if (key == 0) {
        shellPrint(shell, "未知按键: %s\r\n", argv[0]);
        return 0;
    }

    arcade_input_tap(key);
    shellPrint(shell, "注入按键 %s\r\n", arcade_input_name(key));
    return 0;
}

static int cmd_press(int argc, char **argv){
    Shell *shell = shellGetCurrent();
    uint32_t key;
    bool pressed;

    if (argc < 1) {
        shellPrint(shell, "用法: arcade press <up|down|left|right|a|b|start|select> [0|1]\r\n");
        return 0;
    }

    key = arcade_input_key_from_name(argv[0]);
    if (key == 0) {
        shellPrint(shell, "未知按键: %s\r\n", argv[0]);
        return 0;
    }

    pressed = (argc >= 2) ? (atoi(argv[1]) != 0) : true;
    arcade_input_hold(key, pressed);
    shellPrint(shell, "%s %s\r\n", pressed ? "按住" : "松开", arcade_input_name(key));
    return 0;
}

/*
 * TF 卡文件系统读写自检。
 *
 * 只做最小的完整回路：开目录 -> 查容量 -> 建目录 -> 写文件 -> 读回比对 -> 删掉。
 * 每一步都单独打印，卡只读 / 不挂载 / 写入失效时能直接看出断在哪一步。
 */
static int cmd_fs(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    struct lsfs_mount_t *mp = platform_sd_mount_get();
    struct lsfs_dir_t dir;
    struct lsfs_statvfs vfs;
    struct lsfs_file_t file;
    uint8_t out[ARCADE_FS_PROBE_LEN];
    uint8_t in[ARCADE_FS_PROBE_LEN];
    ssize_t got;
    ssize_t wrote;
    int rc;
    int fails = 0;

    (void)argc;
    (void)argv;

    shellPrint(shell, "挂载点: %s\r\n",
               (mp != NULL && mp->mnt_point != NULL) ? mp->mnt_point : "(未知)");

    /* 1. 目录能否打开 */
    lsfs_dir_t_init(&dir);
    rc = lsfs_opendir(&dir, CONFIG_ARCADE_ROM_DIR);
    if (rc != 0) {
        shellPrint(shell, "[FAIL] 打开目录 %s 失败 (%d)，TF 卡未挂载？\r\n",
                   CONFIG_ARCADE_ROM_DIR, rc);
        shellPrint(shell, "fs 自检结果: 中断\r\n");
        return 0;
    }
    lsfs_closedir(&dir);
    shellPrint(shell, "[ OK ] 目录可打开 %s\r\n", CONFIG_ARCADE_ROM_DIR);

    /* 2. 容量 */
    memset(&vfs, 0, sizeof(vfs));
    if (lsfs_statvfs(CONFIG_ARCADE_ROM_DIR, &vfs) == 0 && vfs.f_frsize > 0) {
        unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long avail = (unsigned long long)vfs.f_bfree * vfs.f_frsize;

        shellPrint(shell, "[ OK ] 总容量 %u KB，剩余 %u KB\r\n",
                   (unsigned)(total / 1024ULL), (unsigned)(avail / 1024ULL));
    } else {
        shellPrint(shell, "[WARN] 取容量失败（statvfs 不支持，不影响读写）\r\n");
    }

    /* 3. 建目录（已存在也算通过） */
    rc = lsfs_mkdir(ARCADE_FS_TEST_DIR);
    if (rc == 0) {
        shellPrint(shell, "[ OK ] 创建目录 %s\r\n", ARCADE_FS_TEST_DIR);
    } else {
        lsfs_dir_t_init(&dir);
        if (lsfs_opendir(&dir, ARCADE_FS_TEST_DIR) == 0) {
            lsfs_closedir(&dir);
            shellPrint(shell, "[ OK ] 目录已存在 %s\r\n", ARCADE_FS_TEST_DIR);
        } else {
            shellPrint(shell, "[FAIL] 创建目录失败 (%d)，卡可能是只读或已写满\r\n", rc);
            fails++;
        }
    }

    /* 4. 写测试文件 */
    if (fails == 0) {
        for (int i = 0; i < ARCADE_FS_PROBE_LEN; i++) {
            out[i] = (uint8_t)(i * 7 + 3);
        }

        lsfs_file_t_init(&file);
        rc = lsfs_open(&file, ARCADE_FS_TEST_FILE, LSFS_O_WRITE | LSFS_O_CREATE | LSFS_O_TRUNC);
        if (rc != 0) {
            shellPrint(shell, "[FAIL] 创建文件 %s 失败 (%d)\r\n", ARCADE_FS_TEST_FILE, rc);
            fails++;
        } else {
            wrote = lsfs_write(&file, out, sizeof(out));
            rc = lsfs_sync(&file);
            lsfs_close(&file);

            if (wrote == (ssize_t)sizeof(out)) {
                shellPrint(shell, "[ OK ] 写入 %d 字节 %s（sync=%d）\r\n",
                           (int)wrote, ARCADE_FS_TEST_FILE, rc);
            } else {
                shellPrint(shell, "[FAIL] 写入不足 %d/%d 字节\r\n",
                           (int)wrote, (int)sizeof(out));
                fails++;
            }
        }
    }

    /* 5. 读回比对 */
    if (fails == 0) {
        memset(in, 0, sizeof(in));
        lsfs_file_t_init(&file);
        rc = lsfs_open(&file, ARCADE_FS_TEST_FILE, LSFS_O_READ);
        if (rc != 0) {
            shellPrint(shell, "[FAIL] 读回打开失败 (%d)\r\n", rc);
            fails++;
        } else {
            got = lsfs_read(&file, in, sizeof(in));
            lsfs_close(&file);

            if (got == (ssize_t)sizeof(out) && memcmp(in, out, sizeof(out)) == 0) {
                shellPrint(shell, "[ OK ] 读回 %d 字节，内容一致\r\n", (int)got);
            } else {
                shellPrint(shell, "[FAIL] 读回不一致（%d 字节）\r\n", (int)got);
                fails++;
            }
        }
    }

    /* 6. 清理 */
    rc = lsfs_unlink(ARCADE_FS_TEST_FILE);
    shellPrint(shell, "[%s] 删除测试文件 (%d)\r\n", rc == 0 ? " OK " : "WARN", rc);
    rc = lsfs_unlink(ARCADE_FS_TEST_DIR);
    shellPrint(shell, "[%s] 删除测试目录 (%d)\r\n", rc == 0 ? " OK " : "WARN", rc);

    shellPrint(shell, "fs 自检结果: %s（%d 项失败）\r\n", fails == 0 ? "通过" : "失败", fails);
    return 0;
}

static int cmd_save(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    const char *path;

    (void)argc;
    (void)argv;

    if (!arcade_core_running()) {
        shellPrint(shell, "当前没有运行中的 ROM\r\n");
        return 0;
    }

    path = arcade_save_path();
    if (path == NULL) {
        shellPrint(shell, "当前 ROM 不支持存档（内置演示 ROM 或不在卡上）\r\n");
        return 0;
    }

    s_save_result = 0;
    if (arcade_request(ARCADE_REQ_SAVE, 0) != 0) {
        shellPrint(shell, "请求超时\r\n");
        return 0;
    }

    if (s_save_result > 0) {
        shellPrint(shell, "已落盘 %d 字节 -> %s\r\n", s_save_result, path);
    } else if (s_save_result == 0) {
        shellPrint(shell, "没有改动，无需落盘（%s）\r\n", path);
    } else {
        shellPrint(shell, "落盘失败 (%d)，下个周期重试\r\n", s_save_result);
    }

    return 0;
}

static int cmd_romsum(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    char header[96];

    (void)argc;
    (void)argv;

    if (!arcade_core_running()) {
        shellPrint(shell, "当前没有运行中的 ROM\r\n");
        return 0;
    }

    /* 走和模拟器完全相同的读路径算一遍哈希，用来确认 ROM 数据是完整的 */
    if (arcade_gb_rom_header(header, sizeof(header)) >= 0) {
        shellPrint(shell, "%s\r\n", header);
    }

    shellPrint(shell, "rom fnv1a=0x%08X\r\n", (unsigned)arcade_gb_rom_checksum());
    return 0;
}

/*
 * 列出存档：不做目录遍历（本平台 lsfs_readdir 在卡根目录会死锁），
 * 只对已知 ROM 逐个 lsfs_open 探测同名 .sav。
 */
static int cmd_saves(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    int found = 0;
    int count;

    (void)argc;
    (void)argv;

    shellPrint(shell, "存档与 ROM 同目录（*.sav）\r\n");
    count = arcade_rom_count();
    if (count > ARCADE_SAVES_MAX) {
        count = ARCADE_SAVES_MAX;
    }

    for (int i = 0; i < count; i++) {
        const char *rom = arcade_rom_path(i);
        struct lsfs_file_t file;
        char path[128];
        const char *dot;
        off_t size;
        int n;

        if (rom == NULL || rom[0] != '/') {
            continue;
        }

        dot = strrchr(rom, '.');
        n = snprintf(path, sizeof(path), "%.*s.sav",
                     (int)(dot != NULL ? (size_t)(dot - rom) : strlen(rom)), rom);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }

        lsfs_file_t_init(&file);
        if (lsfs_open(&file, path, LSFS_O_READ) != 0) {
            continue;
        }
        size = lsfs_lsize(&file);
        lsfs_close(&file);

        shellPrint(shell, "%s (%d bytes)\r\n", path, (int)size);
        found++;
    }

    shellPrint(shell, "共 %d 个存档\r\n", found);
    return 0;
}

/*
 * arcade fb            帧缓冲 / canvas / 刷屏链路状态
 * arcade fb fill <0-3> 冻结画面并把帧缓冲填成调色板第 n 档（直接看屏变没变）
 * arcade fb resume     恢复模拟
 */
static int cmd_fb(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc >= 1 && strcasecmp(argv[0], "fill") == 0) {
        int shade = (argc >= 2) ? atoi(argv[1]) : 0;

        if (shade < 0 || shade > 3) {
            shade = 0;
        }
        if (arcade_request(ARCADE_REQ_FB_FILL, shade) != 0) {
            shellPrint(shell, "请求超时（arcade 任务无响应）\r\n");
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
        shellPrint(shell, "已冻结并填充调色板 %d 档（期望屏幕整体变成 %04X），恢复用 arcade fb resume\r\n",
                   shade, (unsigned)arcade_video_palette_raw(shade));
        return 0;
    }

    if (argc >= 1 && strcasecmp(argv[0], "resume") == 0) {
        if (arcade_request(ARCADE_REQ_FB_RESUME, 0) != 0) {
            shellPrint(shell, "请求超时（arcade 任务无响应）\r\n");
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
        shellPrint(shell, "已恢复模拟\r\n");
        return 0;
    }

    s_fb_ready = false;
    if (arcade_request(ARCADE_REQ_FB_REPORT, 0) != 0) {
        shellPrint(shell, "请求超时（arcade 任务无响应）\r\n");
        return 0;
    }

    /* arcade_request 只能保证请求被取走，报告还要等主循环写完 */
    for (int waited = 0; !s_fb_ready && waited < 300; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    shellPrint(shell, "frozen=%d mode=%s\r\n%s\r\n",
               s_frozen ? 1 : 0,
               s_mode == ARCADE_MODE_LIST ? "list" : "play",
               s_fb_report);
    return 0;
}

static int cmd_cpu(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    char line[512];

    (void)argc;
    (void)argv;

    if (!arcade_core_running()) {
        shellPrint(shell, "当前没有运行中的 ROM\r\n");
        return 0;
    }

    for (int i = 0; i < 2; i++) {
        line[0] = '\0';
        if (arcade_gb_machine_report(line, sizeof(line)) < 0) {
            shellPrint(shell, "采样失败\r\n");
            return 0;
        }

        /* shell 的单次打印缓冲很有限，逐行输出才不会把后半截切断 */
        for (char *p = line; p != NULL && *p != '\0';) {
            char *nl = strchr(p, '\n');

            if (nl != NULL) {
                *nl = '\0';
            }
            shellPrint(shell, "%s\r\n", p);
            p = (nl != NULL) ? nl + 1 : NULL;
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }

    return 0;
}

static int cmd_peek(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    char line[1024];
    uint32_t addr;
    uint32_t len;

    if (argc < 1) {
        shellPrint(shell, "用法: arcade peek <addr(hex)> [len]\r\n");
        return 0;
    }

    addr = (uint32_t)strtoul(argv[0], NULL, 16);
    len = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 10) : 64;

    if (!arcade_core_running()) {
        shellPrint(shell, "当前没有运行中的 ROM\r\n");
        return 0;
    }

    line[0] = '\0';
    if (arcade_gb_peek(line, sizeof(line), addr, len) < 0) {
        shellPrint(shell, "读取失败\r\n");
        return 0;
    }

    for (char *p = line; p != NULL && *p != '\0';) {
        char *nl = strchr(p, '\n');

        if (nl != NULL) {
            *nl = '\0';
        }
        shellPrint(shell, "%s\r\n", p);
        p = (nl != NULL) ? nl + 1 : NULL;
    }

    return 0;
}

/*
 * arcade shot：把当前帧缓冲打成 80x36 的 ASCII 图。
 *
 * 「屏幕看起来卡住」时，先看这一张图就能分清是黑屏、白屏、某个画面，
 * 还是屏上已经不是游戏画面了。格宽 3px、高 6px 是按终端字符 1:2 的长宽比
 * 挑的，刚好保持 GB 160x144 的比例。
 */
static int cmd_shot(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    lv_color_t *fb = arcade_video_get_fb();
    static const char k_ch[4] = {' ', '.', ':', '#'};
    char line[96];
    int fb_w = arcade_video_fb_width();
    int fb_h = arcade_video_fb_height();

    (void)argc;
    (void)argv;

    if (fb == NULL) {
        shellPrint(shell, "fb=(null)\r\n");
        return 0;
    }

    for (int row = 0; row < 36; row++) {
        int off = 0;
        int y0 = row * (fb_h / 36);
        int dy = fb_h / 36;

        for (int col = 0; col < 80; col++) {
            int x0 = col * (fb_w / 80);
            int dx = fb_w / 80;
            int lum = 0;
            int n = 0;

            for (int y = y0; y < y0 + dy; y++) {
                for (int x = x0; x < x0 + dx; x++) {
                    uint16_t v = fb[y * fb_w + x].full;
                    /*
                     * 不能只比对 DMG 四档颜色：CGB 游戏的配色全来自游戏自己，
                     * 匹配不上就会整屏空白（之前就这样）。直接算亮度。
                     */
                    int r = (v >> 11) & 0x1F;
                    int g = (v >> 5) & 0x3F;
                    int b = v & 0x1F;

                    lum += (r * 30 + g * 59 + b * 11) / 100;
                    n++;
                }
            }

            if (n > 0) {
                lum /= n;   /* 0..63 */
            }
            line[off++] = k_ch[lum / 16];
        }

        line[off] = '\0';
        shellPrint(shell, "%s\r\n", line);
    }

    return 0;
}

/* arcade pc：帧边界 PC 直方图 */
static int cmd_pc(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    char line[1024];

    (void)argc;
    (void)argv;

    if (!arcade_core_running()) {
        shellPrint(shell, "当前没有运行中的 ROM\r\n");
        return 0;
    }

    line[0] = '\0';
    if (arcade_gb_hist_report(line, sizeof(line)) < 0) {
        shellPrint(shell, "采样失败\r\n");
        return 0;
    }

    for (char *p = line; p != NULL && *p != '\0';) {
        char *nl = strchr(p, '\n');

        if (nl != NULL) {
            *nl = '\0';
        }
        shellPrint(shell, "%s\r\n", p);
        p = (nl != NULL) ? nl + 1 : NULL;
    }

    return 0;
}

/* arcade audio：音频输出状态（peak 看波形，reset 清计数） */
static int cmd_audio(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    char line[512];

    (void)argc;

    if (argc >= 1 && strcasecmp(argv[0], "reset") == 0) {
        arcade_audio_reset_stats();
        shellPrint(shell, "音频统计已清零\r\n");
        return 0;
    }

    line[0] = '\0';
    if (argc >= 1 && strcasecmp(argv[0], "peak") == 0) {
        if (arcade_audio_peak_report(line, sizeof(line)) < 0) {
            shellPrint(shell, "音频未初始化\r\n");
            return 0;
        }
    } else if (arcade_audio_report(line, sizeof(line)) < 0) {
        shellPrint(shell, "音频未初始化\r\n");
        return 0;
    }

    for (char *p = line; p != NULL && *p != '\0';) {
        char *nl = strchr(p, '\n');

        if (nl != NULL) {
            *nl = '\0';
        }
        shellPrint(shell, "%s\r\n", p);
        p = (nl != NULL) ? nl + 1 : NULL;
    }

    return 0;
}

/* arcade vol <0-100>：调音量并保存到 KV */
static int cmd_vol(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    int vol;

    if (argc < 1) {
        shellPrint(shell, "当前音量: %u%%（用法: arcade vol <0-100>）\r\n",
                   (unsigned)arcade_audio_volume());
        return 0;
    }

    vol = atoi(argv[0]);
    if (vol < 0) {
        vol = 0;
    } else if (vol > 100) {
        vol = 100;
    }

    arcade_audio_set_volume((uint32_t)vol);
    shellPrint(shell, "音量: %d%%\r\n", vol);
    return 0;
}

/* arcade perf [reset]：帧率与各段耗时（emu / blit / 等缓冲 / SPI 送屏） */
static int cmd_perf(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    char line[512];

    if (argc >= 1 && strcasecmp(argv[0], "reset") == 0) {
        arcade_video_reset_perf();
        arcade_gb_reset_perf();
        shellPrint(shell, "计时已清零\r\n");
        return 0;
    }

    line[0] = '\0';
    arcade_video_perf(line, sizeof(line));
    shellPrint(shell, "fps=%d.%d mode=%s sysclk=%u MHz scale=%s %s frame≈%u.%ums\r\n",
               (int)(s_fps_x10 / 10), (int)(s_fps_x10 % 10),
               s_mode == ARCADE_MODE_LIST ? "list" : "play",
               (unsigned)(SystemCoreClock / 1000000U),
               arcade_video_scale() == 1 ? "1x" : "1.5x",
               s_scale_auto ? "auto" : "manual",
               (unsigned)(s_frame_us / 1000), (unsigned)((s_frame_us / 100) % 10));
    shellPrint(shell, "logic=%d.%d render=%d.%d fps skip=%s(%s) render≈%u.%ums skip≈%u.%ums\r\n",
               (int)(s_logic_fps_x10 / 10), (int)(s_logic_fps_x10 % 10),
               (int)(s_render_fps_x10 / 10), (int)(s_render_fps_x10 % 10),
               s_skip_auto ? "auto" : "off",
               s_skip_mech == ARCADE_RENDER_DUMMY ? "dummy" : "full",
               (unsigned)(s_render_cost_us / 1000),
               (unsigned)((s_render_cost_us / 100) % 10),
               (unsigned)(s_skip_cost_us / 1000),
               (unsigned)((s_skip_cost_us / 100) % 10));

    for (char *p = line; p != NULL && *p != '\0';) {
        char *nl = strchr(p, '\n');

        if (nl != NULL) {
            *nl = '\0';
        }
        shellPrint(shell, "%s\r\n", p);
        p = (nl != NULL) ? nl + 1 : NULL;
    }

    return 0;
}

/* arcade scale <1|15|auto>：1 = 160x144，15 = 1.5 倍 240x216，auto = 自动 */
static int cmd_scale(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc >= 1) {
        if (strcasecmp(argv[0], "auto") == 0) {
            s_scale_auto = true;
            s_frame_us = 0;
            s_scale_down_cnt = 0;
            s_scale_up_cnt = 0;
            shellPrint(shell, "缩放: 自动（优先 1.5x，帧率不够就掉 1x）\r\n");
            return 0;
        }

        s_scale_auto = false;
        arcade_video_set_scale(atoi(argv[0]) == 1 ? 1 : 2);
    }

    shellPrint(shell, "缩放: %s %s（%dx%d）  frame≈%u.%ums\r\n",
               arcade_video_scale() == 1 ? "1x" : "1.5x",
               s_scale_auto ? "auto" : "manual",
               arcade_video_fb_width(), arcade_video_fb_height(),
               (unsigned)(s_frame_us / 1000), (unsigned)((s_frame_us / 100) % 10));
    return 0;
}

/*
 * arcade roty [px]：180° 硬件翻转的行偏移覆盖（调试）
 *
 * 面板用 MADCTL MY|MX 做整块显存的镜像，行偏移补偿是推算出来的
 * （显存行数 - 玻璃行数 = 80）。万一实际玻璃位置不同，可以用这条命令在
 * 不重新烧录的情况下当场试：roty 0 / roty 80 / roty -1（恢复自动）。
 */
static int cmd_roty(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc >= 1) {
        arcade_video_set_rotate_row_shift((int16_t)atoi(argv[0]));
    }

    shellPrint(shell, "rotate row shift = %d（-1 = 自动）\r\n",
               (int)arcade_video_rotate_row_shift());
    return 0;
}

/*
 * arcade spi [MHz]：运行期改面板 SPI 时钟（测上限 / 提升送屏带宽用）。
 *
 * 改频动作本身必须挖到模拟器任务的空闲窗口里做（在途 DMA 会让 SPI_Control
 * 回 BUSY），所以这里只投递请求、在 arcade_handle_request() 里执行。
 * 改完之后用 `arcade perf` 的 txcache（MB/s）看实际效果。
 */
static int cmd_spi(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    uint32_t cur = arcade_video_spi_freq();
    int mhz;

    if (argc < 1) {
        shellPrint(shell, "spi=%u kHz（实测带宽看 arcade perf 的 txcache）\r\n",
                   (unsigned)(cur / 1000U));
        return 0;
    }

    mhz = atoi(argv[0]);
    if (mhz < 1 || mhz > 200) {
        shellPrint(shell, "MHz 取值范围 1~200\r\n");
        return 0;
    }

    s_spi_result = 0x7FFFFFFF;
    s_spi_freq_req = (uint32_t)mhz * 1000000U;
    s_req_arg = mhz;
    s_req = ARCADE_REQ_SPI;

    /* 等模拟器任务把改频做完（最多 1s） */
    for (int i = 0; i < 100 && s_spi_result == 0x7FFFFFFF; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_spi_result == 0x7FFFFFFF) {
        shellPrint(shell, "改频超时（模拟器任务没响应）\r\n");
        return 0;
    }

    if (s_spi_result != 0) {
        shellPrint(shell, "改 %d MHz 失败（ret=%d），仍为 %u kHz\r\n", mhz,
                   s_spi_result, (unsigned)(arcade_video_spi_freq() / 1000U));
        return 0;
    }

    shellPrint(shell, "display spi = %u kHz，跑几秒后 arcade perf 看 txcache\r\n",
               (unsigned)(arcade_video_spi_freq() / 1000U));
    return 0;
}

/* arcade apu <0|1>：开关模拟器 APU（用来量声音占多少时间） */
static int cmd_apu(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (!arcade_core_running()) {
        shellPrint(shell, "当前没有运行中的 ROM\r\n");
        return 0;
    }

    if (argc < 1) {
        shellPrint(shell, "用法: arcade apu <0|1>\r\n");
        return 0;
    }

    arcade_gb_set_audio_enabled(atoi(argv[0]) != 0);
    /* 计时清零，跑几秒再 arcade perf 看 emu 耗时变化 */
    arcade_video_reset_perf();
    arcade_gb_reset_perf();
    shellPrint(shell, "APU %s（计时已清零，等几秒 后 arcade perf）\r\n",
               atoi(argv[0]) != 0 ? "on" : "off");
    return 0;
}

/*
 * arcade bg <0|1>：只关 PPU 像素渲染（游戏逻辑照跑）。
 *
 * 把 emu 耗时拆成两半：「画像素」vs 「CPU 解释器 + 每 tick 状态机」。
 * bg 0 后 emu 少掉的那部分就是 PPU 渲染成本，剩下的是 CPU 侧。
 * 现在是 `arcade skip` 的快捷方式：0 = 强制全程跳像素（等于跳帧保速的
 * 全跳模式），1 = 交回自动调度。
 */
static int cmd_bg(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (!arcade_core_running()) {
        shellPrint(shell, "当前没有运行中的 ROM\r\n");
        return 0;
    }

    if (argc < 1) {
        shellPrint(shell, "用法: arcade bg <0|1>\r\n");
        return 0;
    }

    if (atoi(argv[0]) != 0) {
        s_skip_auto = true;
        shellPrint(shell, "PPU 渲染恢复（跳帧保速 auto）\r\n");
    } else {
        s_skip_auto = false;
        arcade_gb_set_render_mode(ARCADE_RENDER_SKIP);
        shellPrint(shell, "PPU 像素渲染 off（逻辑/音频照跑，等几秒 arcade cpu 看 emu）\r\n");
    }

    if (s_skip_auto) {
        arcade_gb_set_render_mode(ARCADE_RENDER_FULL);
    }

    arcade_video_reset_perf();
    arcade_gb_reset_perf();
    return 0;
}

/*
 * arcade skip <0|auto|full|dummy>：跳帧保速（逻辑/音频恒 59.7Hz，画面按需少画）。
 *
 *   0     关闭：每帧都渲染（帧率会被 PPU 拖到 ~55fps，音频跟着变慢）
 *   auto  默认：按帧预算挑着渲染（约 2/3 帧，30~40fps）
 *   full  跳过时连像素都不算（默认实现）
 *   dummy 跳过时照算像素但不写显存（PPU 时序逐 tick 一致，用来对比）
 */
static int cmd_skip(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc >= 1) {
        if (strcasecmp(argv[0], "0") == 0 || strcasecmp(argv[0], "off") == 0) {
            s_skip_auto = false;
            arcade_gb_set_render_mode(ARCADE_RENDER_FULL);
        } else if (strcasecmp(argv[0], "auto") == 0) {
            s_skip_auto = true;
        } else if (strcasecmp(argv[0], "full") == 0) {
            s_skip_auto = true;
            s_skip_mech = ARCADE_RENDER_SKIP;
        } else if (strcasecmp(argv[0], "dummy") == 0) {
            s_skip_auto = true;
            s_skip_mech = ARCADE_RENDER_DUMMY;
        } else {
            shellPrint(shell, "用法: arcade skip <0|auto|full|dummy>\r\n");
            return 0;
        }

        s_skip_credit_us = 0;
    }

    shellPrint(shell, "skip=%s 方式=%s  render≈%u.%ums skip≈%u.%ums extra≈%u.%ums\r\n",
               s_skip_auto ? "auto" : "off",
               s_skip_mech == ARCADE_RENDER_DUMMY ? "dummy" : "full",
               (unsigned)(s_render_cost_us / 1000),
               (unsigned)((s_render_cost_us / 100) % 10),
               (unsigned)(s_skip_cost_us / 1000),
               (unsigned)((s_skip_cost_us / 100) % 10),
               (unsigned)(s_render_extra_us / 1000),
               (unsigned)((s_render_extra_us / 100) % 10));
    return 0;
}

/*
 * arcade pwrho1d <0|1>：用 AON IOMUX 的 force-output 把 POWER_EN 锁到高。
 *
 * 为什么需要：这台机器是电池供电，POWER_EN 是电源自锁的栅极。cskburn 刷完
 * 会复位芯片，复位窗口里 PB3 没有驱动、自锁撑不住，设备就掉电了 —— 所以每刷
 * 一次都得人工长按开机。先把它锁到高，复位过程 AON 域一直硬拉高，刷完
 * 设备还能自己起来（开发期省事用，正式固件不该调）。
 *
 * pwrho1d 0 把 force 位交还给常规 GPIO 驱动。
 */
static int cmd_pwrho1d(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    int on = (argc >= 1) ? (atoi(argv[0]) != 0) : 1;
    volatile uint32_t *aon =
        (volatile uint32_t *)&IP_AON_IOMUX->REG_PAD_AON_GPIOB_00.all + POWER_EN_PIN;
    uint32_t before = *aon;

    /* bit22 OEN_FRC / bit23 OUT_REG / bit24 OUT_FRC：强制输出高 */
    if (on) {
        *aon &= ~(0x1E00000u);
        *aon |= 0x1C00000u;
    } else {
        *aon &= ~(0x1C00000u);
    }

    shellPrint(shell, "POWER_EN(PB%d) AON force %s  reg=%08X -> %08X\r\n",
               POWER_EN_PIN, on ? "ON（复位期间保持供电）" : "OFF（交还 GPIO）",
               (unsigned)before, (unsigned)*aon);
    return 0;
}

/*
 * arcade membench：量 PSRAM / 内部 SRAM 的访存带宽。
 *
 * 模拟器慢到底是「算得慢」还是「访存慢」，先看这两个数：
 * 如果 PSRAM 顺序读只有几十 MB/s，那 Emulator（近 300KB 常驻 PSRAM）
 * 的每个字段访问都在付这个代价，优化方向就是搬内存而不是改指令。
 */
/* 内部 SRAM 读的样本：1KB 足够看出「缓存命中时的纯执行速度」
 * （内部 SRAM 只抢得到很少一点，不能为了调试占掉模拟器的口粮） */
static uint8_t s_membench_sram[1024];

static int cmd_membench(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    const size_t psram_size = 256 * 1024;
    const int iter = 16;
    volatile uint32_t sum = 0;
    uint8_t *ps;
    TickType_t t0;
    TickType_t t1;
    uint32_t ms;

    (void)argc;
    (void)argv;

    ps = (uint8_t *)exram_malloc(4, psram_size);
    if (ps == NULL) {
        shellPrint(shell, "PSRAM 分配失败\r\n");
        return 0;
    }

    memset(ps, 0x5A, psram_size);
    memset(s_membench_sram, 0x5A, sizeof(s_membench_sram));

    /* PSRAM 顺序读：步长 64B，每步一定 Cache 不命中 */
    t0 = xTaskGetTickCount();
    for (int k = 0; k < iter; k++) {
        for (size_t i = 0; i < psram_size; i += 64) {
            sum += ps[i];
        }
    }
    t1 = xTaskGetTickCount();
    ms = (uint32_t)(t1 - t0);
    shellPrint(shell, "psram read : %u ms, %u MB/s\r\n",
               (unsigned)ms,
               (unsigned)(ms ? ((uint32_t)psram_size * iter / 1024) / ms : 0));

    /* PSRAM 写 */
    t0 = xTaskGetTickCount();
    for (int k = 0; k < iter; k++) {
        for (size_t i = 0; i < psram_size; i += 64) {
            ps[i] = (uint8_t)(i + k);
        }
    }
    t1 = xTaskGetTickCount();
    ms = (uint32_t)(t1 - t0);
    shellPrint(shell, "psram write: %u ms, %u MB/s\r\n",
               (unsigned)ms,
               (unsigned)(ms ? ((uint32_t)psram_size * iter / 1024) / ms : 0));

    /* 内部 SRAM 读（8KB 装得下，基本是纯执行速度） */
    t0 = xTaskGetTickCount();
    for (int k = 0; k < iter * 32; k++) {
        for (size_t i = 0; i < sizeof(s_membench_sram); i += 64) {
            sum += s_membench_sram[i];
        }
    }
    t1 = xTaskGetTickCount();
    ms = (uint32_t)(t1 - t0);
    shellPrint(shell, "sram read  : %u ms, %u MB/s\r\n",
               (unsigned)ms,
               (unsigned)(ms ? ((uint32_t)sizeof(s_membench_sram) * iter * 32 / 1024) / ms : 0));

    shellPrint(shell, "sysclk=%u MHz sum=%u\r\n",
               (unsigned)(SystemCoreClock / 1000000U), (unsigned)sum);

    exram_free(ps);
    return 0;
}

/*
 * arcade pad：USB 手柄（HID 非 boot 设备）的原始输入报告与解析结果。
 *
 * 手柄没有统一的报告布局，所以先把原始字节打出来，按一下键再跑一次，
 * 看哪一位变了就能定下映射（arcade_input.c 里的 pad_mask_*）。
 */
static int cmd_pad(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    uint8_t raw[24];
    char hex[(sizeof(raw) * 3) + 1];
    size_t off = 0;
    int len = 0;
    bool xinput = false;
    uint32_t n;
    uint32_t mask;

    (void)argc;
    (void)argv;

    n = arcade_input_pad_info(raw, (int)sizeof(raw), &len, &xinput);
    mask = arcade_input_mask();

    hex[0] = '\0';
    for (int i = 0; i < len && off < sizeof(hex); i++) {
        off += (size_t)snprintf(&hex[off], sizeof(hex) - off, "%02X%s",
                                raw[i], i + 1 == len ? "" : " ");
    }

    shellPrint(shell, "reports=%u len=%d layout=%s\r\n",
               (unsigned)n, len, xinput ? "xinput" : "generic");
    shellPrint(shell, "raw=%s\r\n", len > 0 ? hex : "(无，检查 USB 是否 host 模式/手柄是否已插入)");
    shellPrint(shell, "mask=0x%02x%s%s%s%s%s%s%s%s\r\n", (unsigned)mask,
               (mask & ARCADE_KEY_UP) ? " up" : "",
               (mask & ARCADE_KEY_DOWN) ? " down" : "",
               (mask & ARCADE_KEY_LEFT) ? " left" : "",
               (mask & ARCADE_KEY_RIGHT) ? " right" : "",
               (mask & ARCADE_KEY_A) ? " a" : "",
               (mask & ARCADE_KEY_B) ? " b" : "",
               (mask & ARCADE_KEY_START) ? " start" : "",
               (mask & ARCADE_KEY_SELECT) ? " select" : "");

    return 0;
}

static const struct listen_cmd_t g_arcade_cmds[] = {
    {"stat", cmd_stat, "打印模拟器状态与帧率"},
    {"list", cmd_list, "列出 TF 卡中的 ROM"},
    {"rescan", cmd_rescan, "重新扫描 TF 卡"},
    {"play", cmd_play, "载入指定序号的 ROM"},
    {"exit", cmd_exit, "退出游戏返回列表"},
    {"key", cmd_key, "注入一次按键"},
    {"press", cmd_press, "按住/松开某个按键"},
    {"pad", cmd_pad, "USB 手柄状态与原始报告（定位按键位序）"},
    {"fs", cmd_fs, "TF 卡文件系统读写自检"},
    {"save", cmd_save, "立即落盘当前存档"},
    {"saves", cmd_saves, "列出 TF 卡上的 .sav 存档"},
    {"romsum", cmd_romsum, "打印 ROM 头部与整份校验和"},
    {"fb", cmd_fb, "帧缓冲/刷屏自检（fill <0-3> 填纯色, resume 恢复）"},
    {"cpu", cmd_cpu, "打印 CPU/LCDC 寄存器快照（连打两次看 PC 是否在走）"},
    {"pc", cmd_pc, "帧边界 PC/SP 直方图（判断 CPU 卡在哪个循环）"},
    {"shot", cmd_shot, "把当前帧缓冲打成 80x36 ASCII 图（看屏上到底是什么）"},
    {"peek", cmd_peek, "按 CPU 视角读内存：peek <hex地址> [长度]"},
    {"audio", cmd_audio, "音频状态：audio [peak|reset]"},
    {"vol", cmd_vol, "音量：vol <0-100>"},
    {"perf", cmd_perf, "帧率与各段耗时：perf [reset]"},
    {"period", cmd_period, "帧节拍：period [ms]（0 = 回到固定节拍）"},
    {"clock", cmd_clock, "量真实核频（mcycle）"},
    {"scale", cmd_scale, "缩放：scale <1|15|auto>（auto = 帧率不够自动掉 1x）"},
    {"roty", cmd_roty, "180° 硬件翻转的行偏移：roty [px]（-1 = 自动）"},
    {"spi", cmd_spi, "面板 SPI 时钟：spi [MHz]（改完钉 arcade perf 看带宽）"},
    {"apu", cmd_apu, "开关模拟器 APU：apu <0|1>（量声音耗时）"},
    {"bg", cmd_bg, "开关 PPU 像素渲染：bg <0|1>（= skip 快捷方式，量画像素耗时）"},
    {"skip", cmd_skip, "跳帧保速：skip <0|auto|full|dummy>（逻辑/音频恒 59.7Hz）"},
    {"pwrho1d", cmd_pwrho1d, "把 POWER_EN 锁到高：pwrho1d <0|1>（刷机调试用）"},
    {"membench", cmd_membench, "量 PSRAM / SRAM 带宽"},
};

static int cmd_arcade_help(Shell *shell)
{
    for (size_t i = 0; i < sizeof(g_arcade_cmds) / sizeof(g_arcade_cmds[0]); i++) {
        shellPrint(shell, "%-10s\t:\t%s\r\n", g_arcade_cmds[i].name, g_arcade_cmds[i].help);
    }
    return 0;
}

static int arcade_cmd_handler(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc <= 1) {
        return cmd_arcade_help(shell);
    }

    for (size_t i = 0; i < sizeof(g_arcade_cmds) / sizeof(g_arcade_cmds[0]); i++) {
        if (strcmp(g_arcade_cmds[i].name, argv[1]) == 0) {
            return g_arcade_cmds[i].exec(argc - 2, argv + 2);
        }
    }

    return cmd_arcade_help(shell);
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 arcade,
                 arcade_cmd_handler,
                 复古模拟器调试命令);
