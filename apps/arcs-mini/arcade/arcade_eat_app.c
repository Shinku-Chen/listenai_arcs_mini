/*
 * 单应用固件入口（CONFIG_ARCADE_APP_EATONLY=y）：「今天吃啥」开机即用。
 *
 * 和全功能固件（arcade_app.c）的区别：
 *   - 没有 ROM 列表页、没有 GB/GBC 模拟器、没有小恐龙/跳一跳，
 *     这些源码在 CMake 里直接不参与编译（cmake/arcade/CMakeLists.txt）；
 *   - 开机直接进入「今天吃啥」：按住滚动、松手定格，两组素材严格交替；
 *   - 长按不退出、不关机（app_button.c 的关机路径需要 app_button_init()，
 *     这个固件根本不调它）；
 *   - 调试命令精简成 arcade stat / arcade eat [...]
 *
 * 显示链路和全功能固件保持一致：service_brightness_init() attach 面板 →
 * LVGL 只做一次 bring-up（拿 display 的 draw buffer + 唤醒屏幕）→
 * 之后完全由 arcade_video 的送屏任务直写面板，LVGL 不再参与绘制。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "esp_heap_caps.h"

#include "lisa_device.h"
#include "lisa_display.h"
#include "lisa_log.h"
#include "shell.h"
#include "cmd/cmd.h"
#include "sys_init.h"

#include "lvgl.h"
#include "lv_port_disp.h"

#include "voice_msg.h"
#include "service_button.h"
#include "service_brightness.h"

#include "arcade_app.h"
#include "arcade_core.h"
#include "arcade_eat.h"
#include "arcade_video.h"

/* POWER_EN 的 AON 强制输出位（`arcade pwrho1d`，开发期刷机用） */
#include "arcs_ap.h"
#include "pinmux.h"

#define TAG "eat"

#define EAT_TASK_STACK     4096
#define EAT_TASK_PRIORITY  4

/* 逻辑节拍：和全功能固件同一条 GB 帧预算（16.743ms ≈ 59.7Hz） */
#define EAT_FRAME_US       16743U

extern int boot_watchdog_feed(void);

static uint32_t s_next_start_cyc;
static uint32_t s_behind_cnt;
static uint32_t s_logic_frames;
static uint32_t s_render_frames;
static uint32_t s_logic_fps_x10;
static uint32_t s_render_fps_x10;
static TickType_t s_rate_tick;

/* 调试用：原始按键事件计数（证明「按下」确实在多击窗口之前就到了） */
static volatile uint32_t s_raw_cnt;
static volatile uint8_t s_raw_last = 0xFF;

/* ------------- 按键 ------------- */

/*
 * 原始按键事件（按下 / 松开）。
 *
 * 走 service_button 的 raw 回调而不是 voice_msg 的 CLICK：那颗键要区分
 * 单击/双击/…/长按，必须等多击判定窗口（几百毫秒）超时才算完，
 * 「按住滚动、松手定格」用它会明显发滞。原始事件在按下/松开那一刻就到。
 *
 * 回调跑在按键扫描任务（20ms 一轮）里，这里只落标志。
 * LONG_HOLD（1600ms）故意忽略：这颗键在这个固件里只负责抽签，按住多久都继续滚。
 */
static void eat_raw_button_cb(uint8_t btn_id, uint8_t event, void *user)
{
    (void)btn_id;
    (void)user;

    s_raw_cnt++;
    s_raw_last = event;

    switch ((voice_msg_button_action_t)event) {
    case VOICE_MSG_BUTTON_ACTION_PRESS_DOWN:
        arcade_eat_press();
        break;
    /* 松手（短按松开 / 长按松开 / 长按保持后松开）立刻定格 */
    case VOICE_MSG_BUTTON_ACTION_SHORT_UP:
    case VOICE_MSG_BUTTON_ACTION_LONG_UP:
    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD_UP:
        arcade_eat_release();
        break;
    default:
        break;
    }
}

/* ------------- 显示 ------------- */

/*
 * 面板唤醒。
 *
 * service_brightness_init() 已经把 SPI/GPIO attach 到 display 上，并把面板
 * 送进 sleep（blanking_on）；这里再把它唤醒。lv_port_disp_init() 顺手拿到
 * LVGL 的 draw buffer —— 本固件不用 LVGL 画东西，但它同时也是「屏幕已经
 * 就绪」的既有初始化路径，保持和全功能固件一致，省得漏掉板级细节。
 */
