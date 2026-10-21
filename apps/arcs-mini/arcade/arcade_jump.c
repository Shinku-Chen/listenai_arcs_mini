/*
 * 内置小游戏：跳一跳（微信那个）。
 *
 * 为什么自己写而不是塞进模拟器：见 arcade_dino.c 顶部的说明 —— 这颗芯片上
 * 解释一帧 GB 指令要 17ms，而原生小游戏一帧只是几十个矩形，随便跑满 60Hz。
 *
 * 视角：等距 2.5D。平台画成 2:1 的菱形顶面 + 左右两侧壁（立方体），跳跃方向
 * 沿菱形的一条对角边（屏上是 (±1, -0.5) 的方向），所以看上去像斜着跳出去。
 *
 * 相机：当前平台永远画在固定锚点 (ANCHOR_X, ANCHOR_Y)，下一块按 gap 偏移。
 * 落地后 PAN 的 12 帧里，整个场景按偏移量反向插值，把新平台挪到锚点上，
 * 然后「换庄」：next 变 cur，再造一块新的 next。
 *
 * 全部定点整数（RV32 没有 FPU）：
 *   - gap / 距离 / 位置都是「沿轴像素」，只在画的时候投影成 (x, y)
 *   - 蓄力时长 → 跳跃距离是线性映射，蓄满 1000ms = 140px
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_kv.h"
#include "lisa_log.h"

#include "arcade_gfx.h"
#include "arcade_jump.h"
#include "arcade_video.h"

#define TAG "arcade-jump"

/* KV 里存最高分（重启后保留） */
#define KV_KEY_JUMP_BEST   "arcade.jump.best"

/* ---------------- 画面几何 ---------------- */

#define ANCHOR_X        120     /* 当前平台中心在屏上的位置（相机跟着它走） */
#define ANCHOR_Y        170
#define PLAT_H           18     /* 立方体侧壁高度 */
#define PLAT_RX_MAX      30     /* 平台半宽上限（分数越高越小，越难站） */
#define PLAT_RX_MIN      16

#define FLY_FRAMES       24     /* 一次跳跃的滞空帧数（≈0.4 秒） */
#define FLY_H            30     /* 抛物线最高点（px） */
#define PAN_FRAMES       12     /* 落地后相机跟随帧数 */
#define SQUASH_FRAMES     6     /* 落地压缩回弹帧数 */
#define FALL_FRAMES      20     /* 踩空掉落帧数 */
#define DEAD_LOCK        24     /* 死亡后至少这么多帧才接受重开 */

#define CHARGE_MAX_MS  1000     /* 蓄满所需时长 */
#define CHARGE_STEP_MS   17     /* 每逻辑帧累积的蓄力（≈16.7ms） */
#define DIST_MIN         20     /* 蓄力 0 时的跳跃距离 */
#define DIST_MAX        140     /* 蓄满时的跳跃距离 */

#define BG_BAND           8     /* 背景渐变的行高（240/8 = 30 条） */

#define RGB565(r, g, b)  ARCADE_GFX_RGB565(r, g, b)

#define C_PLAT_TOP    RGB565(126, 138, 156)   /* 顶面 */
#define C_PLAT_L      RGB565( 74,  84, 100)   /* 左侧壁（背光） */
#define C_PLAT_R      RGB565( 96, 106, 124)   /* 右侧壁（受光） */
#define C_INK         RGB565( 44,  50,  60)
#define C_BODY        RGB565(232, 118,  86)   /* 小人身体 */
#define C_HEAD        RGB565(248, 216, 192)   /* 小人脑袋 */
#define C_SHADOW      RGB565(168, 176, 190)
#define C_BAR_BG      RGB565( 70,  78,  92)
#define C_BAR_FG      RGB565(242, 178,  58)
#define C_BAR_HOT     RGB565(238,  92,  70)
#define C_DIM         RGB565(112, 120, 134)
#define C_PERFECT     RGB565(236, 152,  40)

/* ---------------- 状态 ---------------- */

