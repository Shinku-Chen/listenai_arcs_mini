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
#include "arcade_dino.h"
#include "arcade_jump.h"
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
    ARCADE_MODE_DINO,     /* 内置小游戏「小恐龙」（原生，不走模拟器） */
    ARCADE_MODE_JUMP,     /* 内置小游戏「跳一跳」（原生） */
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
    ARCADE_REQ_DINO,          /* 进入内置小游戏「小恐龙」 */
    ARCADE_REQ_DINO_RESET,    /* 重开一局 */
    ARCADE_REQ_JUMP,          /* 进入内置小游戏「跳一跳」 */
    ARCADE_REQ_JUMP_RESET,    /* 跳一跳重开一局 */
    ARCADE_REQ_JUMP_CHARGE,   /* 跳一跳：按住 arg 毫秒后松手 */
} arcade_req_t;

static arcade_mode_t s_mode = ARCADE_MODE_LIST;
static int s_sel;
/* 按键任务写、模拟器任务读，必须 volatile，否则可能被优化成只读一次的循环 */
static volatile uint8_t s_pending_action = 0xFF;
/* 内置小游戏：「上一帧跳跃键是否按下」，用来做边沿触发 */
static bool s_dino_prev_pressed;
static bool s_jump_prev_pressed;

/*
 * 物理按键的原始事件队列（service_button 的 raw 回调 → arcade 主循环）。
 * 回调跑在按键扫描任务（20ms 一轮），这里只用单生产者/单消费者的读写指针，
 * 不加重入保护：队列满了就丢掉最旧的（按键本来就只是「状态变化」）。
 */
#define ARCADE_RAW_Q_LEN 8
static volatile uint8_t s_raw_q[ARCADE_RAW_Q_LEN];
static volatile uint8_t s_raw_head;
static volatile uint8_t s_raw_tail;
/* 调试用：原始事件计数 / 最后一条（`arcade jump stat` 里能看到，
 * 用来确认「按下」真的在蓄力之前就已经到了） */
static volatile uint32_t s_raw_evt_cnt;
static volatile uint8_t s_raw_evt_last = 0xFF;
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

/*
 * 渲染帧率下限（跳帧保速模式下生效）。
 *
 * 用户口径：速度优先（逻辑/音频必须 59.7fps），但画面能大就大、不能砦。
 * 所以策略是分层的：
 *   1) 先试 1.5x（大画面）；render 连续 2 秒 < 25fps → 降到 1x
 *      （1x 送屏字节减半、缩放几乎不花时间，渲染帧率自然抬高）
 *   2) 1x 还不到 25fps → 抬高渲染比例下限（每 秒 +3%，最高 45%），
 *      但只有在逻辑仍 ≥ 59fps 时才抬
 *   3) 1x 下 render 连续 5 秒 > 32fps → 试回 1.5x（带迟滞，不来回抖）
 */
#define RENDER_FLOOR_FPS    25U
#define RENDER_CEIL_FPS     32U
#define RENDER_FLOOR_STEP   30U      /* 比例下限每次调整的步长（千分数） */
#define RENDER_FLOOR_MAX    450U     /* 比例下限上限（≈27fps） */

static bool s_scale_auto = true;
static uint32_t s_frame_us;          /* 帧周期 EMA（µs） */
static uint32_t s_scale_down_cnt;
static uint32_t s_scale_up_cnt;
static uint32_t s_last_frame_cyc;
/* 渲染下限控制器（1 秒一次采样） */
static uint32_t s_floor_tick_cyc;    /* 上次采样的 rdcycle */
static uint32_t s_floor_low_cnt;     /* 连续低于下限的秒数 */
static uint32_t s_floor_high_cnt;    /* 连续高于上限的秒数 */
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

/*
 * 渲染调度用「千分比相位累加器」，不用「信用够不够」判据。
 *
 * 为什么换：原来的判据是 credit >= 3/4 * 渲染额外开销，而 credit 只在一帧
 * 耗时**低于**帧预算时才涨。一旦渲染成本 EMA 被某个长停帧（写卡 / 切页面，
 * 几十毫秒）学成天价，门槛就抬到 credit 上限之上 —— 实测表现是 logic=58 /
 * render=0（屏幕一直是白板），而且成天不渲染 → 成本 EMA 再也学不回来，死锁。
 * 相位累加器由「目标渲染比例」驱动、比例有下限，渲染帧率不可能归零。
 */
#define ARCADE_RATIO_INIT        667U     /* 初始渲染比例（千分数）= 2/3 */
#define ARCADE_RATIO_MIN         150U     /* 下限：任何情况至少 ~9fps 画面 */
#define ARCADE_RATIO_MAX         1000U
#define ARCADE_RENDER_GAP_MAX    12U      /* 连续这么多帧没画就强制画一帧（~200ms） */

