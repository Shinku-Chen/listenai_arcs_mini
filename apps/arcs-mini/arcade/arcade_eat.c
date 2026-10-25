/*
 * 内置小游戏：「今天吃啥」老虎机。
 *
 * 用户需求：
 *   1. 按住 → 图片快速循环滚动（老虎机）
 *   2. 松手 → 立刻定格，屏上那帧就是结果
 *   3. 两组素材**严格交替**：第 1 次按住 → 第一组（去处盘），松手定格；
 *      第 2 次按住 → 第二组（菜品盘）；第 3 次又回第一组……组内起始帧随机
 *   4. 长按**不退出、不关机**（单应用固件里这颗键只负责抽签）
 *
 * 素材是两套索引色帧序列（见 arcade/assets/ + tools/prepare_eat_what.py）：
 *   每帧 = 256 色 RGB565 调色板(512B) + 160x160 像素索引(25600B)
 * 屏上按 1.5 倍最近邻放大到 240x240 —— 和模拟器切 1.5 倍时同一个比例，
 * 于是 160*3 = 240*2，横向每个源像素正好出 1.5 个目标像素，可以按
 * 「2 个源像素 → 3 个目标像素」成组写，省掉一次索引查表。
 *
 * 画面直接写 arcade_video 的 240x240 全屏缓冲（不走 LVGL，也不走模拟器）。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_log.h"

#include "arcade_core.h"
#include "arcade_eat.h"
#include "arcade_eat_assets.h"
#include "arcade_gfx.h"
#include "arcade_video.h"

#define TAG "arcade-eat"

/* 素材几何：160x160 索引帧 → 240x240 目标（1.5 倍） */
#define EAT_SRC        ARCADE_EAT_RES
#define EAT_DST        ARCADE_GFX_W
#define EAT_PALETTE    (256 * 2)
#define EAT_FRAME      ARCADE_EAT_FRAME_BYTES

/* 逻辑步长 / 滚动速度 */
#define EAT_STEP_MS    17      /* 每逻辑帧累积的时间（≈16.7ms，和跳一跳一致） */
#define EAT_ROLL_MS    60      /* 每 60ms 换一帧 ≈ 16.7 fps 的滚动感 */
/*
 * 长按退出的毫秒数。
 *
 * 单应用固件（CONFIG_ARCADE_APP_EATONLY=y）里根本没有「列表页」可以退回，
 * 而且用户明确要求「长按不关机」——所以这个值设成永远到不了的上限，
 * 滚动过程想按多久就按多久。全功能固件里仍然保留「按住 3 秒回列表」。
 *
 * 为什么自己数而不用驱动给的 LONG_HOLD：那个在 1600ms 就报，
 * 「按住滚动、想清楚了再松手」很容易超过 1.6 秒，会被误退出。
 */
#if CONFIG_ARCADE_APP_EATONLY
#define EAT_EXIT_HOLD_MS 0xFFFFFFFFU
#else
#define EAT_EXIT_HOLD_MS 3000
#endif

#define RGB565(r, g, b) ARCADE_GFX_RGB565(r, g, b)

#define C_BAR_FG   RGB565(242, 178,  58)
#define C_STRIP    RGB565(238,  92,  70)
#define C_INK      RGB565( 20,  22,  28)
#define C_DIM      RGB565(140, 146, 160)

/* ---------------- 状态 ---------------- */

static struct {
    uint8_t state;       /* arcade_eat_state_t */
    uint8_t type;        /* 0 = 去处盘(g1)，1 = 菜品盘(g2) */
    uint16_t frame;      /* 当前帧序号 */
    uint32_t hold_ms;    /* 本次按住了多久 */
    uint32_t roll_acc;   /* 滚动帧的毫秒累加器 */
    uint32_t advances;   /* 本次按下后滚过多少帧 */
    uint32_t rng;
} g;

static bool s_active;

