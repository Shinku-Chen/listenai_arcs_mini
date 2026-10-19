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
} arcade_req_t;

static arcade_mode_t s_mode = ARCADE_MODE_LIST;
static int s_sel;
/* 按键任务写、模拟器任务读，必须 volatile，否则可能被优化成只读一次的循环 */
static volatile uint8_t s_pending_action = 0xFF;

static volatile arcade_req_t s_req;
static volatile int s_req_arg;
/* shell 的 arcade save 命令等这个结果 */
static volatile int s_save_result;

/* arcade fb：冻结画面并把帧缓冲/刷屏链路的状态回报给 shell */
static volatile bool s_frozen;
static volatile bool s_fb_ready;
static char s_fb_report[1024];

static uint32_t s_frames;
static TickType_t s_fps_tick;
static uint32_t s_fps_x10;

extern int boot_watchdog_feed(void);
extern struct lsfs_mount_t *platform_sd_mount_get(void);

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
    static const int k_sx[3] = {0, ARCADE_FB_WIDTH / 2, ARCADE_FB_WIDTH - 1};
    static const int k_sy[3] = {0, ARCADE_FB_HEIGHT / 2, ARCADE_FB_HEIGHT - 1};
    lv_color_t *fb = arcade_video_get_fb();
    int off = 0;
    int uniq[5] = {0};
    int uniq_n = 0;

    if (fb == NULL) {
        snprintf(s_fb_report, sizeof(s_fb_report), "fb=(null)");
        return;
    }

    for (int i = 0; i < ARCADE_FB_WIDTH * ARCADE_FB_HEIGHT; i++) {
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
            int idx = k_sy[r] * ARCADE_FB_WIDTH + k_sx[c];
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
    arcade_ui_player_invalidate();
    lv_task_handler();

    /* 进入游戏才开始出声（列表页保持静音） */
    arcade_audio_set_active(true);

    s_mode = ARCADE_MODE_PLAY;
    s_frames = 0;
    s_fps_tick = xTaskGetTickCount();
    s_fps_x10 = 0;
}

static void arcade_stop_game(void)
{
    arcade_audio_set_active(false);
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
        arcade_ui_player_invalidate();
        lv_task_handler();
        break;
    case ARCADE_REQ_FB_RESUME:
        s_frozen = false;
        arcade_video_fill_black();
        arcade_ui_player_invalidate();
        lv_task_handler();
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

    (void)argc;
    (void)argv;

    if (fb == NULL) {
        shellPrint(shell, "fb=(null)\r\n");
        return 0;
    }

    for (int row = 0; row < 36; row++) {
        int off = 0;
        int y0 = row * (ARCADE_FB_HEIGHT / 36);
        int dy = ARCADE_FB_HEIGHT / 36;

        for (int col = 0; col < 80; col++) {
            int x0 = col * (ARCADE_FB_WIDTH / 80);
            int dx = ARCADE_FB_WIDTH / 80;
            int lum = 0;
            int n = 0;

            for (int y = y0; y < y0 + dy; y++) {
                for (int x = x0; x < x0 + dx; x++) {
                    uint16_t v = fb[y * ARCADE_FB_WIDTH + x].full;
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

static const struct listen_cmd_t g_arcade_cmds[] = {
    {"stat", cmd_stat, "打印模拟器状态与帧率"},
    {"list", cmd_list, "列出 TF 卡中的 ROM"},
    {"rescan", cmd_rescan, "重新扫描 TF 卡"},
    {"play", cmd_play, "载入指定序号的 ROM"},
    {"exit", cmd_exit, "退出游戏返回列表"},
    {"key", cmd_key, "注入一次按键"},
    {"press", cmd_press, "按住/松开某个按键"},
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