static bool s_skip_auto = true;                       /* 自动跳帧保速 */
static arcade_render_mode_t s_skip_mech = ARCADE_RENDER_SKIP;  /* 跳过的实现方式 */
static uint32_t s_render_cost_us = 18200;             /* 渲染帧耗时 EMA */
static uint32_t s_skip_cost_us = 13700;               /* 跳过帧耗时 EMA */
static uint32_t s_render_ratio = ARCADE_RATIO_INIT;   /* 目标渲染比例（千分数） */
static uint32_t s_ratio_floor = ARCADE_RATIO_MIN;     /* 自适应抬高的比例下限 */
static uint32_t s_render_phase;                       /* 相位累加器（千分数） */
static uint32_t s_render_gap;                         /* 连续未渲染帧数 */
static uint32_t s_force_renders;                      /* 保底强制渲染次数（诊断用） */
static bool s_ever_rendered;
static uint32_t s_next_start_cyc;                     /* 下一帧的绝对起点（rdcycle） */
static uint32_t s_behind_cnt;                         /* 连续落后帧数（喂 IDLE 用） */
static bool s_last_render = true;
static uint32_t s_logic_frames;
static uint32_t s_render_frames;
static uint32_t s_skipped_frames;
static uint32_t s_logic_fps_x10;
static uint32_t s_render_fps_x10;
static uint32_t s_ui_us;                              /* LVGL handler 耗时 EMA（µs） */
static uint32_t s_cost_us;                            /* 整帧工作耗时 EMA（µs，不含睡眠） */
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

static const char *arcade_mode_name(void)
{
    switch (s_mode) {
    case ARCADE_MODE_PLAY:
        return "play";
    case ARCADE_MODE_DINO:
        return "dino";
    case ARCADE_MODE_JUMP:
        return "jump";
    default:
        return "list";
    }
}

/* 是否在「游戏页」（模拟器 / 内置小游戏）：帧节拍与 LVGL 降频都按这个判 */
static bool arcade_in_game(void)
{
    return s_mode == ARCADE_MODE_PLAY || s_mode == ARCADE_MODE_DINO ||
           s_mode == ARCADE_MODE_JUMP;
}

/*
 * 物理按键的**原始**事件回调（service_button → 这里）。
 *
 * 为什么要绕开 voice_msg 的 CLICK：那颗键要把单击/双击/…/长按区分开，必须
 * 等多击判定窗口超时才能把一次按键定性成单击，实测下来几百毫秒，跳一跳的
 * 「按住蓄力、松手起跳」根本没法玩。原始事件是按下/松开那一刻就来的。
 *
 * 回调跑在按键扫描任务（20ms 一轮）里：这里只把事件塞进队列，真正的处理在
 * arcade 任务（arcade_raw_button_drain）里做。
 */
static void arcade_raw_button_cb(uint8_t btn_id, uint8_t event, void *user)
{
    uint8_t next = (uint8_t)((s_raw_head + 1U) % ARCADE_RAW_Q_LEN);

    (void)btn_id;
    (void)user;

    s_raw_evt_cnt++;
    s_raw_evt_last = event;

    if (next == s_raw_tail) {
        /* 满了就丢弃：按键事件只表达「状态变化」，旧的没用了 */
        return;
    }

    s_raw_q[s_raw_head] = event;
    s_raw_head = next;
}

/* 把排队的原始事件喂给当前小游戏（只在 arcade 任务里调） */
static void arcade_raw_button_drain(void)
{
    while (s_raw_tail != s_raw_head) {
        uint8_t ev = s_raw_q[s_raw_tail];

        s_raw_tail = (uint8_t)((s_raw_tail + 1U) % ARCADE_RAW_Q_LEN);

        if (s_mode != ARCADE_MODE_JUMP || !arcade_jump_active()) {
            continue;
        }

        switch ((voice_msg_button_action_t)ev) {
        case VOICE_MSG_BUTTON_ACTION_PRESS_DOWN:
            arcade_jump_press();
            break;
        /* 短按松开 / 长按（>800ms 的蓄力）松开，都是「松手起跳」 */
        case VOICE_MSG_BUTTON_ACTION_SHORT_UP:
        case VOICE_MSG_BUTTON_ACTION_LONG_UP:
        case VOICE_MSG_BUTTON_ACTION_LONG_HOLD_UP:
            arcade_jump_release();
            break;
        default:
            break;
        }
    }
}

/*
 * 内置小游戏（小恐龙）。
 *
 * 它不走模拟器，也就不需要 ROM / 存档 / 音频链路，只需要显示、节拍和按键：
 *   - arcade_video 切成全屏直写几何（240x240，无缩放任务）
 *   - arcade_dino 每帧自己取一块缓冲画完提交
 * 退出时反过来：先让 dino 停手，再把几何换回模拟器的 240x216，最后清空流水线
 * 并刷回 LVGL 的列表页。
 */
static void arcade_start_dino(void)
{
    /* 从模拟器直接切过来时先把模拟器停掉（两者不能同时占着显示流水线） */
    if (arcade_core_running()) {
        arcade_audio_set_active(false);
        arcade_core_stop();
    }

    arcade_ui_show_player();
    arcade_ui_player_message(NULL);

    arcade_video_use_fullscreen(true);
    arcade_dino_enter();

    s_mode = ARCADE_MODE_DINO;
    s_dino_prev_pressed = false;
    s_frames = 0;
    s_fps_tick = xTaskGetTickCount();
    s_fps_x10 = 0;
    s_logic_frames = 0;
    s_render_frames = 0;
    s_skipped_frames = 0;
    s_rate_tick = 0;
    s_next_start_cyc = arcade_cycles();
    s_behind_cnt = 0;

    LISA_LOGI(TAG, "dino started");
}

static void arcade_stop_dino(void)
{
    arcade_dino_leave();
    arcade_video_use_fullscreen(false);
    arcade_video_present_drain();
    arcade_show_list();

    LISA_LOGI(TAG, "dino stopped");
}

/*
 * 内置小游戏（跳一跳）。和 dino 完全一样的进出流程，只是换了一只游戏。
 */