/* 按键请求：原始按键在按键扫描任务里落成标志，arcade 任务里消费 */
static volatile bool s_press_req;
static volatile bool s_release_req;
/* >0 表示「按住这么多毫秒后自动松手」（调试 hold / 自动连抽用） */
static volatile int32_t s_hold_target_ms;
/* 按住了 EAT_EXIT_HOLD_MS 还没松手 → 请求退出（上层消费后回列表） */
static volatile bool s_exit_req;

/* 自动连抽 */
static volatile int32_t s_auto_total;
static volatile int32_t s_auto_done;

/* 抽取记录（环形，只在 arcade 任务里写） */
static arcade_eat_pick_t s_log[ARCADE_EAT_LOG_MAX];
static uint32_t s_log_head;
static uint32_t s_draws;
static uint32_t s_hist[2];
/* 下一次按下抽哪一组：0 = 去处盘(g1)，1 = 菜品盘(g2)。
 * 严格交替，开机从 0 开始（第一次按住永远是第一组）。 */
static uint8_t s_next_type;

/* 自己的一份 fps 统计（`arcade eat stat` 用） */
static uint32_t s_logic_frames;
static uint32_t s_render_frames;
static uint32_t s_skipped_frames;
static uint32_t s_logic_fps_x10;
static uint32_t s_render_fps_x10;
static TickType_t s_rate_tick;
static uint32_t s_rate_logic;
static uint32_t s_rate_render;

/* 目标行 → 源行（1.5 倍最近邻：src = dst * 2 / 3） */
static uint8_t s_row[EAT_DST];
static bool s_geom_ready;

/* 两套素材的帧数（进游戏时算一次） */
static int s_frames[2];

/* ---------------- 素材访问 ---------------- */

static const uint8_t *asset_data(int type)
{
    return type == 0 ? arcade_eat_g1 : arcade_eat_g2;
}

static uint32_t asset_size(int type)
{
    return type == 0 ? arcade_eat_g1_size : arcade_eat_g2_size;
}

static int asset_frames(int type)
{
    uint32_t bytes = asset_size(type);

    if (bytes < EAT_FRAME) {
        return 0;
    }

    return (int)(bytes / EAT_FRAME);
}

static bool have_assets(void)
{
    return s_frames[0] > 0 || s_frames[1] > 0;
}

/* 取某一帧的首地址；越界或没素材时返回 NULL */
static const uint8_t *frame_ptr(int type, int frame)
{
    if (type < 0 || type > 1 || frame < 0 || frame >= s_frames[type]) {
        return NULL;
    }

    return asset_data(type) + (uint32_t)frame * EAT_FRAME;
}

/* ---------------- 随机 / 帧推进 ---------------- */

static uint32_t eat_rand(void)
{
    /* 线性同余，够用且没有除法。取高位，低位周期太短。 */
    g.rng = g.rng * 1103515245U + 12345U;
    return g.rng >> 16;
}

static void eat_advance(void)
{
    int n = s_frames[g.type];

    if (n <= 0) {
        return;
    }

    g.frame = (uint16_t)((g.frame + 1) % (uint32_t)n);
    g.advances++;
}

/* ---------------- 抽取记录 ---------------- */

static void eat_record(void)
{
    if (s_draws < ARCADE_EAT_LOG_MAX) {
        s_log[s_draws].type = g.type;
        s_log[s_draws].frame = (uint8_t)g.frame;
    } else {
        s_log[s_log_head].type = g.type;
        s_log[s_log_head].frame = (uint8_t)g.frame;
    }

    s_log_head = (s_log_head + 1U) % ARCADE_EAT_LOG_MAX;
    s_draws++;

    if (g.type < 2) {
        s_hist[g.type]++;
    }

    if (s_auto_total > 0) {
        s_auto_done++;
    }
}

/* ---------------- 逻辑 ---------------- */

static void eat_begin_roll(void)
{
    if (g.state == ARCADE_EAT_ROLLING) {
        return;
    }

    if (!have_assets()) {
        return;
    }

    /* 严格交替：这一次抽 s_next_type，下一次就是另一组。
     * 某一套素材缺失时退化成另一套（保证按下去一定有画面）。 */
    g.type = s_next_type;
    if (s_frames[g.type] == 0) {
        g.type = (uint8_t)(g.type ^ 1U);
    }
    if (s_frames[g.type] == 0) {
        return;
    }
    s_next_type = (uint8_t)(g.type ^ 1U);

    g.frame = (uint16_t)(eat_rand() % (uint32_t)s_frames[g.type]);
    g.hold_ms = 0;
    g.roll_acc = 0;
    g.advances = 0;
    g.state = ARCADE_EAT_ROLLING;
}

