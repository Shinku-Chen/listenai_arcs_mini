/*
 * 内置小游戏：断网小恐龙。
 *
 * 为什么要自己写一个原生小游戏：
 *   GB/GBC 模拟器在这颗芯片上已经到极限了（模拟一帧要 17ms，比 60fps 的
 *   16.67ms 预算还多一点），再往上榨收益极小。而原生小游戏不需要解释器，
 *   一帧只是一次全屏填充 + 几十个小矩形，随便都能跑满 60Hz。
 *
 * 和模拟器共用的部分（都不重写）：
 *   - 显示：arcade_video 的三块目标缓冲 + DMA 送屏任务 → 这里是
 *     arcade_video_acquire_full()/submit_full()，直接往 240x240 的
 *     RGB565 缓冲里画，取不到空缓冲就跳过这一帧（面板保留上一帧）。
 *   - 节拍：arcade_app.c 主循环的绝对 rdcycle 节拍，锁 16.743ms。
 *   - 输入：物理单键 → arcade_input_tap(ARCADE_KEY_A) → 这里读点位图判边沿。
 *
 * 所有物理量都用定点整数（RV32 没有 FPU，float 会变成软件浮点，慢几十倍）：
 *   - 速度 speed_q16：Q16 的「像素/帧」
 *   - 位置 x_q8 / 高度 y_q8：Q8 的「像素」
 *   - 垂直速度 vy_q8：Q8 的「像素/帧」，重力每帧减 GRAV_Q8
 * 这样一帧的全部运算是几十条整数加减移位，没有除法、没有软浮点。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_kv.h"
#include "lisa_log.h"

#include "arcade_dino.h"
#include "arcade_video.h"

#define TAG "arcade-dino"

/* KV 里存最高分（重启后保留） */
#define KV_KEY_DINO_BEST   "arcade.dino.best"

/* ---------------- 画面几何 ---------------- */

#define DINO_SCREEN_W   240
#define DINO_SCREEN_H   240
#define DINO_GROUND_Y   200     /* 地平线 = 恐龙脚底 */
#define DINO_SPRITE_W   24
#define DINO_SPRITE_H   26
#define DINO_X          26

/* RGB565 常量（r/g/b 各 8 位 → 565） */
#define RGB565(r, g, b)                                     \
    ((uint16_t)((((r) * 31 / 255) << 11) |                  \
                (((g) * 63 / 255) << 5) |                   \
                ((b) * 31 / 255)))

#define C_SKY     RGB565(246, 246, 247)
#define C_INK     RGB565( 52,  58,  62)     /* 恐龙 / 障碍 */
#define C_LINE    RGB565(148, 152, 156)     /* 地平线 / 地面纹理 */
#define C_CLOUD   RGB565(208, 212, 216)
#define C_DIM     RGB565(136, 140, 144)     /* HI / 提示文字 */

/* ---------------- 物理参数 ---------------- */

/*
 * 速度用「像素/帧」的 Q16：514 ≈ 120px/s，1372 ≈ 320px/s（按 59.7fps）。
 * 每帧加 61 → 858 帧（≈14.4 秒）从起始爬到封顶。
 */
#define SPEED_START_Q16   ((int32_t)514 << 8)
#define SPEED_MAX_Q16     ((int32_t)1372 << 8)
#define SPEED_RAMP_Q16    61

/*
 * 跳跃：v0=2275、g=169（都是 Q8 像素/帧 与 Q8 像素/帧²）
 *   → 峰值高度 = v0²/(2g) ≈ 59.8px，滞空 2*v0/g ≈ 27 帧 ≈ 0.45 秒
 */
#define JUMP_V0_Q8        2275
#define GRAV_Q8            169
#define FAST_FALL_Q8       900      /* 空中再点一次：立刻加速下坠 */

/* 障碍：小的 10x22，大的用同一张图放大 2 倍（20x44） */
#define OBS_MAX           4
#define OBS_SMALL_W       10
#define OBS_SMALL_H       22
#define OBS_BIG_SCALE     2

#define CLOUD_MAX         3

/* 死亡后至少这么多帧才接受重开，避免那一下手抖连点直接重开 */
#define DEAD_LOCK_FRAMES  24

typedef struct {
    int32_t x_q8;      /* 左边缘，Q8 像素 */
    int16_t w;
    int16_t h;
} dino_obs_t;

typedef struct {
    int32_t x_q8;
    int16_t y;
} dino_cloud_t;