static void eat_display_wakeup(void)
{
    lisa_device_t *display = lisa_device_get("display");

    if (display == NULL || !lisa_device_ready(display)) {
        LISA_LOGE(TAG, "display device not ready");
        return;
    }

    lv_init();
    lv_port_disp_init(display);

    if (lisa_display_blanking_on(display) != 0) {
        LISA_LOGW(TAG, "blanking on failed");
    }
    lv_task_handler();

    if (lisa_display_blanking_off(display) != 0) {
        LISA_LOGE(TAG, "blanking off failed, screen may stay dark");
    }
}

/* ------------- 主循环 ------------- */

static void eat_rate_tick(bool rendered)
{
    TickType_t now = xTaskGetTickCount();

    s_logic_frames++;
    if (rendered) {
        s_render_frames++;
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

static void eat_app_task(void *arg)
{
    (void)arg;

    LISA_LOGI(TAG, "task started");

    if (arcade_video_init() != 0) {
        LISA_LOGE(TAG, "video init failed");
    }

    arcade_video_use_fullscreen(true);
    arcade_eat_enter();
    LISA_LOGI(TAG, "eat entered");

    s_next_start_cyc = arcade_cycles();
    s_behind_cnt = 0;
    s_rate_tick = 0;

    for (;;) {
        uint32_t rendered_before = arcade_eat_renders();

        arcade_eat_tick();
        eat_rate_tick(arcade_eat_renders() != rendered_before);

        /*
         * 固定节拍：按 rdcycle 的绝对帧起点锁 16.743ms。
         * 用绝对时间而不是「累计信用」：tick 只有 1ms 粒度，睡 16 还是 17ms
         * 都会让逻辑偏离 59.7Hz；绝对节拍下睡少的部分下一帧自动补回来。
         */
        int32_t remain_us = (int32_t)((int32_t)(s_next_start_cyc - arcade_cycles()) /
                                      (int32_t)ARCADE_CYC_PER_US);

        if (remain_us >= 1000) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)remain_us / 1000U));
            s_behind_cnt = 0;
        } else if (remain_us > 0) {
            taskYIELD();
            s_behind_cnt = 0;
        } else if (++s_behind_cnt >= 16U) {
            /* 连续落后够多就让 IDLE 喘一 tick，防止把它饿死 */
            s_behind_cnt = 0;
            vTaskDelay(1);
        } else {
            taskYIELD();
        }

        s_next_start_cyc += EAT_FRAME_US * ARCADE_CYC_PER_US;
        if ((int32_t)(arcade_cycles() - s_next_start_cyc) >
            (int32_t)EAT_FRAME_US * (int32_t)ARCADE_CYC_PER_US) {
            /* 落后超过一整帧才重新对齐，否则会把欠的帧一次还掉 */
            s_next_start_cyc = arcade_cycles();
        }

        boot_watchdog_feed();
    }
}

int arcade_app_main(void)
{
    BaseType_t ret;

    LISA_LOGI(TAG, "eat-only firmware, app main entered");

    /* main.c 提前返回，语音助手链路不会跑：按键要自己拉起 */
    if (service_button_init() != 0) {
        LISA_LOGW(TAG, "button init failed");
    }
    /* 按住 / 松手要走原始事件，不然要等多击判定窗口 */
    service_button_set_raw_callback(eat_raw_button_cb, NULL);
    LISA_LOGI(TAG, "button ready");

    /* 面板的 bus 是在这里 attach 的，顺序不能改（见 arcade_app.c 的注释） */
    service_brightness_init();
    LISA_LOGI(TAG, "display service ready");

    eat_display_wakeup();

    ret = xTaskCreate(eat_app_task, "eat", EAT_TASK_STACK, NULL,
                      EAT_TASK_PRIORITY, NULL);
    if (ret != pdPASS) {
        LISA_LOGE(TAG, "create eat task failed");
    } else {
        LISA_LOGI(TAG, "eat task created");
    }

    uint32_t ticks = 0;

    for (;;) {
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(200));

        if (++ticks % 25 == 0) {
            LISA_LOGI(TAG, "alive draws=%u logic=%u.%u render=%u.%u fps",
                      (unsigned)arcade_eat_draws(),
                      (unsigned)(arcade_eat_logic_fps_x10() / 10),
                      (unsigned)(arcade_eat_logic_fps_x10() % 10),
                      (unsigned)(arcade_eat_render_fps_x10() / 10),
                      (unsigned)(arcade_eat_render_fps_x10() % 10));
        }
    }

    return 0;
}