static void eat_stop_roll(void)
{
    if (g.state != ARCADE_EAT_ROLLING) {
        return;
    }

    /* 极短的一下点击也至少走一帧，否则看起来像「按了没反应」 */
    if (g.advances == 0) {
        eat_advance();
    }

    s_hold_target_ms = 0;
    g.state = ARCADE_EAT_RESULT;
    eat_record();
}

static void eat_logic(void)
{
    g.hold_ms += EAT_STEP_MS;

    if (g.state != ARCADE_EAT_ROLLING) {
        return;
    }

    /* 按住时长到了就自动松手（`arcade eat hold` / 自动连抽） */
    if (s_hold_target_ms > 0 && g.hold_ms >= (uint32_t)s_hold_target_ms) {
        s_release_req = true;
        return;
    }

    /*
     * 按住超过 3 秒还没松手 = 想退出（自己数，不用驱动的 1.6s LONG_HOLD）。
     * 单应用固件里 EAT_EXIT_HOLD_MS 是 0xFFFFFFFF，这段永远不成立——
     * 用户要求「长按不关机」，按住多久都继续转。
     */
    if (g.hold_ms >= EAT_EXIT_HOLD_MS) {
        s_exit_req = true;
        return;
    }

    g.roll_acc += EAT_STEP_MS;
    while (g.roll_acc >= EAT_ROLL_MS) {
        g.roll_acc -= EAT_ROLL_MS;
        eat_advance();
    }
}

/* ---------------- 渲染 ---------------- */

static void eat_prepare_geom(void)
{
    for (int y = 0; y < EAT_DST; y++) {
        s_row[y] = (uint8_t)((y * 2) / 3);
    }

    s_geom_ready = true;
}

/* 把一帧 160x160 索引图按 1.5 倍最近邻铺满 240x240 */
static void eat_blit(uint16_t *fb, const uint8_t *fr)
{
    const uint16_t *pal = (const uint16_t *)(const void *)fr;
    const uint8_t *src = fr + EAT_PALETTE;

    for (int y = 0; y < EAT_DST; y++) {
        const uint8_t *sp = src + (int)s_row[y] * EAT_SRC;
        uint16_t *dp = fb + y * EAT_DST;

        /* 2 个源像素 → 3 个目标像素，横向一次搞定 */
        for (int k = 0; k < EAT_SRC / 2; k++) {
            uint16_t a = pal[sp[2 * k]];
            uint16_t b = pal[sp[2 * k + 1]];

            dp[3 * k] = a;
            dp[3 * k + 1] = b;
            dp[3 * k + 2] = b;
        }
    }
}

static void eat_draw_blank(uint16_t *fb)
{
    /* 素材缺失兜底：黑底 + 提示，别留一块没画过的花屏 */
    arcade_gfx_fill(fb, 0, 0, EAT_DST, EAT_DST, C_INK);
    arcade_gfx_text_center(fb, 104, "NO ASSET", 3, C_BAR_FG);
    arcade_gfx_text_center(fb, 148, "arcade eat", 1, C_BAR_FG);
}

/*
 * 待机画面。
 *
 * 刻意不铺素材首帧：素材底部的文字（店名/菜名）会被提示条盖住，
 * 而且开机第一眼应该是「怎么玩」，不是某一张还没抽中的图。
 * 中文字库没编进这个固件（只有 5x7 点阵 ASCII），所以提示用英文短句。
 * NEXT 那一行就是「下次按下会抽第几组」。
 */