static struct {
    uint8_t state;          /* arcade_dino_state_t */
    uint8_t airborne;
    int32_t y_q8;           /* 恐龙离地高度 */
    int32_t vy_q8;          /* 垂直速度（向上为正） */
    int32_t scroll_q8;      /* 已跑距离 */
    int32_t speed_q16;
    int32_t spawn_q8;       /* 到下一次生成障碍还要跑多远 */
    uint32_t score;
    uint32_t best;
    uint32_t anim;          /* 跑步动画计时（帧） */
    uint32_t dead_frames;
    uint32_t rng;
    dino_obs_t obs[OBS_MAX];
    dino_cloud_t cloud[CLOUD_MAX];
} g;

static bool s_active;
static bool s_auto;
static volatile bool s_jump_req;

/* 自己的一份 fps 统计（`arcade dino stat` 用，不依赖模拟器那套） */
static uint32_t s_logic_frames;
static uint32_t s_render_frames;
static uint32_t s_skipped_frames;
static uint32_t s_logic_fps_x10;
static uint32_t s_render_fps_x10;
static TickType_t s_rate_tick;
static uint32_t s_rate_logic;
static uint32_t s_rate_render;

/* ---------------- 5x7 点阵字 ---------------- */
/* 每字节一列，bit0 = 最上面一行（经典 5x7 LCD 字模） */

static const uint8_t FONT_DIGIT[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E},   /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00},   /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46},   /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31},   /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10},   /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39},   /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30},   /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03},   /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36},   /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E},   /* 9 */
};

static const uint8_t FONT_ALPHA[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E},   /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36},   /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22},   /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C},   /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41},   /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01},   /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A},   /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F},   /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00},   /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01},   /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41},   /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40},   /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},   /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F},   /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E},   /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06},   /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E},   /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46},   /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31},   /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01},   /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F},   /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F},   /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F},   /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63},   /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07},   /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43},   /* Z */
};

static const uint8_t FONT_DASH[5]  = {0x08, 0x08, 0x08, 0x08, 0x08};
static const uint8_t FONT_COLON[5] = {0x00, 0x36, 0x36, 0x00, 0x00};

static const uint8_t *font_glyph(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return FONT_DIGIT[ch - '0'];
    }
    if (ch >= 'A' && ch <= 'Z') {
        return FONT_ALPHA[ch - 'A'];
    }
    if (ch >= 'a' && ch <= 'z') {
        return FONT_ALPHA[ch - 'a'];
    }
    if (ch == '-') {
        return FONT_DASH;
    }
    if (ch == ':') {
        return FONT_COLON;
    }

    return NULL;
}

/* ---------------- 精灵图 ---------------- */
/* '#' = 上色，其它 = 透明。行宽固定 DINO_SPRITE_W。 */

static const char *const DINO_BODY[DINO_SPRITE_H - 6] = {
    "..................#####.",
    ".................#######",
    ".................##.####",
    ".................#######",
    ".................#######",
    "..................######",
    "..................######",
    "..................#####.",
    "....##...........######.",
    "...####.........########",
    "..#####........#########",
    ".######.......##########",
    ".#######################",
    "..######################",
    "...###################..",
    "....################....",
    ".....##############.....",
    "......############......",
    "......##########........",
    "......########..........",
};

static const char *const DINO_LEGS_A[6] = {
    "......##.....###........",
    "......##.....###........",
    "......##.....###........",
    "......##......###.......",
    ".....##........###......",
    "....###........####.....",
};

static const char *const DINO_LEGS_B[6] = {
    "......##.....###........",
    "......##.....###........",
    "......##.....####.......",
    "......###.....###.......",
    "......###......###......",
    ".....####........##.....",
};

/* 仙人掌（10 宽 x 22 高），大的按 2 倍最近邻放大 */
static const char *const CACTUS[OBS_SMALL_H] = {
    "....##....",
    "....##....",
    ".#..##..#.",
    ".#..##..#.",
    ".##.##.##.",
    "..#####.#.",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
    "....##....",
};