/* ------------- shell 调试命令 ------------- */

/* 把当前帧缓冲打成 80x36 的 ASCII 图（确认不是白屏） */
static void eat_fb_ascii(Shell *shell)
{
    lv_color_t *fb = arcade_video_get_fb();
    static const char k_ch[4] = {' ', '.', ':', '#'};
    char line[96];
    int fb_w = arcade_video_fb_width();
    int fb_h = arcade_video_fb_height();

    if (fb == NULL) {
        shellPrint(shell, "fb=(null)\r\n");
        return;
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
}

static const char *eat_type_name(int type)
{
    return type == 0 ? "PLACE" : "DISH";
}

static const char *eat_state_name(arcade_eat_state_t st)
{
    switch (st) {
    case ARCADE_EAT_IDLE:
        return "idle";
    case ARCADE_EAT_ROLLING:
        return "rolling";
    case ARCADE_EAT_RESULT:
        return "result";
    default:
        return "?";
    }
}

/* 等按键流程走完（shell 只负责投请求，真正的动作在 eat 任务里做） */
static bool eat_wait_state(arcade_eat_state_t want, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (arcade_eat_state() == want) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return arcade_eat_state() == want;
}

static int cmd_eat(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    const char *sub = argc >= 1 ? argv[0] : NULL;

    if (sub == NULL) {
        shellPrint(shell, "state=%s next=%s\r\n",
                   eat_state_name(arcade_eat_state()),
                   eat_type_name(arcade_eat_next_type()));
        shellPrint(shell, "用法: arcade eat [stat|hold <ms>|auto <n>|reset|shot|exit]\r\n");
        return 0;
    }

    if (strcmp(sub, "stat") == 0) {
        shellPrint(shell,
                   "state=%s type=%s frame=%d/%d hold=%dms draws=%u next=%s\r\n",
                   eat_state_name(arcade_eat_state()),
                   eat_type_name(arcade_eat_type()), arcade_eat_frame(),
                   arcade_eat_frame_count(arcade_eat_type()),
                   arcade_eat_hold_ms_cur(), (unsigned)arcade_eat_draws(),
                   eat_type_name(arcade_eat_next_type()));
        shellPrint(shell, "hist: PLACE=%u DISH=%u (frames %d/%d)\r\n",
                   (unsigned)arcade_eat_hist(0), (unsigned)arcade_eat_hist(1),
                   arcade_eat_frame_count(0), arcade_eat_frame_count(1));
        shellPrint(shell, "logic=%u.%u render=%u.%u fps frames=%u renders=%u skips=%u\r\n",
                   (unsigned)(arcade_eat_logic_fps_x10() / 10),
                   (unsigned)(arcade_eat_logic_fps_x10() % 10),
                   (unsigned)(arcade_eat_render_fps_x10() / 10),
                   (unsigned)(arcade_eat_render_fps_x10() % 10),
                   (unsigned)arcade_eat_frames(), (unsigned)arcade_eat_renders(),
                   (unsigned)arcade_eat_skips());
        shellPrint(shell, "raw=%u last=%u psram_free=%u KB\r\n",
                   (unsigned)s_raw_cnt, (unsigned)s_raw_last,
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return 0;
    }

    if (strcmp(sub, "hold") == 0) {
        int ms = argc >= 2 ? atoi(argv[1]) : 400;

        if (ms <= 0) {
            ms = 1;
        }

        arcade_eat_hold(ms);
        /* 按住时长 + 一拍余量；超时也照常打印当前状态，方便看「没停下来」 */
        (void)eat_wait_state(ARCADE_EAT_RESULT, ms + 2000);

        shellPrint(shell, "hold %dms -> state=%s type=%s frame=%d hold=%dms\r\n", ms,
                   eat_state_name(arcade_eat_state()),
                   eat_type_name(arcade_eat_type()), arcade_eat_frame(),
                   arcade_eat_hold_ms_cur());
        return 0;
    }

    if (strcmp(sub, "auto") == 0) {
        int n = argc >= 2 ? atoi(argv[1]) : 0;

        if (n <= 0) {
            shellPrint(shell, "auto: pending=%d done=%d\r\n",
                       arcade_eat_auto_pending() ? 1 : 0, arcade_eat_auto_done());
            return 0;
        }

        if (n > ARCADE_EAT_LOG_MAX) {
            n = ARCADE_EAT_LOG_MAX;
        }

        uint32_t before = arcade_eat_draws();

        arcade_eat_auto(n);

        /* 每次按住 150~550ms，n 次最多 n * 0.8 秒，留足余量等它抽完 */
        for (int waited = 0; waited < 45000 && arcade_eat_auto_pending(); waited += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        int done = arcade_eat_auto_done();
        uint32_t total = arcade_eat_draws();
        uint32_t kept = total < ARCADE_EAT_LOG_MAX ? total : ARCADE_EAT_LOG_MAX;
        uint32_t show = (uint32_t)done < kept ? (uint32_t)done : kept;
        uint32_t c0 = 0;
        uint32_t c1 = 0;

        for (uint32_t k = 0; k < show; k++) {
            arcade_eat_pick_t p;

            if (!arcade_eat_log(kept - show + k, &p)) {
                break;
            }
            if (p.type == 0) {
                c0++;
            } else {
                c1++;
            }
            shellPrint(shell, "  [%02u] %s frame=%u/%d\r\n", (unsigned)(k + 1U),
                       eat_type_name(p.type), (unsigned)p.frame,
                       arcade_eat_frame_count(p.type));
        }

        shellPrint(shell, "auto %d: PLACE=%u DISH=%u (draws %u -> %u) pending=%d\r\n",
                   done, (unsigned)c0, (unsigned)c1, (unsigned)before,
                   (unsigned)arcade_eat_draws(), arcade_eat_auto_pending() ? 1 : 0);
        return 0;
    }

    if (strcmp(sub, "reset") == 0 || strcmp(sub, "exit") == 0) {
        /* 单应用固件没有列表可退：回待机画面 */
        arcade_eat_reset();
        shellPrint(shell, "已回到待机画面\r\n");
        return 0;
    }

    if (strcmp(sub, "shot") == 0) {
        eat_fb_ascii(shell);
        return 0;
    }

    shellPrint(shell, "用法: arcade eat [stat|hold <ms>|auto <n>|reset|shot|exit]\r\n");
    return 0;
}

static int cmd_stat(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    (void)argc;
    (void)argv;

    shellPrint(shell, "mode=eat-only state=%s type=%s frame=%d/%d draws=%u\r\n",
               eat_state_name(arcade_eat_state()), eat_type_name(arcade_eat_type()),
               arcade_eat_frame(), arcade_eat_frame_count(arcade_eat_type()),
               (unsigned)arcade_eat_draws());
    shellPrint(shell, "logic=%u.%u render=%u.%u fps frames=%u renders=%u skips=%u\r\n",
               (unsigned)(s_logic_fps_x10 / 10), (unsigned)(s_logic_fps_x10 % 10),
               (unsigned)(s_render_fps_x10 / 10), (unsigned)(s_render_fps_x10 % 10),
               (unsigned)arcade_eat_frames(), (unsigned)arcade_eat_renders(),
               (unsigned)arcade_eat_skips());
    shellPrint(shell, "raw=%u last=%u psram_free=%u KB\r\n", (unsigned)s_raw_cnt,
               (unsigned)s_raw_last,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return 0;
}

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

static const struct listen_cmd_t g_eat_cmds[] = {
    {"stat", cmd_stat, "打印单应用固件状态（帧率 / PSRAM）"},
    {"eat", cmd_eat, "今天吃啥：eat [stat|hold <ms>|auto <n>|reset|shot|exit]"},
    {"pwrho1d", cmd_pwrho1d, "把 POWER_EN 锁到高：pwrho1d <0|1>（刷机调试用）"},
};

static int eat_cmd_handler(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc <= 1) {
        for (size_t i = 0; i < sizeof(g_eat_cmds) / sizeof(g_eat_cmds[0]); i++) {
            shellPrint(shell, "%-10s\t:\t%s\r\n", g_eat_cmds[i].name, g_eat_cmds[i].help);
        }
        return 0;
    }

    for (size_t i = 0; i < sizeof(g_eat_cmds) / sizeof(g_eat_cmds[0]); i++) {
        if (strcmp(g_eat_cmds[i].name, argv[1]) == 0) {
            return g_eat_cmds[i].exec(argc - 2, argv + 2);
        }
    }

    return cmd_eat(0, NULL);
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 arcade,
                 eat_cmd_handler,
                 今天吃啥单应用固件调试命令);