static struct {
    uint8_t state;          /* arcade_jump_state_t */
    uint8_t dir;            /* +1 = 右上，-1 = 左上 */
    uint8_t squash;         /* 落地回弹剩余帧 */
    int32_t charge_ms;      /* 本次蓄力时长 */
    int32_t gap;            /* 当前平台 → 下一平台（沿轴 px，恒为偶数） */
    int32_t rx;             /* 当前平台半宽 */
    int32_t nrx;            /* 下一平台半宽 */
    int32_t fly_dist;       /* 本跳的实际距离（沿轴 px） */
    int32_t fly_i;          /* 飞行帧计数 */
    int32_t pan_i;          /* 相机跟随帧计数 */
    int32_t fall_i;         /* 掉落帧计数 */
    int32_t land_err;       /* 上一次落点偏差（正 = 跳过头） */
    uint32_t score;
    uint32_t best;
    uint32_t combo;
    uint32_t msg_frames;    /* "PERFECT +N" 剩余显示帧 */
    uint32_t dead_frames;
    uint32_t rng;
} g;

static bool s_active;
static bool s_auto;
/* 按键请求：原始按键在按键扫描任务里落成标志，主循环（arcade 任务）里消费 */
static volatile bool s_press_req;
static volatile bool s_release_req;
/* >0 时表示「蓄力到这么多毫秒就自动松手」（调试 / 自动玩用） */
static volatile int32_t s_charge_target_ms;

/* 自己的一份 fps 统计（`arcade jump stat` 用，不依赖模拟器那套） */
static uint32_t s_logic_frames;
static uint32_t s_render_frames;
static uint32_t s_skipped_frames;
static uint32_t s_logic_fps_x10;
static uint32_t s_render_fps_x10;
static TickType_t s_rate_tick;
static uint32_t s_rate_logic;
static uint32_t s_rate_render;

/* 背景渐变，一次性算好（每 BG_BAND 行一条） */
static uint16_t s_bg[ARCADE_GFX_H / BG_BAND];
static bool s_bg_ready;

/* ---------------- 随机 / 难度 ---------------- */

static uint32_t jump_rand(void)
{
    /* 线性同余，够用且没有除法 */
    g.rng = g.rng * 1103515245U + 12345U;
    return g.rng >> 16;
}

/* 分数越高平台越窄：30px 半宽起步，每 3 分窄 1px，最窄 16px */
static int32_t platform_rx(uint32_t score)
{
    int32_t rx = PLAT_RX_MAX - (int32_t)(score / 3);

    return rx < PLAT_RX_MIN ? PLAT_RX_MIN : rx;
}

/*
 * 两块平台中心的距离（沿轴 px）。
 *
 * 一定是偶数（投影到屏上是 (±1, -0.5)，偶数才有整数 y）。间距本身随分数
 * 慢慢拉开方差，但下限固定 46px —— 平台半宽最大 30，沿轴的可落脚范围只有
 * rx/2，46px 已经足够分开两块，再小就会视觉重叠。
 */
static int32_t next_gap(uint32_t score)
{
    int32_t spread = 10 + (int32_t)(score / 2);

    if (spread > 20) {
        spread = 20;
    }

    return 2 * (23 + (int32_t)(jump_rand() % (uint32_t)spread));
}

/* 蓄力时长 → 跳跃距离（线性） */
static int32_t dist_from_charge(int32_t ms)
{
    if (ms > CHARGE_MAX_MS) {
        ms = CHARGE_MAX_MS;
    }
    if (ms < 0) {
        ms = 0;
    }

    return DIST_MIN + (ms * (DIST_MAX - DIST_MIN)) / CHARGE_MAX_MS;
}

/* 反解：跳 dist 需要蓄多久（自动玩 / 调试用） */
static int32_t charge_from_dist(int32_t dist)
{
    if (dist < DIST_MIN) {
        dist = DIST_MIN;
    }
    if (dist > DIST_MAX) {
        dist = DIST_MAX;
    }

    return ((dist - DIST_MIN) * CHARGE_MAX_MS) / (DIST_MAX - DIST_MIN);
}

/* ---------------- 局面 ---------------- */

static void jump_load_best(void)
{
    int v = 0;

    if (lisa_kv_get_int(KV_KEY_JUMP_BEST, &v) == 0 && v > 0) {
        g.best = (uint32_t)v;
    }
}

static void jump_save_best(void)
{
    (void)lisa_kv_set_int(KV_KEY_JUMP_BEST, (int)g.best);
}

static void jump_spawn_next(void)
{
    g.dir = (jump_rand() & 1U) ? 1 : -1;
    g.gap = next_gap(g.score);
    g.nrx = platform_rx(g.score);
}