/* ---------------- 绘制基础 ---------------- */

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > DINO_SCREEN_W) {
        w = DINO_SCREEN_W - x;
    }
    if (y + h > DINO_SCREEN_H) {
        h = DINO_SCREEN_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    uint32_t pair = ((uint32_t)color << 16) | color;

    for (int row = 0; row < h; row++) {
        uint16_t *d = fb + (size_t)(y + row) * DINO_SCREEN_W + x;
        int n = w;

        /* 32 位写要 4 字节对齐：列号为奇数时先单独写一个像素 */
        if (((uintptr_t)d & 3U) != 0U) {
            *d++ = color;
            n--;
        }

        uint32_t *p = (uint32_t *)d;
        int nw = n / 2;

        for (int i = 0; i < nw; i++) {
            p[i] = pair;
        }
        if (n & 1) {
            d[n - 1] = color;
        }
    }
}

static void draw_sprite(uint16_t *fb, int x, int y, const char *const *rows, int nrows,
                        int scale, uint16_t color)
{
    for (int r = 0; r < nrows; r++) {
        const char *line = rows[r];

        for (int c = 0; line[c] != '\0'; c++) {
            if (line[c] == '#') {
                fill_rect(fb, x + c * scale, y + r * scale, scale, scale, color);
            }
        }
    }
}