static void eat_idle_screen(uint16_t *fb)
{
    arcade_gfx_fill(fb, 0, 0, EAT_DST, EAT_DST, C_INK);
    arcade_gfx_text_center(fb, 58, "EAT  WHAT ?", 3, C_BAR_FG);
    arcade_gfx_text_center(fb, 120, "HOLD  THE  KEY", 2, C_BAR_FG);
    arcade_gfx_text_center(fb, 168, "RELEASE TO STOP", 1, C_DIM);
    arcade_gfx_text_center(fb, 188,
                           s_next_type == 0 ? "NEXT 1 : PLACE" : "NEXT 2 : DISH",
                           1, C_DIM);
}

static void eat_render(uint16_t *fb)
{
    if (!s_geom_ready) {
        eat_prepare_geom();
    }

    /* 待机：什么都不铺素材，只显示「按住 / 松开」和下一组 */
    if (g.state == ARCADE_EAT_IDLE) {
        eat_idle_screen(fb);
        return;
    }

    const uint8_t *fr = frame_ptr(g.type, g.frame);

    if (fr == NULL) {
        eat_draw_blank(fb);
        return;
    }

    eat_blit(fb, fr);

    if (g.state == ARCADE_EAT_ROLLING) {
        /* 滚动中：屏幕最下面一条细红条，表示「还在转」 */
        arcade_gfx_fill(fb, 0, EAT_DST - 4, EAT_DST, 4, C_STRIP);
    }
}

/* ---------------- 对外接口 ---------------- */

void arcade_eat_enter(void)
{
    memset(&g, 0, sizeof(g));

    s_frames[0] = asset_frames(0);
    s_frames[1] = asset_frames(1);

    g.state = ARCADE_EAT_IDLE;
    g.type = 0;
    g.frame = 0;
    s_next_type = 0;   /* 开机第一次按住 = 第一组（去处盘），之后严格交替 */

    s_active = true;
    s_press_req = false;
    s_release_req = false;
    s_hold_target_ms = 0;
    s_exit_req = false;
    s_auto_total = 0;
    s_auto_done = 0;

    s_log_head = 0;
    s_draws = 0;
    s_hist[0] = 0;
    s_hist[1] = 0;

    s_logic_frames = 0;
    s_render_frames = 0;
    s_skipped_frames = 0;
    s_rate_tick = 0;
    s_rate_logic = 0;
    s_rate_render = 0;

    /* 种子混一个真实的时基，否则每次开机抽出来的是同一串 */
    g.rng = arcade_cycles() ^ ((uint32_t)xTaskGetTickCount() * 2654435761U);
    if (g.rng == 0) {
        g.rng = 0x9E3779B9U;
    }

    if (!s_geom_ready) {
        eat_prepare_geom();
    }

    LISA_LOGI(TAG, "enter (g1=%d frames, g2=%d frames)", s_frames[0], s_frames[1]);
}

void arcade_eat_leave(void)
{
    s_active = false;
    s_press_req = false;
    s_release_req = false;
    s_hold_target_ms = 0;
    s_exit_req = false;
    s_auto_total = 0;
    s_auto_done = 0;
}

bool arcade_eat_exit_requested(void)
{
    if (!s_exit_req) {
        return false;
    }

    s_exit_req = false;
    return true;
}

bool arcade_eat_active(void)
{
    return s_active;
}

void arcade_eat_press(void)
{
    s_press_req = true;
}

void arcade_eat_release(void)
{
    s_release_req = true;
}

void arcade_eat_hold(int ms)
{
    if (g.state == ARCADE_EAT_ROLLING) {
        return;
    }

    s_hold_target_ms = ms > 0 ? ms : 1;
    s_press_req = true;
}

void arcade_eat_auto(int n)
{
    if (n <= 0) {
        s_auto_total = 0;
        s_auto_done = 0;
        return;
    }

    s_auto_total = n;
    s_auto_done = 0;
}

bool arcade_eat_auto_pending(void)
{
    return s_auto_total > 0;
}

int arcade_eat_auto_done(void)
{
    return (int)s_auto_done;
}