static void jump_reset(void)
{
    uint32_t best = g.best;

    memset(&g, 0, sizeof(g));

    g.best = best;
    g.state = ARCADE_JUMP_READY;
    g.rx = PLAT_RX_MAX;
    g.rng = 0x2468ACE1U;
    jump_spawn_next();

    s_press_req = false;
    s_release_req = false;
    s_charge_target_ms = 0;
    s_logic_frames = 0;
    s_render_frames = 0;
    s_skipped_frames = 0;
    s_rate_tick = 0;
    s_rate_logic = 0;
    s_rate_render = 0;
}

/* ---------------- 逻辑 ---------------- */

static void jump_begin_charge(void)
{
    if (g.state == ARCADE_JUMP_DEAD) {
        /* 死亡瞬间手抖连点会立刻重开，压一压 */
        if (g.dead_frames >= DEAD_LOCK) {
            jump_reset();
        }
        return;
    }

    if (g.state != ARCADE_JUMP_READY) {
        return;
    }

    g.state = ARCADE_JUMP_CHARGE;
    g.charge_ms = 0;
}

static void jump_launch(void)
{
    if (g.state != ARCADE_JUMP_CHARGE) {
        return;
    }

    g.fly_dist = dist_from_charge(g.charge_ms);
    g.fly_i = 0;
    g.state = ARCADE_JUMP_FLY;
}

/* 落地判定 */
static void jump_resolve(void)
{
    int32_t land_r = g.nrx / 2;          /* 沿轴方向的可落脚半径 */
    int32_t perfect_r = land_r / 5;      /* 中心 20% 算「完美」 */
    int32_t err = g.fly_dist - g.gap;

    g.land_err = err;

    if (err >= -land_r && err <= land_r) {
        bool perfect = (err >= -perfect_r && err <= perfect_r);

        g.score += 1;
        if (perfect) {
            g.score += 2;                /* 踩中中心额外 +2 */
            g.combo++;
            g.msg_frames = 45;
        } else {
            g.combo = 0;
        }

        g.state = ARCADE_JUMP_PAN;
        g.pan_i = 0;
        g.squash = SQUASH_FRAMES;
        return;
    }

    g.combo = 0;
    g.state = ARCADE_JUMP_FALL;
    g.fall_i = 0;
}

/* 相机跟随结束：下一块「换庄」成为当前平台 */
static void jump_rebase(void)
{
    g.rx = g.nrx;
    g.pan_i = 0;
    g.squash = 0;
    g.charge_ms = 0;
    g.land_err = 0;
    g.state = ARCADE_JUMP_READY;
    jump_spawn_next();
}

static void jump_auto_tick(void)
{
    int32_t land_r;
    int32_t need;
    int32_t err_px;
    int32_t target;

    if (g.state != ARCADE_JUMP_READY || s_charge_target_ms > 0) {
        return;
    }

    /*
     * 按「落在平台中心附近」反算蓄力时长，再叠一个 ±land_r/4 的随机误差，
     * 这样既不会永远完美，也不会永远踩空 —— 压测时能真正跑过判定分支。
     */
    land_r = g.nrx / 2;
    need = charge_from_dist(g.gap);
    /* 误差控制在 ±land_r/2 以内：既会偶尔完美，也不会一直踩空 */
    err_px = (int32_t)(jump_rand() % (uint32_t)(land_r + 1)) - land_r / 2;
    target = need + (err_px * CHARGE_MAX_MS) / (DIST_MAX - DIST_MIN);
    if (target < 30) {
        target = 30;
    }

    s_charge_target_ms = target;
    jump_begin_charge();
}