static void draw_char(uint16_t *fb, int x, int y, char ch, int scale, uint16_t color)
{
    const uint8_t *cols = font_glyph(ch);

    if (cols == NULL) {
        return;
    }

    for (int c = 0; c < 5; c++) {
        uint8_t bits = cols[c];

        for (int r = 0; r < 7; r++) {
            if ((bits & (1U << r)) != 0U) {
                fill_rect(fb, x + c * scale, y + r * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(uint16_t *fb, int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s != '\0'; s++) {
        draw_char(fb, x, y, *s, scale, color);
        x += 6 * scale;
    }
}

static int text_width(const char *s, int scale)
{
    int n = (int)strlen(s);

    return n > 0 ? n * 6 * scale - scale : 0;
}

static void draw_text_center(uint16_t *fb, int y, const char *s, int scale, uint16_t color)
{
    draw_text(fb, (DINO_SCREEN_W - text_width(s, scale)) / 2, y, s, scale, color);
}

/* ---------------- 游戏逻辑 ---------------- */

static uint32_t dino_rand(void)
{
    /* 线性同余，够用且没有除法 */
    g.rng = g.rng * 1103515245U + 12345U;
    return g.rng >> 16;
}

static int dino_speed_px_s(void)
{
    return (int)((g.speed_q16 * 60) >> 16);
}

static void dino_reset(void)
{
    uint32_t best = g.best;

    memset(&g, 0, sizeof(g));

    g.state = ARCADE_DINO_RUN;
    g.best = best;
    g.speed_q16 = SPEED_START_Q16;
    /* 第一根障碍从右边缘外进来，跑到恐龙面前约 1.8 秒，给个准备时间 */
    g.spawn_q8 = (int32_t)(DINO_SCREEN_W + 24) << 8;
    g.rng = 0x13579BDFU;

    for (int i = 0; i < CLOUD_MAX; i++) {
        g.cloud[i].x_q8 = (int32_t)(30 + i * 92) << 8;
        g.cloud[i].y = (int16_t)(34 + (i * 27) % 46);
    }

    s_logic_frames = 0;
    s_render_frames = 0;
    s_skipped_frames = 0;
    s_rate_tick = 0;
    s_rate_logic = 0;
    s_rate_render = 0;
}

static void dino_load_best(void)
{
    int v = 0;

    if (lisa_kv_get_int(KV_KEY_DINO_BEST, &v) == 0 && v > 0) {
        g.best = (uint32_t)v;
    }
}

static void dino_save_best(void)
{
    (void)lisa_kv_set_int(KV_KEY_DINO_BEST, (int)g.best);
}

static void dino_spawn_obstacle(void)
{
    int slot = -1;

    for (int i = 0; i < OBS_MAX; i++) {
        if (g.obs[i].w == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }

    /*
     * 大仙人掌只在速度够高时才出。
     *
     * 这不是拍脑袋的难度曲线，是算出来的：恐龙跳过 44px 高的障碍时，
     * 「脚底高于 44px」只有约 14.7 帧（起跳后第 6.1~20.8 帧）。而一个
     * 20px 宽的大仙人掌扫过恐龙 16px 的命中盒需要 (20+16)/dx 帧——
     * dx=2px/帧（120px/s）时要 18 帧，比 14.7 还长，**低速度下无论怎么跳都过不去**。
     *
     * 所以 200px/s（dx≈3.35，扫过 10 帧）以下只出小仙人掌；
     * 之后概率缓缓升到 40%，和速度一起加难度。
     */
    int speed_px = dino_speed_px_s();
    bool big = false;

    if (speed_px >= 200) {
        int chance = (speed_px - 200) / 3;

        if (chance > 60) {
            chance = 60;
        }
        big = (dino_rand() % 100U) < (uint32_t)chance;
    }

    dino_obs_t *o = &g.obs[slot];

    o->w = (int16_t)(big ? OBS_SMALL_W * OBS_BIG_SCALE : OBS_SMALL_W);
    o->h = (int16_t)(big ? OBS_SMALL_H * OBS_BIG_SCALE : OBS_SMALL_H);
    o->x_q8 = (int32_t)DINO_SCREEN_W << 8;

    /*
     * 下一个障碍的间距：跟着速度线性拉长，保证跳跃够用。
     * 跳跃滞空 27 帧，飞过 27*dx 像素；间距必须比这个大才能过。
     */
    int gap = 150 + (speed_px * 45) / 100 + (int)(dino_rand() % 130U);

    g.spawn_q8 = (int32_t)(gap + o->w) << 8;
}

static void dino_do_jump(void)
{
    if (g.state == ARCADE_DINO_DEAD) {
        if (g.dead_frames >= DEAD_LOCK_FRAMES) {
            dino_reset();
        }
        return;
    }

    if (!g.airborne) {
        g.airborne = 1;
        g.vy_q8 = JUMP_V0_Q8;
    } else {
        /* 空中再点一次：加速下坠（长按落地的手感） */
        g.vy_q8 -= FAST_FALL_Q8;
    }
}

static bool dino_hit_test(void)
{
    int y_px = g.y_q8 >> 8;
    int feet = DINO_GROUND_Y - y_px;
    int d_top = feet - (DINO_SPRITE_H - 3);
    int d_left = DINO_X + 7;
    int d_right = DINO_X + DINO_SPRITE_W - 1;

    for (int i = 0; i < OBS_MAX; i++) {
        const dino_obs_t *o = &g.obs[i];

        if (o->w == 0) {
            continue;
        }

        int o_left = o->x_q8 >> 8;
        int o_right = o_left + o->w;
        int o_bottom = DINO_GROUND_Y + 1;
        int o_top = o_bottom - o->h;

        /* 命中盒两边各内缩 1~2px，比看着撞上宽松一点，手感更好 */
        if (d_right > o_left + 1 && d_left < o_right - 1 &&
            feet > o_top + 1 && d_top < o_bottom) {
            return true;
        }
    }

    return false;
}

static void dino_auto_jump(void)
{
    /*
     * 注意：必须在空中直接返回。dino_do_jump() 在空中的语义是「加速下坠」，
     * 而自动跳的触发条件在空中仍然成立 —— 不拦住就会每帧往下砸 900，
     * 恐龙刚离地就被拍回地面（实测就是「auto=1 但第一根障碍就撞上」）。
     */
    if (g.airborne) {
        return;
    }

    /*
     * 起跳时机用「还有几帧撞上」算，和速度无关：
     * 障碍扫过恐龙命中盒需要 (w+16)/dx 帧，必须在「脚底高于障碍顶部」的
     * 窗口内完成。小仙人掌（22px 高）窗口很宽，提前 12 帧跳就够；
     * 大仙人掌（44px 高）窗口只有 14.7 帧，得提前 8 帧跳、把它夹在峰值附近。
     */
    int dino_right = DINO_X + DINO_SPRITE_W;
    int32_t reach_q8 = ((int32_t)DINO_SCREEN_W << 8);   /* 找一个最近的 */
    int32_t reach_w = 0;

    for (int i = 0; i < OBS_MAX; i++) {
        const dino_obs_t *o = &g.obs[i];

        if (o->w == 0) {
            continue;
        }
        if ((o->x_q8 >> 8) + o->w < dino_right) {
            continue;   /* 已经过去了 */
        }
        if (o->x_q8 < reach_q8) {
            reach_q8 = o->x_q8;
            reach_w = o->w;
        }
    }

    if (reach_q8 >= ((int32_t)DINO_SCREEN_W << 8)) {
        return;
    }

    int32_t frames = (reach_w > OBS_SMALL_W) ? 8 : 12;
    int32_t trigger_q8 = (g.speed_q16 >> 8) * frames;

    if (reach_q8 - ((int32_t)dino_right << 8) <= trigger_q8) {
        dino_do_jump();
    }
}

static void dino_logic(void)
{
    g.anim++;

    if (g.state == ARCADE_DINO_DEAD) {
        if (g.dead_frames < 0xFFFFU) {
            g.dead_frames++;
        }
        return;
    }

    int32_t dx = g.speed_q16 >> 8;   /* Q8 像素/帧 */

    /* 速度爬坡（到顶就停） */
    if (g.speed_q16 < SPEED_MAX_Q16) {
        g.speed_q16 += SPEED_RAMP_Q16;
        if (g.speed_q16 > SPEED_MAX_Q16) {
            g.speed_q16 = SPEED_MAX_Q16;
        }
    }

    g.scroll_q8 += dx;
    g.score = (uint32_t)((g.scroll_q8 >> 8) / 20);

    /* 垂直运动 */
    if (g.airborne) {
        g.vy_q8 -= GRAV_Q8;
        g.y_q8 += g.vy_q8;
        if (g.y_q8 <= 0) {
            g.y_q8 = 0;
            g.vy_q8 = 0;
            g.airborne = 0;
        }
    }

    /* 障碍前进 / 回收 */
    for (int i = 0; i < OBS_MAX; i++) {
        dino_obs_t *o = &g.obs[i];

        if (o->w == 0) {
            continue;
        }

        o->x_q8 -= dx;

        if ((o->x_q8 >> 8) + o->w < 0) {
            o->w = 0;
        }
    }

    /* 生成下一个障碍 */
    g.spawn_q8 -= dx;
    if (g.spawn_q8 <= 0) {
        dino_spawn_obstacle();
    }

    /* 云：只是装饰，飘得比地面慢 */
    for (int i = 0; i < CLOUD_MAX; i++) {
        dino_cloud_t *c = &g.cloud[i];

        c->x_q8 -= dx / 6;
        if ((c->x_q8 >> 8) < -(40)) {
            c->x_q8 = (int32_t)(DINO_SCREEN_W + 20 + (int)(dino_rand() % 60U)) << 8;
            c->y = (int16_t)(30 + (dino_rand() % 50U));
        }
    }

    if (s_auto) {
        dino_auto_jump();
    }
    if (dino_hit_test()) {
        g.state = ARCADE_DINO_DEAD;
        g.dead_frames = 0;
        if (g.score > g.best) {
            g.best = g.score;
            dino_save_best();
        }
        LISA_LOGI(TAG, "game over: score=%u best=%u", (unsigned)g.score,
                  (unsigned)g.best);
    }
}

/* ---------------- 渲染 ---------------- */

static void dino_render(uint16_t *fb)
{
    /* 整屏铺底色：天空和地面同色，靠地平线分界 */
    fill_rect(fb, 0, 0, DINO_SCREEN_W, DINO_SCREEN_H, C_SKY);
    fill_rect(fb, 0, DINO_GROUND_Y, DINO_SCREEN_W, 1, C_LINE);

    /* 地面纹理：跟着滚动走的两段短线 */
    {
        int off = ((int)(g.scroll_q8 >> 8)) % 96;

        for (int i = -1; i <= DINO_SCREEN_W / 96 + 1; i++) {
            int x = i * 96 - off;

            fill_rect(fb, x + 16, DINO_GROUND_Y + 9, 13, 2, C_LINE);
            fill_rect(fb, x + 56, DINO_GROUND_Y + 21, 9, 2, C_LINE);
        }
    }

    /* 云 */
    for (int i = 0; i < CLOUD_MAX; i++) {
        int x = g.cloud[i].x_q8 >> 8;
        int y = g.cloud[i].y;

        fill_rect(fb, x + 7, y, 17, 5, C_CLOUD);
        fill_rect(fb, x, y + 5, 30, 5, C_CLOUD);
        fill_rect(fb, x + 11, y - 4, 9, 5, C_CLOUD);
    }

    /* 障碍 */
    for (int i = 0; i < OBS_MAX; i++) {
        const dino_obs_t *o = &g.obs[i];

        if (o->w == 0) {
            continue;
        }

        int x = o->x_q8 >> 8;
        int y = DINO_GROUND_Y - o->h;
        int scale = (o->w > OBS_SMALL_W) ? OBS_BIG_SCALE : 1;

        draw_sprite(fb, x, y, CACTUS, OBS_SMALL_H, scale, C_INK);
    }

    /* 恐龙：空中用固定的跳跃姿势，地面每 5 帧换一次腿 */
    {
        int y = DINO_GROUND_Y - (g.y_q8 >> 8) - DINO_SPRITE_H;
        const char *const *legs;

        if (g.airborne) {
            legs = DINO_LEGS_A;
        } else {
            legs = ((g.anim / 5) & 1U) ? DINO_LEGS_B : DINO_LEGS_A;
        }

        if (g.state == ARCADE_DINO_DEAD) {
            legs = DINO_LEGS_A;
        }

        draw_sprite(fb, DINO_X, y, DINO_BODY, DINO_SPRITE_H - 6, 1, C_INK);
        draw_sprite(fb, DINO_X, y + DINO_SPRITE_H - 6, legs, 6, 1, C_INK);
    }

    /* 分数：HI 在左上、当前分在右上（和 Chrome 那只一样的位置感） */
    {
        char buf[16];

        snprintf(buf, sizeof(buf), "HI %05u", (unsigned)g.best);
        draw_text(fb, 8, 12, buf, 2, C_DIM);

        snprintf(buf, sizeof(buf), "%05u", (unsigned)g.score);
        draw_text(fb, DINO_SCREEN_W - 8 - text_width(buf, 3), 8, buf, 3, C_INK);
    }

    if (g.state == ARCADE_DINO_DEAD) {
        char buf[24];

        draw_text_center(fb, 96, "GAME OVER", 3, C_INK);

        snprintf(buf, sizeof(buf), "SCORE %05u", (unsigned)g.score);
        draw_text_center(fb, 130, buf, 2, C_INK);

        draw_text_center(fb, 156, "PRESS TO RETRY", 1, C_DIM);
    }
}

/* ---------------- 对外接口 ---------------- */

void arcade_dino_enter(void)
{
    dino_load_best();
    s_active = true;
    s_jump_req = false;
    dino_reset();

    LISA_LOGI(TAG, "enter (best=%u)", (unsigned)g.best);
}

void arcade_dino_leave(void)
{
    s_active = false;
    s_auto = false;
    s_jump_req = false;
}

bool arcade_dino_active(void)
{
    return s_active;
}

void arcade_dino_click(void)
{
    s_jump_req = true;
}

void arcade_dino_set_auto(bool on)
{
    s_auto = on;
}

bool arcade_dino_auto(void)
{
    return s_auto;
}

void arcade_dino_reset(void)
{
    dino_reset();
}

void arcade_dino_tick(void)
{
    if (!s_active) {
        return;
    }

    if (s_jump_req) {
        s_jump_req = false;
        dino_do_jump();
    }

    dino_logic();

    /* 渲染：取不到空缓冲就跳过这一帧（面板留着上一帧），逻辑不受影响 */
    uint16_t *fb = arcade_video_acquire_full();

    if (fb != NULL) {
        dino_render(fb);
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

arcade_dino_state_t arcade_dino_state(void)
{
    return (arcade_dino_state_t)g.state;
}

uint32_t arcade_dino_score(void)
{
    return g.score;
}

uint32_t arcade_dino_best(void)
{
    return g.best;
}

int arcade_dino_height(void)
{
    return (int)(g.y_q8 >> 8);
}

bool arcade_dino_airborne(void)
{
    return g.airborne != 0;
}

int arcade_dino_obstacle_count(void)
{
    int n = 0;

    for (int i = 0; i < OBS_MAX; i++) {
        if (g.obs[i].w != 0) {
            n++;
        }
    }

    return n;
}

int arcade_dino_obstacle_x(int index)
{
    int n = 0;

    for (int i = 0; i < OBS_MAX; i++) {
        if (g.obs[i].w == 0) {
            continue;
        }
        if (n == index) {
            return g.obs[i].x_q8 >> 8;
        }
        n++;
    }

    return -1;
}

int arcade_dino_speed_px(void)
{
    return dino_speed_px_s();
}

uint32_t arcade_dino_logic_fps_x10(void)
{
    return s_logic_fps_x10;
}

uint32_t arcade_dino_render_fps_x10(void)
{
    return s_render_fps_x10;
}

uint32_t arcade_dino_frames(void)
{
    return s_logic_frames;
}

uint32_t arcade_dino_renders(void)
{
    return s_render_frames;
}

uint32_t arcade_dino_skips(void)
{
    return s_skipped_frames;
}