void arcade_eat_reset(void)
{
    g.state = ARCADE_EAT_IDLE;
    g.type = 0;
    g.frame = 0;
    g.hold_ms = 0;
    g.roll_acc = 0;
    g.advances = 0;
    s_next_type = 0;

    s_draws = 0;
    s_hist[0] = 0;
    s_hist[1] = 0;
    s_log_head = 0;

    s_press_req = false;
    s_release_req = false;
    s_hold_target_ms = 0;
    s_exit_req = false;
    s_auto_total = 0;
    s_auto_done = 0;
}

void arcade_eat_tick(void)
{
    if (!s_active) {
        return;
    }

    /*
     * 自动连抽：上一次定格之后马上开始下一次。
     * 每次按住时长随机，于是停在哪个帧也是随机的。
     */
    if (s_auto_total > 0 && s_hold_target_ms == 0 && !s_press_req &&
        g.state != ARCADE_EAT_ROLLING) {
        if (s_auto_done >= s_auto_total) {
            s_auto_total = 0;
        } else {
            s_hold_target_ms = (int32_t)(150 + (eat_rand() % 400));
            s_press_req = true;
        }
    }

    if (s_press_req) {
        s_press_req = false;
        eat_begin_roll();
    }
    if (s_release_req) {
        s_release_req = false;
        eat_stop_roll();
    }

    eat_logic();

    /* 渲染：取不到空缓冲就跳过这一帧（面板留着上一帧），逻辑不受影响 */
    uint16_t *fb = arcade_video_acquire_full();

    if (fb != NULL) {
        eat_render(fb);
        arcade_video_submit_full();
        s_render_frames++;
        s_rate_render++;
    } else {
        s_skipped_frames++;
    }

    s_logic_frames++;
    s_rate_logic++;

    TickType_t now = xTaskGetTickCount();

    if (s_rate_tick == 0) {
        s_rate_tick = now;
    } else if ((uint32_t)(now - s_rate_tick) >= pdMS_TO_TICKS(1000)) {
        uint32_t elapsed = (uint32_t)(now - s_rate_tick);

        s_logic_fps_x10 = (s_rate_logic * 10000U) / elapsed;
        s_render_fps_x10 = (s_rate_render * 10000U) / elapsed;
        s_rate_logic = 0;
        s_rate_render = 0;
        s_rate_tick = now;
    }
}

/* ---------------- 调试读取 ---------------- */

arcade_eat_state_t arcade_eat_state(void)
{
    return (arcade_eat_state_t)g.state;
}

int arcade_eat_type(void)
{
    return (int)g.type;
}

int arcade_eat_frame(void)
{
    return (int)g.frame;
}

int arcade_eat_next_type(void)
{
    return (int)s_next_type;
}

int arcade_eat_frame_count(int type)
{
    if (type < 0 || type > 1) {
        return 0;
    }

    return s_frames[type];
}

int arcade_eat_hold_ms_cur(void)
{
    return (int)g.hold_ms;
}

uint32_t arcade_eat_draws(void)
{
    return s_draws;
}

uint32_t arcade_eat_hist(int type)
{
    if (type < 0 || type > 1) {
        return 0;
    }

    return s_hist[type];
}

bool arcade_eat_log(uint32_t i, arcade_eat_pick_t *out)
{
    uint32_t kept = s_draws < ARCADE_EAT_LOG_MAX ? s_draws : ARCADE_EAT_LOG_MAX;
    uint32_t base;
    uint32_t idx;

    if (out == NULL || i >= kept) {
        return false;
    }

    /* s_log_head 指向「下一次要写的位置」，最旧的一条就在它那里 */
    base = (s_draws < ARCADE_EAT_LOG_MAX) ? 0U : s_log_head;
    idx = (base + i) % ARCADE_EAT_LOG_MAX;

    *out = s_log[idx];
    return true;
}

uint32_t arcade_eat_logic_fps_x10(void)
{
    return s_logic_fps_x10;
}

uint32_t arcade_eat_render_fps_x10(void)
{
    return s_render_fps_x10;
}

uint32_t arcade_eat_frames(void)
{
    return s_logic_frames;
}

uint32_t arcade_eat_renders(void)
{
    return s_render_frames;
}

uint32_t arcade_eat_skips(void)
{
    return s_skipped_frames;
}