static void jump_logic(void)
{
    if (g.msg_frames > 0) {
        g.msg_frames--;
    }

    switch (g.state) {
    case ARCADE_JUMP_CHARGE:
        g.charge_ms += CHARGE_STEP_MS;
        if (g.charge_ms >= CHARGE_MAX_MS) {
            g.charge_ms = CHARGE_MAX_MS;
        }
        if (s_charge_target_ms > 0 && g.charge_ms >= s_charge_target_ms) {
            s_charge_target_ms = 0;
            jump_launch();
        }
        break;

    case ARCADE_JUMP_FLY:
        g.fly_i++;
        if (g.fly_i >= FLY_FRAMES) {
            jump_resolve();
        }
        break;

    case ARCADE_JUMP_PAN:
        g.pan_i++;
        if (g.squash > 0) {
            g.squash--;
        }
        if (g.pan_i >= PAN_FRAMES) {
            jump_rebase();
        }
        break;

    case ARCADE_JUMP_FALL:
        g.fall_i++;
        if (g.fall_i >= FALL_FRAMES) {
            g.state = ARCADE_JUMP_DEAD;
            g.dead_frames = 0;
            if (g.score > g.best) {
                g.best = g.score;
                jump_save_best();
            }
            LISA_LOGI(TAG, "game over: score=%u best=%u err=%d gap=%d",
                      (unsigned)g.score, (unsigned)g.best, (int)g.land_err,
                      (int)g.gap);
        }
        break;

    case ARCADE_JUMP_DEAD:
        if (g.dead_frames < 0xFFFFU) {
            g.dead_frames++;
        }
        break;

    default:
        break;
    }

    if (s_auto) {
        jump_auto_tick();
    }
}

/* ---------------- 绘制 ---------------- */

static void bg_prepare(void)
{
    int n = ARCADE_GFX_H / BG_BAND;

    for (int i = 0; i < n; i++) {
        int t = (i * 255) / (n - 1);          /* 0（顶）→ 255（底） */

        s_bg[i] = RGB565(238 - (t * 34) / 255,
                         242 - (t * 30) / 255,
                         248 - (t * 22) / 255);
    }

    s_bg_ready = true;
}

/*
 * 立方体平台：顶面是 2:1 菱形，逐列画。
 * 每列先画顶面那一段（1px 宽），再从它的下边界往下画侧壁；左半背光、右半受光。
 * squash > 0 时侧壁变矮 —— 落地那一下的压缩回弹。
 */
static void draw_platform(uint16_t *fb, int cx, int cy, int rx, int squash)
{
    int ry = rx / 2;
    int wall = PLAT_H - squash;

    if (rx <= 0) {
        return;
    }
    if (wall < 4) {
        wall = 4;
    }

    for (int x = -rx; x <= rx; x++) {
        int ax = x < 0 ? -x : x;
        int dy = (ry * (rx - ax)) / rx;
        int ytop = cy - dy;
        int ybot = cy + dy;
        uint16_t side = (x < 0) ? C_PLAT_L : C_PLAT_R;

        arcade_gfx_fill(fb, cx + x, ytop, 1, ybot - ytop + 1, C_PLAT_TOP);
        arcade_gfx_fill(fb, cx + x, ybot + 1, 1, wall, side);
    }
}

/* 小人的影子：踩在平台面上的一个小菱形，跳得越高越小 */
static void draw_shadow(uint16_t *fb, int cx, int cy, int rx, int z)
{
    int r = rx - z / 3;

    if (r < 3) {
        r = 3;
    }

    arcade_gfx_fill(fb, cx - r, cy, 2 * r, 1, C_SHADOW);
    arcade_gfx_fill(fb, cx - r / 2 - 1, cy - 1, r + 2, 1, C_SHADOW);
    arcade_gfx_fill(fb, cx - r / 2 - 1, cy + 1, r + 2, 1, C_SHADOW);
}

/*
 * 小人：脚底在 (x, y)。
 * squash = 蓄力时下压的像素数（0..5），身体和脑袋一起压扁；
 * air = 空中，两条腿并起来。
 */
static void draw_player(uint16_t *fb, int x, int y, int squash, bool air)
{
    int total = 17 - squash;             /* 全身高度 */
    int head_h = 6 - squash / 2;
    int leg_h = 4;
    int top;
    int body_h;

    if (head_h < 3) {
        head_h = 3;
    }
    if (total < 9) {
        total = 9;
    }

    top = y - total;
    body_h = total - head_h - leg_h;
    if (body_h < 2) {
        body_h = 2;
    }

    arcade_gfx_fill(fb, x - 3, top, 6, head_h, C_HEAD);              /* 头 */
    arcade_gfx_fill(fb, x - 4, top + head_h, 8, body_h, C_BODY);     /* 身体 */

    if (air) {
        arcade_gfx_fill(fb, x - 2, y - leg_h, 4, leg_h, C_INK);
    } else {
        arcade_gfx_fill(fb, x - 3, y - leg_h, 2, leg_h, C_INK);
        arcade_gfx_fill(fb, x + 1, y - leg_h, 2, leg_h, C_INK);
    }
}

