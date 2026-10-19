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

#include "voice_msg.h"
#include "service_button.h"

#include "arcade_app.h"
#include "arcade_core.h"
#include "arcade_input.h"
#include "arcade_rom.h"
#include "arcade_ui.h"
#include "arcade_video.h"

#define TAG "arcade"

#define ARCADE_TASK_STACK     4096
#define ARCADE_TASK_PRIORITY  4

typedef enum {
    ARCADE_MODE_LIST = 0,
    ARCADE_MODE_PLAY,
} arcade_mode_t;

typedef enum {
    ARCADE_REQ_NONE = 0,
    ARCADE_REQ_PLAY,
    ARCADE_REQ_EXIT,
    ARCADE_REQ_RESCAN,
} arcade_req_t;

static arcade_mode_t s_mode = ARCADE_MODE_LIST;
static int s_sel;
static uint8_t s_pending_action = 0xFF;

static volatile arcade_req_t s_req;
static volatile int s_req_arg;

static uint32_t s_frames;
static TickType_t s_fps_tick;
static uint32_t s_fps_x10;

extern int boot_watchdog_feed(void);

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

    arcade_ui_player_message(NULL);
    arcade_ui_show_player();

    if (arcade_core_start(path) != 0) {
        arcade_show_list();
        arcade_ui_list_set_hint("加载失败（格式不支持或校验错误）");
        return;
    }

    arcade_video_fill_black();
    arcade_ui_player_invalidate();

    s_mode = ARCADE_MODE_PLAY;
    s_frames = 0;
    s_fps_tick = xTaskGetTickCount();
    s_fps_x10 = 0;
}

static void arcade_stop_game(void)
{
    arcade_core_stop();
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
    default:
        break;
    }
}

/* ------------- 主循环 ------------- */

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

static void arcade_task(void *arg)
{
    (void)arg;

    if (arcade_video_init() != 0) {
        LISA_LOGE(TAG, "video init failed");
    }

    if (arcade_ui_init() != 0) {
        LISA_LOGE(TAG, "ui init failed");
    }

    arcade_ui_show_list();
    arcade_ui_list_set_hint("正在读取 TF 卡…");
    arcade_ui_list_refresh(0);
    lv_task_handler();

    arcade_rom_scan();
    arcade_ui_list_refresh(s_sel);

    for (;;) {
        TickType_t start = xTaskGetTickCount();
        uint32_t idle;

        if (s_pending_action != 0xFF) {
            uint8_t action = s_pending_action;
            s_pending_action = 0xFF;
            arcade_handle_action(action);
        }

        arcade_handle_request();

        if (s_mode == ARCADE_MODE_PLAY) {
            bool kbd_exit = false;
            uint32_t kbd = arcade_input_take_keyboard(&kbd_exit);

            if (kbd_exit) {
                LISA_LOGI(TAG, "ESC from keyboard, back to ROM list");
                arcade_stop_game();
            } else {
                arcade_core_set_input(arcade_input_mask() | kbd);
                arcade_core_run_frame();
                arcade_ui_player_invalidate();
                arcade_frame_tick();
            }
        }

        idle = arcade_ui_handler();

        uint32_t period = CONFIG_ARCADE_FRAME_PERIOD_MS;
        if (s_mode != ARCADE_MODE_PLAY && idle > period) {
            period = idle;
        }

        TickType_t spent = xTaskGetTickCount() - start;
        TickType_t wait = pdMS_TO_TICKS(period);
        if (spent < wait) {
            vTaskDelay(wait - spent);
        } else {
            vTaskDelay(1);
        }

        boot_watchdog_feed();
    }
}

int arcade_app_main(void)
{
    BaseType_t ret;

    /* main.c 提前返回，语音助手的初始化链路不会跑，按键需要自己拉起 */
    if (service_button_init() != 0) {
        LISA_LOGW(TAG, "button init failed");
    }

    voice_msg_sub(VOICE_MSG_BUTTON_CHANGE, arcade_button_cb, NULL);

    ret = xTaskCreate(arcade_task, "arcade", ARCADE_TASK_STACK, NULL, ARCADE_TASK_PRIORITY, NULL);
    if (ret != pdPASS) {
        LISA_LOGE(TAG, "create arcade task failed");
        return -1;
    }

    for (;;) {
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(200));
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

static int cmd_press(int argc, char **argv)
{
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

static const struct listen_cmd_t g_arcade_cmds[] = {
    {"stat", cmd_stat, "打印模拟器状态与帧率"},
    {"list", cmd_list, "列出 TF 卡中的 ROM"},
    {"rescan", cmd_rescan, "重新扫描 TF 卡"},
    {"play", cmd_play, "载入指定序号的 ROM"},
    {"exit", cmd_exit, "退出游戏返回列表"},
    {"key", cmd_key, "注入一次按键"},
    {"press", cmd_press, "按住/松开某个按键"},
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