static void arcade_start_jump(void)
{
    if (arcade_core_running()) {
        arcade_audio_set_active(false);
        arcade_core_stop();
    }

    arcade_ui_show_player();
    arcade_ui_player_message(NULL);

    arcade_video_use_fullscreen(true);
    arcade_jump_enter();

    s_mode = ARCADE_MODE_JUMP;
    s_jump_prev_pressed = false;
    s_frames = 0;
    s_fps_tick = xTaskGetTickCount();
    s_fps_x10 = 0;
    s_logic_frames = 0;
    s_render_frames = 0;
    s_skipped_frames = 0;
    s_rate_tick = 0;
    s_next_start_cyc = arcade_cycles();
    s_behind_cnt = 0;

    LISA_LOGI(TAG, "jump started");
}

static void arcade_stop_jump(void)
{
    arcade_jump_leave();
    arcade_video_use_fullscreen(false);
    arcade_video_present_drain();
    arcade_show_list();

    LISA_LOGI(TAG, "jump stopped");
}

static void arcade_start_selected(void)
{
    const char *path = arcade_rom_path(s_sel);

    if (path == NULL) {
        arcade_ui_show_list();
        arcade_ui_list_set_hint("没有可运行的 ROM");
        return;
    }

    /* 列表最前面是固件内置的小游戏，不需要 ROM */
    if (arcade_rom_is_builtin(s_sel)) {
        switch (arcade_rom_builtin_id(s_sel)) {
        case ARCADE_BUILTIN_JUMP:
            arcade_start_jump();
            break;
        default:
            arcade_start_dino();
            break;
        }
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
    /* 跳帧保速的成本估计 / 渲染比例重新学（不同 ROM 的开销差很多） */
    s_render_cost_us = 18200;
    s_skip_cost_us = 13700;
    s_render_ratio = ARCADE_RATIO_INIT;
    s_ratio_floor = ARCADE_RATIO_MIN;
    s_render_phase = 0;
    s_render_gap = 0;
    s_force_renders = 0;
    s_ever_rendered = false;
    s_floor_tick_cyc = 0;
    s_floor_low_cnt = 0;
    s_floor_high_cnt = 0;
    s_next_start_cyc = arcade_cycles();
    s_behind_cnt = 0;
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
    if (s_mode == ARCADE_MODE_DINO) {
        arcade_stop_dino();
        return;
    }

    if (s_mode == ARCADE_MODE_JUMP) {
        arcade_stop_jump();
        return;
    }

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

    LISA_LOGI(TAG, "action=%d mode=%s", (int)action, arcade_mode_name());

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

    /* 内置小游戏：单击 = 起跳 / 死亡后重开，长按 = 退出 */
    if (s_mode == ARCADE_MODE_DINO) {
        switch (action) {
        case VOICE_MSG_BUTTON_ACTION_CLICK:
        case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
            /* 通过 arcade_input 转一道，和 shell 注入的按键走同一条路 */
            arcade_input_tap(ARCADE_KEY_A);
            break;
        case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
            LISA_LOGI(TAG, "long press, leave dino");
            arcade_stop_game();
            break;
        default:
            break;
        }
        return;
    }

    /*
     * 跳一跳：按下/松开走的是 service_button 的原始事件（arcade_raw_button_drain），
     * 这里只能拿到过了多击窗口的汇总事件，再拿它去起跳会晚好几帧（而且会和
     * 原始事件重复触发），所以这里只留「长按退出」。
     */
    if (s_mode == ARCADE_MODE_JUMP) {
        if (action == VOICE_MSG_BUTTON_ACTION_LONG_HOLD) {
            LISA_LOGI(TAG, "long press, leave jump");
            arcade_stop_game();
        }
        return;
    }

    /* 模拟器游戏页 */
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
    case ARCADE_REQ_DINO:
        arcade_start_dino();
        break;
    case ARCADE_REQ_DINO_RESET:
        arcade_dino_reset();
        break;
    case ARCADE_REQ_JUMP:
        arcade_start_jump();
        break;
    case ARCADE_REQ_JUMP_RESET:
        arcade_jump_reset();
        break;
    case ARCADE_REQ_JUMP_CHARGE:
        arcade_jump_charge(arg);
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

/* 记一帧的间隔（ms tick）；只统计游戏状态的帧 */
static void frame_stat_push(TickType_t spent)
{
    if (!arcade_in_game()) {
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

/* 渲染帧相对跳过帧的额外开销（µs） */
static uint32_t arcade_render_extra_us(void)
{
    return (s_render_cost_us > s_skip_cost_us)
               ? (s_render_cost_us - s_skip_cost_us) : 0U;
}

/*
 * 这一帧要不要画？
 *
 * 相位累加器：每帧攒 s_render_ratio 份（千分数），攒满 1000 就渲染一帧并
 * 扣掉 1000 —— 长期渲染比例精确等于 s_render_ratio，不漂移。
 * 再叠两道保底：进入游戏的第一帧必须画（否则开局白板）；连续
 * ARCADE_RENDER_GAP_MAX 帧没画时无条件画一帧（任何估算bug都不会导致白屏）。
 */
static bool arcade_should_render(void)
{
    if (!s_skip_auto) {
        return true;
    }

    if (!s_ever_rendered) {
        return true;
    }

    s_render_phase += s_render_ratio;
    if (s_render_phase >= 1000U) {
        s_render_phase -= 1000U;
        return true;
    }

    if (++s_render_gap >= ARCADE_RENDER_GAP_MAX) {
        s_force_renders++;
        return true;
    }

    return false;
}

/*
 * 成本学习 + 渲染比例自适应。
 *
 * 目标：帧起点间隔稳定在 GB 真实帧周期 16.743ms（音频与游戏速度才正常）。
 * 平均每帧成本 ≈ 跳过成本 + 比例 × 额外开销，所以由实测成本直接解出比例：
 *
 *     比例 = (帧预算 - 跳过成本) / (渲染成本 - 跳过成本)
 *
 * 再慢速趋近（1/8），避免成本抖动带着渲染帧率一起跳。
 * 特意**不**用「信用够不够」做判据：那会让成本估偏时渲染帧率直接归零。
 */
static void arcade_skip_account(uint32_t cost_us, bool rendered)
{
    int32_t target;

    /* 只吃「干净」的帧：写卡 / 切页面那种几十毫秒的长停帧会把成本学成天价 */
    if (cost_us >= 4000U && cost_us <= 30000U) {
        if (rendered) {
            s_render_cost_us = (s_render_cost_us * 7U + cost_us) / 8U;
        } else {
            s_skip_cost_us = (s_skip_cost_us * 7U + cost_us) / 8U;
        }
    }

    if (s_render_cost_us <= s_skip_cost_us + 300U) {
        target = (int32_t)ARCADE_RATIO_MAX;   /* 画不画几乎一样贵 → 全画 */
    } else {
        target = (int32_t)(((int32_t)ARCADE_GB_FRAME_US - (int32_t)s_skip_cost_us) * 1000)
                 / (int32_t)(s_render_cost_us - s_skip_cost_us);
    }

    if (target < (int32_t)s_ratio_floor) {
        target = (int32_t)s_ratio_floor;
    } else if (target > (int32_t)ARCADE_RATIO_MAX) {
        target = (int32_t)ARCADE_RATIO_MAX;
    }

    s_render_ratio = (s_render_ratio * 7U + (uint32_t)target) / 8U;
    if (s_render_ratio < s_ratio_floor) {
        s_render_ratio = s_ratio_floor;
    } else if (s_render_ratio > ARCADE_RATIO_MAX) {
        s_render_ratio = ARCADE_RATIO_MAX;
    }
}

/*
 * 渲染帧率下限 / 上限控制器（跳帧保速模式下每秒跑一次）。
 *
 * 判定只用「每秒采样」的真实 render/logic fps，不用 EMA，避免慢热。
 * 必须在 arcade 任务里调（arcade_video_set_scale 会排空送屏流水线）。
 */
static void arcade_render_floor_tick(void)
{
    uint32_t now = arcade_cycles();
    uint32_t render_x10;
    uint32_t logic_x10;

    if (s_floor_tick_cyc == 0) {
        s_floor_tick_cyc = now;
        return;
    }
    if (arcade_cyc_to_us((uint32_t)(now - s_floor_tick_cyc)) < 1000000U) {
        return;
    }
    s_floor_tick_cyc = now;

    render_x10 = s_render_fps_x10;
    logic_x10 = s_logic_fps_x10;
    /* 刚开局采样还没出来：不动 */
    if (render_x10 == 0 || logic_x10 == 0) {
        return;
    }

    if (render_x10 < RENDER_FLOOR_FPS * 10U) {
        s_floor_high_cnt = 0;

        if (arcade_video_scale() != 1) {
            /* 第一层：先降分辨率（送屏字节减半） */
            if (++s_floor_low_cnt >= 2U) {
                s_floor_low_cnt = 0;
                s_ratio_floor = ARCADE_RATIO_MIN;
                arcade_video_set_scale(1);
                LISA_LOGW(TAG, "render %u.%ufps < %ufps floor -> 1x",
                          (unsigned)(render_x10 / 10U), (unsigned)(render_x10 % 10U),
                          (unsigned)RENDER_FLOOR_FPS);
            }
            return;
        }

        /* 第二层：已经是 1x，抬高渲染比例下限（逻辑仍达标才抬） */
        s_floor_low_cnt = 0;
        if (logic_x10 >= 590U && s_ratio_floor + RENDER_FLOOR_STEP <= RENDER_FLOOR_MAX) {
            uint32_t before = s_ratio_floor;

            s_ratio_floor += RENDER_FLOOR_STEP;
            if (s_ratio_floor > s_render_ratio) {
                s_render_ratio = s_ratio_floor;
            }
            LISA_LOGW(TAG, "render %u.%ufps at 1x -> ratio floor %u%% -> %u%%",
                      (unsigned)(render_x10 / 10U), (unsigned)(render_x10 % 10U),
                      (unsigned)(before / 10U), (unsigned)(s_ratio_floor / 10U));
        }
        return;
    }

    s_floor_low_cnt = 0;

    /* 逻辑被抬高后的下限拖慢了：把下限退回去（速度永远优先于画面） */
    if (logic_x10 < 590U && s_ratio_floor > ARCADE_RATIO_MIN) {
        s_ratio_floor = (s_ratio_floor > ARCADE_RATIO_MIN + RENDER_FLOOR_STEP)
                            ? (s_ratio_floor - RENDER_FLOOR_STEP)
                            : ARCADE_RATIO_MIN;
        return;
    }

    /* 1x 下画面已经富余：试回 1.5x（连续 5 秒才切，带迟滞） */
    if (arcade_video_scale() == 1) {
        if (render_x10 > RENDER_CEIL_FPS * 10U) {
            if (++s_floor_high_cnt >= 5U) {
                s_floor_high_cnt = 0;
                s_ratio_floor = ARCADE_RATIO_MIN;
                arcade_video_set_scale(2);
                LISA_LOGI(TAG, "render %u.%ufps 富余 -> 试回 1.5x",
                          (unsigned)(render_x10 / 10U), (unsigned)(render_x10 % 10U));
            }
        } else {
            s_floor_high_cnt = 0;
        }
    } else {
        s_floor_high_cnt = 0;
    }
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
     * 跳帧保速下，帧周期已经被节拍锁死在 16.74ms，不能再用它判断画面好不好。
     * 改用渲染帧率判：达不到下限就降分辨率，富余了再升回去。
     */
    if (s_skip_auto) {
        arcade_render_floor_tick();
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
        uint32_t ui_t0 = 0;
        uint32_t idle;

        if (s_pending_action != 0xFF) {
            uint8_t action = s_pending_action;
            s_pending_action = 0xFF;
            arcade_handle_action(action);
        }

        /* 物理按键的原始事件（按下/松开），跳一跳靠它做「按住蓄力」 */
        arcade_raw_button_drain();

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

                if (render) {
                    s_render_gap = 0;
                    s_ever_rendered = true;
                }

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
        } else if (s_mode == ARCADE_MODE_DINO && !s_frozen) {
            /*
             * 内置小游戏：也是 60Hz 逻辑 + 可能跳过的渲染，只是渲染由 dino
             * 自己取缓冲（取不到就跳过这一帧），所以不需要 arcade_should_render()
             * 那套比例控制。按键走和模拟器同一条路：读 arcade_input 的点位图，
             * 只在「按下」的边沿触发一次跳跃。
             */
            bool kbd_exit = false;
            uint32_t kbd = arcade_input_take_keyboard(&kbd_exit);

            if (kbd_exit) {
                LISA_LOGI(TAG, "ESC from keyboard, leave dino");
                arcade_stop_game();
            } else {
                uint32_t mask = arcade_input_mask() | kbd;
                bool pressed = (mask & (ARCADE_KEY_A | ARCADE_KEY_UP |
                                        ARCADE_KEY_START)) != 0U;

                if (pressed && !s_dino_prev_pressed) {
                    arcade_dino_click();
                }
                s_dino_prev_pressed = pressed;

                uint32_t rendered_before = arcade_dino_renders();

                arcade_dino_tick();
                arcade_frame_tick();
                arcade_rate_tick(arcade_dino_renders() != rendered_before);
            }
        } else if (s_mode == ARCADE_MODE_JUMP && !s_frozen) {
            /*
             * 跳一跳：同样是 60Hz 逻辑 + 可能跳过的渲染。按键用的是物理键的
             * 原始事件（按下开始蓄力、松手起跳）；这里多出来的那一条是给 USB
             * 键盘 / 调试注入用的——它们没有原始事件，只能看 arcade_input 的位图
             * 边沿，按下即蓄力、松开即起跳。
             */
            bool kbd_exit = false;
            uint32_t kbd = arcade_input_take_keyboard(&kbd_exit);

            if (kbd_exit) {
                LISA_LOGI(TAG, "ESC from keyboard, leave jump");
                arcade_stop_game();
            } else {
                uint32_t mask = arcade_input_mask() | kbd;
                bool pressed = (mask & (ARCADE_KEY_A | ARCADE_KEY_UP |
                                        ARCADE_KEY_START)) != 0U;

                if (pressed && !s_jump_prev_pressed) {
                    arcade_jump_press();
                } else if (!pressed && s_jump_prev_pressed) {
                    arcade_jump_release();
                }
                s_jump_prev_pressed = pressed;

                uint32_t rendered_before = arcade_jump_renders();

                arcade_jump_tick();
                arcade_frame_tick();
                arcade_rate_tick(arcade_jump_renders() != rendered_before);
            }
        }

        /*
         * 游戏页的帧是 arcade_video 直接写面板的，LVGL 在这段时间没有任何
         * 东西要画。每帧调 lv_timer_handler() 纯属白花时间（它要跑定时器链表、
         * 判脏区、再调驱动的 flush），所以播放中降到每 8 帧一次；
         * 有暂停/提示文字盖在画面上时照旧每帧跑。
         */
        if (arcade_in_game() && !s_frozen &&
            !arcade_ui_player_message_visible()) {
            if ((s_lvgl_skip++ & 7U) == 0) {
                ui_t0 = arcade_cycles();
                idle = arcade_ui_handler();
            } else {
                idle = 0;
                ui_t0 = 0;
            }
        } else {
            ui_t0 = arcade_cycles();
            idle = arcade_ui_handler();
        }

        if (ui_t0 != 0) {
            uint32_t ui_us = arcade_cyc_to_us((uint32_t)(arcade_cycles() - ui_t0));

            s_ui_us = (s_ui_us * 7U + ui_us) / 8U;
        }

        TickType_t spent = xTaskGetTickCount() - start;
        uint32_t cost_us = arcade_cyc_to_us((uint32_t)(arcade_cycles() - frame_t0));

        /*
         * 只跟踪工作量（留给 `arcade period` 报告），不再拿它去抬步长。
         */
        if (s_mode == ARCADE_MODE_PLAY) {
            s_work_ema = (s_work_ema * 7U + (cost_us / 1000U)) / 8U;
            s_cost_us = (s_cost_us * 7U + cost_us) / 8U;
            arcade_skip_account(cost_us, s_last_render);
        } else if (s_mode == ARCADE_MODE_DINO || s_mode == ARCADE_MODE_JUMP) {
            /* 小游戏的整帧耗时单独记（arcade perf 能直接看到） */
            s_work_ema = (s_work_ema * 7U + (cost_us / 1000U)) / 8U;
            s_cost_us = (s_cost_us * 7U + cost_us) / 8U;
        }

        frame_stat_push(spent);

        /*
         * 帧节拍：按 rdcycle 的微秒数算，而不是按 1ms 的 tick 算。
         * tick 粒度向下取整会把 16.74ms 的预算变成 16ms（62.5fps，音频偏快
         * 4.5%），四舍五入到最近的 tick 才能压在 59.7fps 附近。
         */
        uint32_t period = s_period_ms;
        uint32_t budget_us = (arcade_in_game() && s_period_auto)
                                 ? ARCADE_GB_FRAME_US : (period * 1000U);

        if (!arcade_in_game()) {
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

        if (arcade_in_game()) {
            /*
             * 节拍控制：按 rdcycle 的**绝对**帧起点间隔锁 16.743ms。
             *
             * 用绝对时间而不是「累计信用」：tick 只有 1ms 粒度，睡 16 还是 17ms
             * 都会让逻辑偏离 59.7fps（偏快 → 音高偏高，偏慢 → 偏低）。绝对节拍下
             * 睡少的那部分下一帧自动补回来，长期平均正好 16.743ms。
             */
            int32_t remain_us = (int32_t)((int32_t)(s_next_start_cyc - arcade_cycles()) /
                                          (int32_t)ARCADE_CYC_PER_US);

            if (remain_us >= 1000) {
                /* 整毫秒睡掉；余数留在绝对节拍里，下一帧继续用 */
                vTaskDelay(pdMS_TO_TICKS((uint32_t)remain_us / 1000U));
                s_behind_cnt = 0;
            } else if (remain_us > 0) {
                /* 不足 1 tick：睡会多睡掉整 1ms，让出一次就够 */
                taskYIELD();
                s_behind_cnt = 0;
            } else {
                /* 已经落后：不睡。但连续落后够多时让 IDLE 喘一 tick（防饿死） */
                if (++s_behind_cnt >= 16U) {
                    s_behind_cnt = 0;
                    vTaskDelay(1);
                } else {
                    taskYIELD();
                }
            }
            s_overrun_count = 0;

            /* 推进到下一帧的绝对起点；落后超过一整帧才重新对齐（否则会把债还掉） */
            s_next_start_cyc += (uint32_t)ARCADE_GB_FRAME_US * ARCADE_CYC_PER_US;
            if ((int32_t)(arcade_cycles() - s_next_start_cyc) >
                (int32_t)ARCADE_GB_FRAME_US * (int32_t)ARCADE_CYC_PER_US) {
                s_next_start_cyc = arcade_cycles();
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
    /* 小游戏要按键的原始「按下 / 松开」（多击判定窗口太慢，蓄力没法玩） */
    service_button_set_raw_callback(arcade_raw_button_cb, NULL);
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
               arcade_mode_name(),
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
    shellPrint(shell, "cost: render=%u.%ums skip=%u.%ums extra=%u.%ums\r\n",
               (unsigned)(s_render_cost_us / 1000),
               (unsigned)((s_render_cost_us / 100) % 10),
               (unsigned)(s_skip_cost_us / 1000),
               (unsigned)((s_skip_cost_us / 100) % 10),
               (unsigned)(arcade_render_extra_us() / 1000),
               (unsigned)((arcade_render_extra_us() / 100) % 10));
    shellPrint(shell, "sched: ratio=%u.%u%% floor=%u.%u%% gap=%u forced=%u late=%dms\r\n",
               (unsigned)(s_render_ratio / 10), (unsigned)(s_render_ratio % 10),
               (unsigned)(s_ratio_floor / 10), (unsigned)(s_ratio_floor % 10),
               (unsigned)s_render_gap, (unsigned)s_force_renders,
               s_mode == ARCADE_MODE_PLAY
                   ? (int)((int32_t)(arcade_cycles() - s_next_start_cyc) /
                           (int32_t)(ARCADE_CYC_PER_US * 1000U))
                   : 0);
    shellPrint(shell, "breakdown: emu=%u.%ums ui=%u.%ums loop=%u.%ums scale=%s msg=%d\r\n",
               (unsigned)(arcade_gb_emu_us() / 1000),
               (unsigned)((arcade_gb_emu_us() / 100) % 10),
               (unsigned)(s_ui_us / 1000), (unsigned)((s_ui_us / 100) % 10),
               (unsigned)(s_cost_us / 1000), (unsigned)((s_cost_us / 100) % 10),
               arcade_video_scale() == 1 ? "1x" : "1.5x",
               arcade_ui_player_message_visible() ? 1 : 0);
    {
        uint32_t emu_r = 0;
        uint32_t emu_s = 0;

        arcade_gb_emu_split(&emu_r, &emu_s);
        shellPrint(shell, "emu: render=%u.%ums skip=%u.%ums\r\n",
                   (unsigned)(emu_r / 1000), (unsigned)((emu_r / 100) % 10),
                   (unsigned)(emu_s / 1000), (unsigned)((emu_s / 100) % 10));
    }
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
               arcade_mode_name(),
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
static void arcade_fb_ascii(Shell *shell)
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
}

static int cmd_shot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    arcade_fb_ascii(shellGetCurrent());
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
               arcade_mode_name(),
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
            /* 交还自动：下限控制器从干净状态重新学 */
            s_floor_tick_cyc = 0;
            s_floor_low_cnt = 0;
            s_floor_high_cnt = 0;
            s_ratio_floor = ARCADE_RATIO_MIN;
            shellPrint(shell, "缩放: 自动（优先 1.5x，render 不足 %ufps 就掉 1x）\r\n",
                       (unsigned)RENDER_FLOOR_FPS);
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
 * arcade presents <0|1>：渲染但关掉送屏 / 打开送屏。
 *
 * 用来把「PPU 画像素」与「缩放 + SPI 送屏」两段成本拆开：
 *   presents 1（默认）→ 一个渲染帧的完整成本
 *   presents 0        → 画像素但不送屏的成本
 * 两者之差就是送屏（含 blit 任务抢占给模拟器带来的损失）。
 */
static int cmd_presents(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc < 1) {
        shellPrint(shell, "用法: arcade presents <0|1>\r\n");
        return 0;
    }

    arcade_gb_set_present_enabled(atoi(argv[0]) != 0);
    shellPrint(shell, "present %s（跳帧的成本估计会重新学）\r\n",
               atoi(argv[0]) != 0 ? "on" : "off");
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

        s_render_phase = 0;
        s_render_gap = 0;
        s_ever_rendered = false;
        s_next_start_cyc = arcade_cycles();
    }

    shellPrint(shell, "skip=%s 方式=%s  render≈%u.%ums skip≈%u.%ums extra≈%u.%ums ratio=%u.%u%%\r\n",
               s_skip_auto ? "auto" : "off",
               s_skip_mech == ARCADE_RENDER_DUMMY ? "dummy" : "full",
               (unsigned)(s_render_cost_us / 1000),
               (unsigned)((s_render_cost_us / 100) % 10),
               (unsigned)(s_skip_cost_us / 1000),
               (unsigned)((s_skip_cost_us / 100) % 10),
               (unsigned)(arcade_render_extra_us() / 1000),
               (unsigned)((arcade_render_extra_us() / 100) % 10),
               (unsigned)(s_render_ratio / 10), (unsigned)(s_render_ratio % 10));
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

/*
 * arcade dino [...]: 内置小游戏「小恐龙」
 *
 *   arcade dino              进入游戏（等价于在列表里选第一项）
 *   arcade dino stat         状态：fps / 分数 / 高度 / 障碍
 *   arcade dino jump         手动跳一次（等价于按一下键）
 *   arcade dino reset        重开一局
 *   arcade dino auto <0|1>   自动跳（无人值守压测）
 *   arcade dino shot         帧缓冲 ASCII 图（看有没有画面）
 *   arcade dino exit         退出回列表
 */
static int cmd_dino(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    const char *sub = argc >= 1 ? argv[0] : NULL;

    if (sub == NULL) {
        if (arcade_request(ARCADE_REQ_DINO, 0) != 0) {
            shellPrint(shell, "请求超时\r\n");
            return 0;
        }
        shellPrint(shell, "小恐龙: 已进入（单击跳跃，长按退出）\r\n");
        return 0;
    }

    if (strcmp(sub, "stat") == 0) {
        if (!arcade_dino_active()) {
            shellPrint(shell, "当前没在玩小恐龙（mode=%s）\r\n", arcade_mode_name());
            return 0;
        }

        shellPrint(shell, "state=%s score=%u best=%u speed=%dpx/s y=%d air=%d obs=%d\r\n",
                   arcade_dino_state() == ARCADE_DINO_DEAD ? "dead" : "run",
                   (unsigned)arcade_dino_score(), (unsigned)arcade_dino_best(),
                   arcade_dino_speed_px(), arcade_dino_height(),
                   arcade_dino_airborne() ? 1 : 0, arcade_dino_obstacle_count());

        int n = arcade_dino_obstacle_count();
        int off = 0;
        char buf_dino_obs[96];

        buf_dino_obs[0] = '\0';
        for (int i = 0; i < n && off < 90; i++) {
            off += snprintf(buf_dino_obs + off, sizeof(buf_dino_obs) - (size_t)off, " %d",
                            arcade_dino_obstacle_x(i));
        }
        shellPrint(shell, "obs_x=%s\r\n", n > 0 ? buf_dino_obs : "(无)");

        shellPrint(shell, "logic=%u.%u render=%u.%u fps frames=%u renders=%u skips=%u\r\n",
                   (unsigned)(arcade_dino_logic_fps_x10() / 10),
                   (unsigned)(arcade_dino_logic_fps_x10() % 10),
                   (unsigned)(arcade_dino_render_fps_x10() / 10),
                   (unsigned)(arcade_dino_render_fps_x10() % 10),
                   (unsigned)arcade_dino_frames(), (unsigned)arcade_dino_renders(),
                   (unsigned)arcade_dino_skips());
        shellPrint(shell, "auto=%d\r\n", arcade_dino_auto() ? 1 : 0);
        return 0;
    }

    if (strcmp(sub, "jump") == 0) {
        arcade_dino_click();
        shellPrint(shell, "jump\r\n");
        return 0;
    }

    if (strcmp(sub, "reset") == 0) {
        (void)arcade_request(ARCADE_REQ_DINO_RESET, 0);
        shellPrint(shell, "已重开\r\n");
        return 0;
    }

    if (strcmp(sub, "auto") == 0) {
        if (argc < 2) {
            shellPrint(shell, "auto=%d\r\n", arcade_dino_auto() ? 1 : 0);
            return 0;
        }
        arcade_dino_set_auto(atoi(argv[1]) != 0);
        shellPrint(shell, "auto=%d\r\n", arcade_dino_auto() ? 1 : 0);
        return 0;
    }

    if (strcmp(sub, "shot") == 0) {
        arcade_fb_ascii(shell);
        return 0;
    }

    if (strcmp(sub, "exit") == 0) {
        (void)arcade_request(ARCADE_REQ_EXIT, 0);
        shellPrint(shell, "已返回列表\r\n");
        return 0;
    }

    shellPrint(shell, "用法: arcade dino [stat|jump|reset|auto <0|1>|shot|exit]\r\n");
    return 0;
}

/*
 * arcade jump [...]: 内置小游戏「跳一跳」
 *
 *   arcade jump              进入游戏（等价于在列表里选第一项）
 *   arcade jump stat         状态：分数 / 蓄力 / 平台参数 / fps
 *   arcade jump charge <ms>  模拟「按住 ms 毫秒后松手」
 *   arcade jump auto <0|1>   自动玩（按距离反算蓄力 + 随机误差，无人值守压测）
 *   arcade jump reset        重开一局
 *   arcade jump shot         帧缓冲 ASCII 图（看有没有画面）
 *   arcade jump exit         退出回列表
 */
static int cmd_jump(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    const char *sub = argc >= 1 ? argv[0] : NULL;

    if (sub == NULL) {
        if (arcade_request(ARCADE_REQ_JUMP, 0) != 0) {
            shellPrint(shell, "请求超时\r\n");
            return 0;
        }
        shellPrint(shell, "跳一跳: 已进入（按住蓄力、松手起跳，长按退出）\r\n");
        return 0;
    }

    if (strcmp(sub, "stat") == 0) {
        static const char *const k_state[] = {"ready", "charge", "fly", "pan",
                                              "fall", "dead"};
        uint8_t st = (uint8_t)arcade_jump_state();

        if (!arcade_jump_active()) {
            shellPrint(shell, "当前没在玩跳一跳（mode=%s）\r\n", arcade_mode_name());
            return 0;
        }

        shellPrint(shell, "state=%s score=%u best=%u combo=%u charge=%d%%\r\n",
                   st < sizeof(k_state) / sizeof(k_state[0]) ? k_state[st] : "?",
                   (unsigned)arcade_jump_score(), (unsigned)arcade_jump_best(),
                   (unsigned)arcade_jump_combo(), arcade_jump_charge_pct());
        shellPrint(shell, "gap=%d dir=%s rx=%d nrx=%d err=%d\r\n",
                   arcade_jump_gap(), arcade_jump_dir() > 0 ? "右上" : "左上",
                   arcade_jump_radius(), arcade_jump_next_radius(),
                   arcade_jump_land_err());
        shellPrint(shell, "logic=%u.%u render=%u.%u fps frames=%u renders=%u skips=%u\r\n",
                   (unsigned)(arcade_jump_logic_fps_x10() / 10),
                   (unsigned)(arcade_jump_logic_fps_x10() % 10),
                   (unsigned)(arcade_jump_render_fps_x10() / 10),
                   (unsigned)(arcade_jump_render_fps_x10() % 10),
                   (unsigned)arcade_jump_frames(), (unsigned)arcade_jump_renders(),
                   (unsigned)arcade_jump_skips());
        shellPrint(shell, "auto=%d raw=%u last=%u\r\n", arcade_jump_auto() ? 1 : 0,
                   (unsigned)s_raw_evt_cnt, (unsigned)s_raw_evt_last);
        return 0;
    }

    if (strcmp(sub, "charge") == 0) {
        int ms = argc >= 2 ? atoi(argv[1]) : 400;

        if (arcade_request(ARCADE_REQ_JUMP_CHARGE, ms) != 0) {
            shellPrint(shell, "请求超时\r\n");
            return 0;
        }
        shellPrint(shell, "charge %dms -> score=%u state=%d\r\n", ms,
                   (unsigned)arcade_jump_score(), (int)arcade_jump_state());
        return 0;
    }

    if (strcmp(sub, "auto") == 0) {
        if (argc < 2) {
            shellPrint(shell, "auto=%d\r\n", arcade_jump_auto() ? 1 : 0);
            return 0;
        }
        arcade_jump_set_auto(atoi(argv[1]) != 0);
        shellPrint(shell, "auto=%d\r\n", arcade_jump_auto() ? 1 : 0);
        return 0;
    }

    if (strcmp(sub, "reset") == 0) {
        (void)arcade_request(ARCADE_REQ_JUMP_RESET, 0);
        shellPrint(shell, "已重开\r\n");
        return 0;
    }

    if (strcmp(sub, "shot") == 0) {
        arcade_fb_ascii(shell);
        return 0;
    }

    if (strcmp(sub, "exit") == 0) {
        (void)arcade_request(ARCADE_REQ_EXIT, 0);
        shellPrint(shell, "已返回列表\r\n");
        return 0;
    }

    shellPrint(shell, "用法: arcade jump [stat|charge <ms>|auto <0|1>|reset|shot|exit]\r\n");
    return 0;
}

static const struct listen_cmd_t g_arcade_cmds[] = {    {"stat", cmd_stat, "打印模拟器状态与帧率"},
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
    {"presents", cmd_presents, "渲染但关/开送屏：presents <0|1>（拆分画像素 vs 送屏成本）"},
    {"pwrho1d", cmd_pwrho1d, "把 POWER_EN 锁到高：pwrho1d <0|1>（刷机调试用）"},
    {"membench", cmd_membench, "量 PSRAM / SRAM 带宽"},
    {"dino", cmd_dino, "内置小游戏小恐龙：dino [stat|jump|reset|auto <0|1>|shot|exit]"},
    {"jump", cmd_jump, "内置小游戏跳一跳：jump [stat|charge <ms>|auto <0|1>|reset|shot|exit]"},
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