/* 底部蓄力条 */
static void draw_charge_bar(uint16_t *fb, int pct)
{
    int w = 150;
    int h = 7;
    int x = (ARCADE_GFX_W - w) / 2;
    int y = 224;
    int fill = (w * pct) / 100;

    arcade_gfx_fill(fb, x - 1, y - 1, w + 2, h + 2, C_INK);
    arcade_gfx_fill(fb, x, y, w, h, C_BAR_BG);
    if (fill > 0) {
        arcade_gfx_fill(fb, x, y, fill, h, pct > 80 ? C_BAR_HOT : C_BAR_FG);
    }
}

static void jump_render(uint16_t *fb)
{
    int off_x = 0;
    int off_y = 0;
    int ax;
    int ay;
    int bx;
    int by;
    int sq = 0;
    int px;
    int py;
    int pz = 0;
    int squeeze = 0;
    bool air = false;
    bool show = true;
    char buf[24];

    if (!s_bg_ready) {
        bg_prepare();
    }
    for (int i = 0; i < ARCADE_GFX_H / BG_BAND; i++) {
        arcade_gfx_fill(fb, 0, i * BG_BAND, ARCADE_GFX_W, BG_BAND, s_bg[i]);
    }

    /* 相机偏移只在「落地跟随」那一段有；其余状态整屏不动 */
    if (g.state == ARCADE_JUMP_PAN) {
        off_x = (g.dir * g.gap * g.pan_i) / PAN_FRAMES;
        off_y = (-(g.gap / 2) * g.pan_i) / PAN_FRAMES;
        if (g.squash > 0) {
            sq = (int)g.squash * 2;
        }
    }

    ax = ANCHOR_X - off_x;
    ay = ANCHOR_Y - off_y;
    bx = ANCHOR_X + g.dir * g.gap - off_x;
    by = ANCHOR_Y - g.gap / 2 - off_y;

    draw_platform(fb, bx, by, g.nrx, sq);
    draw_platform(fb, ax, ay, g.rx, 0);

    /* 小人位置 */
    px = ax;
    py = ay;

    switch (g.state) {
    case ARCADE_JUMP_CHARGE:
        squeeze = (int)((g.charge_ms * 5) / CHARGE_MAX_MS);
        break;

    case ARCADE_JUMP_FLY: {
        int i = g.fly_i;

        px = ax + (g.dir * g.fly_dist * i) / FLY_FRAMES;
        py = ay - (g.fly_dist * i) / (2 * FLY_FRAMES);
        pz = (4 * FLY_H * i * (FLY_FRAMES - i)) / (FLY_FRAMES * FLY_FRAMES);
        air = true;
        break;
    }

    case ARCADE_JUMP_PAN: {
        /*
         * 落地瞬间人可能没踩在正中心。这一项按相机同样的节奏收敛到锚点，
         * 于是「人位置 = 锚点 + 偏差 × (1-t)」—— 误差被相机吃掉，不硬吸附。
         */
        int rest = PAN_FRAMES - g.pan_i;

        px = ANCHOR_X + (g.dir * g.fly_dist * rest) / PAN_FRAMES;
        py = ANCHOR_Y - (g.fly_dist * rest) / (2 * PAN_FRAMES);
        break;
    }

    case ARCADE_JUMP_FALL:
        /* 踩空：沿原方向继续滑一点，同时加速下坠（掉出屏幕） */
        px = ax + g.dir * (g.fly_dist + g.fall_i * 2);
        py = ay - g.fly_dist / 2 + (g.fall_i * g.fall_i) / 4;
        air = true;
        break;

    case ARCADE_JUMP_DEAD:
        show = false;
        break;

    default:
        break;
    }

    if (show) {
        if (g.state != ARCADE_JUMP_FALL) {
            draw_shadow(fb, px, py, g.rx, pz);
        }
        draw_player(fb, px, py - pz, squeeze, air);
    }

    /* 左上当前分、右上最高分 */
    snprintf(buf, sizeof(buf), "%u", (unsigned)g.score);
    arcade_gfx_text(fb, 10, 10, buf, 3, C_INK);

    snprintf(buf, sizeof(buf), "HI %05u", (unsigned)g.best);
    arcade_gfx_text(fb, ARCADE_GFX_W - 8 - arcade_gfx_text_width(buf, 1), 14, buf, 1,
                    C_DIM);

    if (g.msg_frames > 0) {
        if (g.combo > 1) {
            snprintf(buf, sizeof(buf), "PERFECT +2 X%u", (unsigned)g.combo);
        } else {
            snprintf(buf, sizeof(buf), "PERFECT +2");
        }
        arcade_gfx_text_center(fb, 56, buf, 2, C_PERFECT);
    }

    if (g.state == ARCADE_JUMP_CHARGE) {
        draw_charge_bar(fb, (int)((g.charge_ms * 100) / CHARGE_MAX_MS));
    }

    if (g.state == ARCADE_JUMP_DEAD) {
        arcade_gfx_text_center(fb, 92, "GAME OVER", 3, C_INK);

        snprintf(buf, sizeof(buf), "SCORE %u", (unsigned)g.score);
        arcade_gfx_text_center(fb, 128, buf, 2, C_INK);

        snprintf(buf, sizeof(buf), "BEST %u", (unsigned)g.best);
        arcade_gfx_text_center(fb, 152, buf, 2, C_DIM);

        if (g.dead_frames >= DEAD_LOCK) {
            arcade_gfx_text_center(fb, 180, "PRESS TO RETRY", 1, C_DIM);
        }
    }
}

/* ---------------- 对外接口 ---------------- */

void arcade_jump_enter(void)
{
    jump_load_best();
    s_active = true;
    s_auto = false;
    jump_reset();

    LISA_LOGI(TAG, "enter (best=%u)", (unsigned)g.best);
}

void arcade_jump_leave(void)
{
    s_active = false;
    s_auto = false;
    s_press_req = false;
    s_release_req = false;
    s_charge_target_ms = 0;
}

bool arcade_jump_active(void)
{
    return s_active;
}

void arcade_jump_press(void)
{
    s_press_req = true;
}

void arcade_jump_release(void)
{
    s_release_req = true;
}

void arcade_jump_charge(int ms)
{
    /* 只有在「站在平台上」时才能模拟蓄力，否则这个目标会残留到下一局 */
    if (g.state != ARCADE_JUMP_READY) {
        return;
    }

    s_charge_target_ms = ms > 0 ? ms : 1;
    s_press_req = true;
}

void arcade_jump_set_auto(bool on)
{
    s_auto = on;
    if (!on) {
        s_charge_target_ms = 0;
    }
}

bool arcade_jump_auto(void)
{
    return s_auto;
}

void arcade_jump_reset(void)
{
    jump_reset();
}

void arcade_jump_tick(void)
{
    if (!s_active) {
        return;
    }

    if (s_press_req) {
        s_press_req = false;
        jump_begin_charge();
    }
    if (s_release_req) {
        s_release_req = false;
        jump_launch();
    }

    jump_logic();

    /* 渲染：取不到空缓冲就跳过这一帧（面板留着上一帧），逻辑不受影响 */
    uint16_t *fb = arcade_video_acquire_full();

    if (fb != NULL) {
        jump_render(fb);
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

arcade_jump_state_t arcade_jump_state(void)
{
    return (arcade_jump_state_t)g.state;
}

uint32_t arcade_jump_score(void)
{
    return g.score;
}

uint32_t arcade_jump_best(void)
{
    return g.best;
}

uint32_t arcade_jump_combo(void)
{
    return g.combo;
}

int arcade_jump_charge_pct(void)
{
    return (int)((g.charge_ms * 100) / CHARGE_MAX_MS);
}

int arcade_jump_gap(void)
{
    return (int)g.gap;
}

int arcade_jump_radius(void)
{
    return (int)g.rx;
}

int arcade_jump_next_radius(void)
{
    return (int)g.nrx;
}

int arcade_jump_dir(void)
{
    return g.dir >= 0 ? 1 : -1;
}

int arcade_jump_land_err(void)
{
    return (int)g.land_err;
}

uint32_t arcade_jump_logic_fps_x10(void)
{
    return s_logic_fps_x10;
}

uint32_t arcade_jump_render_fps_x10(void)
{
    return s_render_fps_x10;
}

uint32_t arcade_jump_frames(void)
{
    return s_logic_frames;
}

uint32_t arcade_jump_renders(void)
{
    return s_render_frames;
}

uint32_t arcade_jump_skips(void)
{
    return s_skipped_frames;
}
