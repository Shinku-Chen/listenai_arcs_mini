/*
 * 模拟器声音：binjgb APU → audio0 (DAC)。
 *
 * 链路只有三块，刻意不碰助手那套 app_player / voice_player：
 *
 *   binjgb 每帧产出 u8 立体声 48kHz 采样
 *        → arcade_audio_push()：混成单声道、乘音量、写环形缓冲（生产者=模拟器任务）
 *        → arcade_audio_task：按 DAC 时钟取一块 → 自适应线性插值重采样 → lisa_audio_play_write
 *
 * 为什么需要重采样：
 *   GB 一帧是 16.74ms 的模拟时间，binjgb 每帧固定产出 803 个 48kHz 采样。
 *   但实测模拟器只有 42~54 fps（比实机 59.7 慢），也就是每秒只能喂
 *   34~43k 采样，而 DAC 每秒固定吃掉 48k。不做处理的话每秒会欠载 5~14k 采样，
 *   声音就是持续爆音。
 *   这里用「比例锁定」：稳态下「每块吃掉的源样本数」= 「每块产出的样本数」
 *   （≈ 13.4 × fps，不是它的倒数；按倒数算会立刻把缓冲抽干）。
 *   比例小于 1 意味着 DAC 每块拿 800 个输出样本去拉更长的源数据，音调变低；
 *   换来的是不爆音，而且音画同步——视频本来就是慢放，声音跟着一起慢才对得上。
 *
 *   比例（音高）怎么定（这是最容易搞错的地方）：
 *   1. 前馈：在 push 事件（一帧 803 个样本刚写完那一刻）打点，用最新/最旧
 *      检查点算精确产出率；再乘上用块打点实测出来的 DAC 块周期。两个端点
 *      都落在事件上，所以 (Δwr, Δt) 没有量化误差。
 *   2. 比例修正：(水位 EMA - LEVEL_CENTER) / 128。水位（环内待播样本数）
 *      本身就是取数量的积分，所以**纯比例即可闭环**；写成累积就是双积分，
 *      必振（实测比例在 0.65x~0.80x 以 1Hz 摆动 = 明显的走调颤音）。
 *      水位先过 1/64 EMA，把 803 样本的锯齿和它与块时钟的拍频伪影滤掉。
 *   3. 应急：水位快吃穿/胀满时按比例临时压/放大取数量，宁可音调偏一瞬
 *      也不能断音。
 *
 *   历史教训（四条，都会让声音彻底坏掉）：
 *   1) 水位越界的「极端保护」方向写反了（水位高 → 少取），缓冲一旦胀满就
 *      只会越来越胀，最后每帧产出的样本全被丢掉，喇叭在循环播放 85ms 陈旧
 *      数据 —— 听感就是「声音完全不对」。
 *   2) 水位修正按「样本数」直接加到 Q16 的取数量上（差 65536 倍），等于没有
 *      水位闭环，只剩前馈；残留 1% 失配就足以把缓冲抽干（underrun 13%）。
 *   3) 用逐块 delta 估产出率：一帧 803 个样本会变成 0/803 的脉冲串，
 *      EMA 也跟着锯齿 → 音高以 1Hz 摆动。必须在 push 事件上打点。
 *   4) 把比例修正写成「offset += err/K」（累积）：水位是取数量的积分，
 *      再积分一次就是双积分回路，任何增益都振。
 *
 * 内存：环形缓冲 / staging / 输出块都放 PSRAM。本平台 exram_malloc 失败会
 * __builtin_trap()，所以分配前先查 PSRAM 余量。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "esp_heap_caps.h"
#include "sysheap.h"
#include "lisa_log.h"
#include "lisa_device.h"
#include "lisa_audio.h"
#include "lisa_gpio.h"
#include "board.h"
#include "lisa_kv.h"

#include "arcade_audio.h"

#define TAG "arcade-audio"

#define AUDIO_DEVICE_NAME   "audio0"

/* 每个 DMA 块 800 样本 @48kHz ≈ 16.7ms，差不多就是一帧 GB */
#define BLOCK_SAMPLES       800
/* 同时在飞的 DMA 块数：4 块 ≈ 67ms 硬件延迟 */
#define BLOCK_COUNT         4
/* 环形缓冲容量（样本）：4096 ≈ 85ms */
#define RING_SAMPLES        4096
/*
 * 水位中心（样本）：环 4096 的中间偏下。
 * 注意水位本身有 803 的锯齿（生产者一帧推 803 个，消费者 61 次/秒各取 570 多个），
 * 所以「均值」必须比 803 大不少，否则谷底会掉到 0 直接欠载。
 */
#define LEVEL_CENTER        2000
/* 水位 EMA（1/64 ≈ 1s），只给比例修正用：锯齿与拍频伪影不进环路 */
#define LEVEL_EMA_SHIFT     6
/*
 * 比例修正 = (水位EMA - LEVEL_CENTER) / 128，纯比例、不累积。
 * 环内水位是「取数量的积分」，所以比例就够；累积（I）会变成双积分，必振。
 */
#define LEVEL_KP_DIV        128
#define LEVEL_TRIM_MAX      80     /* 修正量限幅（样本），防止 EMA 跑飞时音高离谱 */
/*
 * 应急：EMA 有 1s 滞后，水位真快吃穿/胀满时按比例临时压/放大取数量。
 * 代价是这一瞬间音调偏一点，但不会断音。
 */
#define LEVEL_EMERG_LOW     600
#define LEVEL_EMERG_PANIC   200
#define LEVEL_EMERG_HIGH    3500
#define LEVEL_EMERG_FULL    3900
/* staging：一块最多取 BLOCK*1.4 个样本，再多留几个给插值尾部 */
#define STAGE_SAMPLES       (BLOCK_SAMPLES * 14 / 10 + 8)
/* 每块最多取多少 / 最少取多少。按「每块 800 输出样本」换算：
 *   read ≈ 13.4 × fps  →  READ_MIN 400 ≈ 30fps 下限，READ_MAX 1120 ≈ 84fps 上限 */
#define READ_MIN            (BLOCK_SAMPLES * 5 / 10)
#define READ_MAX            (BLOCK_SAMPLES * 14 / 10)
/* 进游戏第一块还没有产出率打点，拿个典型值（≈43fps）开头，400ms 后就被实测值换掉 */
#define READ_START          (BLOCK_SAMPLES * 72 / 100)
/* 标称每秒多少个 DAC 块（48000/800 = 60），只用于给报告换算 */
#define BLOCKS_PER_SEC      (ARCADE_AUDIO_RATE / BLOCK_SAMPLES)
/*
 * 产出率打点：生产者每 RATE_INTERVAL_MS 记一个 (累计样本数, tick)；
 * 消费者用「最新 - 最旧」算产出率，窗口 800ms。端点都在 push 事件上，
 * 所以没有量化误差 —— 逐块看 delta 会变成 0/803 的脉冲串，EMA 也跟着锯齿。
 */
#define RATE_INTERVAL_MS    200
#define RATE_HIST           5
#define RATE_MIN_SPAN_MS    400
/* 产出率估计的平滑（每块 1/8），避免 fps 变化时音高阶梯跳变 */
#define FF_SMOOTH_SHIFT     3
/* 块周期实测：每 61 块（≈1s）量一次；tick 1ms / 1000ms = 0.1% 精度 */
#define CAL_BLOCKS          61

/* 幅度小于这个值当静音：8bit 采样中心偏差会带来低频嗡声 */
#define NOISE_GATE          24
/* 统计窗口：64 块 ≈ 1.07 秒 */
#define STAT_BLOCKS         64

#define VOL_DEFAULT         70
#define VOL_MAX             100
/* 满音量时只到 16bit 满幅的 85%，留给方波叠加的峰值 */
#define VOL_SCALE           108
#define KV_KEY_VOLUME       "arcade.volume"
/* 和语音助手共用同一个喇叭模拟增益设置 */
#define KV_KEY_SPK_GAIN     "user.spk_gain_db"

#define AUDIO_TASK_STACK    2048
#define AUDIO_TASK_PRIO     6

struct arcade_audio_ctx {
    int16_t *ring;      /* RING_SAMPLES */
    int16_t *stage;     /* STAGE_SAMPLES：连续化后的待插值数据 */
    int16_t *block;     /* BLOCK_SAMPLES：交给 DAC 的一块 */

    /*
     * 生产/消费进度用「累计样本数」而不是环形索引：两边各自单调加，
     * 差值天然就是水位，32 位回绕也自动正确。
     */
    volatile uint32_t wr_total;
    volatile uint32_t rd_total;

    /* 消费者私有：插值小数位 */
    uint32_t frac_q16;

    /*
     * 比例控制（音高）：read_q16 是「每块从环形缓冲取多少源样本」的当前值
     * （Q16 定点，能表示亚样本）。ff_q16 是在 push 事件上打点算出的精确
     * 产出率（Q16 样本/块）；read_q16 与 ff_q16 的差就是水位比例的修正量。
     */
    uint32_t read_q16;
    uint32_t ff_q16;
    int32_t ff_diff_q16;      /* 只为报告：ff 到取数量之间差多少样本 */
    int32_t level_ema_q8;

    /* 产出率打点：生产者写，消费者读；[0] 最新，[count-1] 最旧 */
    uint32_t rate_wr[RATE_HIST];
    uint32_t rate_tick[RATE_HIST];
    uint8_t rate_count;
    uint32_t rate_last_tick;

    /* 块周期实测（DAC 块率未必正好是标称的 60/秒，差多少由这里量出来） */
    uint32_t cal_tick;
    uint32_t cal_blocks;
    uint32_t block_period_q8;  /* 块周期，ms × 256 */
    bool cal_ready;

    /* DC 阻断（一阶高通）状态：R = 1 - 1/512，截止频率 ≈ 15Hz
     * （原来是 1/128≈60Hz，会把 GB 的低音弦律削掉，听上去“发薄”） */
    int32_t dc_x1;
    int32_t dc_y1;

    /* 统计 */
    volatile uint32_t blocks;
    volatile uint32_t silence_blocks;
    volatile uint32_t underruns;
    volatile uint32_t overruns;    /* 环形缓冲满时丢掉的样本数 */
    volatile uint32_t write_err;
    volatile uint32_t ratio_permille;
    volatile uint32_t ratio_target_permille;
    volatile int32_t ratio_slew_permille_s;   /* 最近一块的比例变化率（千分比/秒） */
    uint16_t peak[STAT_BLOCKS];
    uint32_t mean_sq[STAT_BLOCKS];
    uint32_t stat_pos;
};

static struct arcade_audio_ctx *s_ctx;
static lisa_device_t *s_dev;
static volatile bool s_task_run;
static volatile bool s_active;
static volatile uint32_t s_vol = VOL_DEFAULT;

/* ---------------- 小工具 ---------------- */

static uint32_t isqrt64(uint64_t v)
{
    uint64_t x = v;
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > x) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)r;
}

static void stat_store(struct arcade_audio_ctx *c, uint16_t peak, uint32_t mean_sq)
{
    c->peak[c->stat_pos] = peak;
    c->mean_sq[c->stat_pos] = mean_sq;
    c->stat_pos = (c->stat_pos + 1) % STAT_BLOCKS;
}

/* ---------------- 生产者：模拟器任务 ---------------- */

void arcade_audio_push(const uint8_t *src, uint32_t frames)
{
    struct arcade_audio_ctx *c = s_ctx;
    uint32_t vol;
    uint32_t wr;
    uint32_t space;
    uint32_t drop;
    uint32_t i;
    uint32_t now;

    if (c == NULL || src == NULL || frames == 0 || !s_active) {
        return;
    }

    vol = s_vol;
    if (vol > VOL_MAX) {
        vol = VOL_MAX;
    }

    taskENTER_CRITICAL();

    space = RING_SAMPLES - (c->wr_total - c->rd_total);
    if (frames > space) {
        /* 缓冲满了：丢最旧的，宁可短暂缺帧也不要让延迟无限增长 */
        drop = frames - space;
        c->rd_total += drop;
        c->overruns += drop;
    }

    wr = c->wr_total % RING_SAMPLES;

    for (i = 0; i < frames; i++) {
        int32_t l = (int32_t)src[0] - 128;
        int32_t r = (int32_t)src[1] - 128;
        /* 混成单声道（(l+r)/2）再放大到 16bit（*256），乘音量一起算：
         * (l+r) 在 [-256,254]，×vol(≤100)×128 最大约 3.25e6，int32 够用。
         * 128 就是这里乘的位宽；乘上 VOL_HEADROOM 给满音量留 15% 余量，
         * 否则 GB 那些满幅方波在 100% 音量下会硬削顶。 */
        int32_t m = (l + r) * (int32_t)vol * VOL_SCALE / VOL_MAX;

        if (m > 32767) {
            m = 32767;
        } else if (m < -32768) {
            m = -32768;
        }

        c->ring[wr] = (int16_t)m;
        wr++;
        if (wr >= RING_SAMPLES) {
            wr = 0;
        }

        src += 2;
    }

    c->wr_total += frames;

    /* 产出率打点：端点都落在一帧样本刚写完那一刻，所以 (Δwr, Δt) 是精确的。
     * 用逐块 delta 的话，一帧 803 个样本会变成 0/803 的脉冲串，EMA 也跟着锯齿走。
     * tick 只做打点节奏用，精度够（800ms 窗口下 1ms 分辨率 = 0.125%）。 */
    now = (uint32_t)xTaskGetTickCount();
    if (c->rate_count == 0 || (now - c->rate_last_tick) >= RATE_INTERVAL_MS) {
        if (c->rate_count < RATE_HIST) {
            c->rate_count++;
        }
        for (i = c->rate_count - 1; i > 0; i--) {
            c->rate_wr[i] = c->rate_wr[i - 1];
            c->rate_tick[i] = c->rate_tick[i - 1];
        }
        c->rate_wr[0] = c->wr_total;
        c->rate_tick[0] = now;
        c->rate_last_tick = now;
    }

    taskEXIT_CRITICAL();
}

/* ---------------- 消费者：音频任务 ---------------- */

/*
 * 比例控制：算出这一块应该从环形缓冲取多少源样本（返回 Q16）。
 *
 * 稳态条件只有一个：「每块吃掉的源样本数 == 每块产出的样本数」。
 * 公式：read = 产出率(样本/ms) × 块周期(ms)
 *
 *   1. 产出率来自 push 打点（精确，见 arcade_audio_push）；
 *      块周期来自块打点（DAC 实测，见下面的 CAL_BLOCKS）。
 *      两者相乘就是「稳态该吃多少」，它决定了音高：模拟器只有实机 ~70%
 *      速度时它自然 ≈ 0.72×800，音调整体变低，但音画同步、不爆音。
 *   2. 比例修正：(水位EMA - LEVEL_CENTER)/128，纯比例。环内水位就是取数量的
 *      积分，所以比例即可闭环；一旦写成累积（I）就是双积分，必然振荡
 *      （这条踩过：比例在 0.65x~0.80x 之间 1Hz 摆动 = 明显走调颤音）。
 *   3. 应急：水位接近吃穿/胀满时按比例临时压/放大取数量，宁可音调偏一下
 *      也不能断音。
 *
 * 为什么不拿瞬时水位算：一帧 803 个样本是一次性推的，水位本身就是 803 振幅的
 * 锯齿；它和块时钟的拍频还会落在 1~8Hz，任何滤波都去不掉。
 */
static uint32_t audio_read_q16(struct arcade_audio_ctx *c, uint32_t avail)
{
    int32_t offset_q16;
    int32_t rail_q16 = 0;
    int32_t read_q16;
    int32_t prev_q16;
    uint32_t now;
    uint32_t dwr = 0;
    uint32_t dt = 0;
    uint8_t rate_count;
    int32_t level_err;

    /* ---- 块周期实测：每 61 块量一次（精度 1ms/1000ms = 0.1%） ---- */
    now = (uint32_t)xTaskGetTickCount();
    if (!c->cal_ready) {
        c->cal_tick = now;
        c->cal_blocks = c->blocks;
        c->cal_ready = true;
    } else if ((c->blocks - c->cal_blocks) >= CAL_BLOCKS) {
        uint32_t dt = now - c->cal_tick;
        uint32_t db = c->blocks - c->cal_blocks;

        if (dt > 0) {
            c->block_period_q8 = (dt << 8) / db;
        }
        c->cal_tick = now;
        c->cal_blocks = c->blocks;
    }

    /* ---- 前馈：产出率(样本/ms) × 块周期(ms) ---- */
    rate_count = 0;
    taskENTER_CRITICAL();
    rate_count = c->rate_count;
    if (rate_count >= 2) {
        dwr = c->rate_wr[0] - c->rate_wr[rate_count - 1];
        dt = c->rate_tick[0] - c->rate_tick[rate_count - 1];
    }
    taskEXIT_CRITICAL();

    if (rate_count >= 2) {
        if (dt >= RATE_MIN_SPAN_MS) {
            int32_t est = (int32_t)(((int64_t)dwr * c->block_period_q8 * 65536) /
                                    ((int64_t)dt * 256));

            if (est < ((int32_t)READ_MIN << 16)) {
                est = (int32_t)READ_MIN << 16;
            } else if (est > ((int32_t)READ_MAX << 16)) {
                est = (int32_t)READ_MAX << 16;
            }
            /* 平滑一下：估计值每 200ms 更新一次，直接套用会让音高阶梯跳 */
            c->ff_q16 += (uint32_t)((est - (int32_t)c->ff_q16) >> FF_SMOOTH_SHIFT);
        }
    }

    /* ---- 水位比例修正（纯 P） ---- */
    c->level_ema_q8 += (((int32_t)avail << 8) - c->level_ema_q8) / (1 << LEVEL_EMA_SHIFT);
    level_err = (c->level_ema_q8 >> 8) - (int32_t)LEVEL_CENTER;
    offset_q16 = (level_err * 65536) / LEVEL_KP_DIV;
    if (offset_q16 > ((int32_t)LEVEL_TRIM_MAX << 16)) {
        offset_q16 = (int32_t)LEVEL_TRIM_MAX << 16;
    } else if (offset_q16 < -((int32_t)LEVEL_TRIM_MAX << 16)) {
        offset_q16 = -((int32_t)LEVEL_TRIM_MAX << 16);
    }

    /* ---- 应急（EMA 有 1s 滞后，这里看瞬时水位） ---- */
    if (avail < LEVEL_EMERG_PANIC) {
        rail_q16 = -((int32_t)c->read_q16 / 4);
    } else if (avail < LEVEL_EMERG_LOW) {
        rail_q16 = -((int32_t)c->read_q16 / 8);
    } else if (avail > LEVEL_EMERG_FULL) {
        rail_q16 = (int32_t)c->read_q16 / 4;
    } else if (avail > LEVEL_EMERG_HIGH) {
        rail_q16 = (int32_t)c->read_q16 / 8;
    }

    read_q16 = (int32_t)c->ff_q16 + offset_q16 + rail_q16;
    if (read_q16 < ((int32_t)READ_MIN << 16)) {
        read_q16 = (int32_t)READ_MIN << 16;
    } else if (read_q16 > ((int32_t)READ_MAX << 16)) {
        read_q16 = (int32_t)READ_MAX << 16;
    }

    prev_q16 = (int32_t)c->read_q16;
    c->read_q16 = (uint32_t)read_q16;
    c->ff_diff_q16 = offset_q16 + rail_q16;   /* 报告用 */
    c->ratio_permille = (uint32_t)(((uint64_t)read_q16 * 1000u) / ((uint32_t)BLOCK_SAMPLES << 16));
    c->ratio_target_permille =
        (uint32_t)(((uint64_t)c->ff_q16 * 1000u) / ((uint32_t)BLOCK_SAMPLES << 16));
    c->ratio_slew_permille_s =
        (int32_t)(((int64_t)(read_q16 - prev_q16) * 1000 * BLOCKS_PER_SEC) /
                  ((int64_t)BLOCK_SAMPLES << 16));

    return (uint32_t)read_q16;
}

/*
 * 从环形缓冲里取一块出来，做线性插值重采样填满 BLOCK_SAMPLES。
 */
static void audio_fill_block(struct arcade_audio_ctx *c)
{
    uint32_t avail;
    uint32_t rd;
    uint32_t read_q16;
    uint32_t read_count;
    uint32_t span;
    uint32_t consumed;
    uint32_t i;
    uint64_t pos;
    uint64_t step;
    int32_t peak = 0;
    uint64_t sumsq = 0;
    int16_t *out = c->block;

    avail = c->wr_total - c->rd_total;

    if (avail < 16) {
        /* 欠载：输出静音，并把插值/DC 状态复位，避免下一块从旧数据接上来。
         * 不清比例状态——那是「模拟器恒定偏慢/偏快」的学习值，
         * 一欠载就清会让它永远学不会比例，反复欠载。 */
        memset(out, 0, sizeof(int16_t) * BLOCK_SAMPLES);
        c->frac_q16 = 0;
        c->dc_x1 = 0;
        c->dc_y1 = 0;
        c->underruns++;
        c->blocks++;
        stat_store(c, 0, 0);
        return;
    }

    read_q16 = audio_read_q16(c, avail);

    /* staging 要装下插值用到的后一个样本，所以按「向上取整 + 2」准备 */
    read_count = (read_q16 + 0xFFFFu) >> 16;
    if (read_count + 2 > avail) {
        /* 数据不够（产出突然变慢）：本块被动降速，只吃现有的 */
        read_count = avail - 2;
        read_q16 = read_count << 16;
    }
    if (read_count < 2) {
        read_count = 2;
        read_q16 = read_count << 16;
    }

    /* 拷到连续 staging：环形缓冲最多跨一次边界，逐个取模太浪费 */
    span = read_count + 2;
    if (span > STAGE_SAMPLES) {
        span = STAGE_SAMPLES;
    }

    taskENTER_CRITICAL();
    rd = c->rd_total % RING_SAMPLES;
    for (i = 0; i < span; i++) {
        if (i < avail) {
            uint32_t idx = rd + i;

            if (idx >= RING_SAMPLES) {
                idx -= RING_SAMPLES;
            }
            c->stage[i] = c->ring[idx];
        } else {
            /* 尾部不够两个样本时保持最后一个值，避免插值出咔哒声 */
            c->stage[i] = (i > 0) ? c->stage[i - 1] : 0;
        }
    }
    taskEXIT_CRITICAL();

    pos = c->frac_q16;
    /* step 直接用 Q16 的取数量算，保留小数：否则整数截断会带来 0.7% 的率误差，
     * 控制环只能在两个整数之间来回跳（一直成对地多取/少取）。 */
    step = read_q16 / BLOCK_SAMPLES;

    for (i = 0; i < BLOCK_SAMPLES; i++) {
        uint32_t idx = (uint32_t)(pos >> 16);
        int32_t f = (int32_t)(pos & 0xFFFFu);
        int32_t a = c->stage[idx];
        int32_t b = c->stage[idx + 1];
        int32_t v = a + (int32_t)(((int64_t)(b - a) * f) >> 16);
        int32_t y = v - c->dc_x1 + c->dc_y1 - (c->dc_y1 >> 9);
        int32_t ay;

        c->dc_x1 = v;
        c->dc_y1 = y;

        if (y > 32767) {
            y = 32767;
        } else if (y < -32768) {
            y = -32768;
        }
        if (y < NOISE_GATE && y > -NOISE_GATE) {
            y = 0;
        }

        out[i] = (int16_t)y;

        ay = (y < 0) ? -y : y;
        if (ay > peak) {
            peak = ay;
        }
        sumsq += (uint64_t)((int32_t)y * (int32_t)y);

        pos += step;
    }

    consumed = (uint32_t)(pos >> 16);
    c->frac_q16 = (uint32_t)(pos & 0xFFFFu);
    if (consumed > avail) {
        consumed = avail;
    }

    c->rd_total += consumed;
    c->blocks++;
    stat_store(c, (uint16_t)peak, (uint32_t)(sumsq / BLOCK_SAMPLES));
}

static void arcade_audio_task(void *arg)
{
    struct arcade_audio_ctx *c = s_ctx;
    int16_t *silence = NULL;
    int ret;

    (void)arg;

    silence = (int16_t *)exram_malloc(4, sizeof(int16_t) * BLOCK_SAMPLES);
    if (silence != NULL) {
        memset(silence, 0, sizeof(int16_t) * BLOCK_SAMPLES);
    }

    while (s_task_run) {
        if (!s_active) {
            /* 不在游戏里：持续喂静音块，让 DAC 保持自己的时钟节奏，
             * 同时把环形缓冲清空，下一次进游戏从干净状态开始 */
            taskENTER_CRITICAL();
            c->rd_total = c->wr_total;
            taskEXIT_CRITICAL();
            c->silence_blocks++;
            stat_store(c, 0, 0);

            if (silence == NULL) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            ret = lisa_audio_play_write(s_dev, silence, BLOCK_SAMPLES);
        } else {
            audio_fill_block(c);
            ret = lisa_audio_play_write(s_dev, c->block, BLOCK_SAMPLES);
        }

        if (ret < 0) {
            c->write_err++;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    if (silence != NULL) {
        exram_free(silence);
    }

    vTaskDelete(NULL);
}

/* ---------------- 对外接口 ---------------- */

/* 外部功放（PA）使能：正常固件由 voice_player_comm 管；模拟器固件跳过了语音
 * 链路，所以这里自己把 PA MUTE 脚拉到"非静音"电平，否则 DAC 有声但喇叭不出声。 */
static void arcade_audio_pa_enable(void)
{
#ifdef PA_MUTE_DEVICE_NAME
    lisa_device_t *dev = lisa_device_get(PA_MUTE_DEVICE_NAME);
    uint32_t level;
    uint32_t init_flag;

    if (dev == NULL || !lisa_device_ready(dev)) {
        LISA_LOGW(TAG, "PA mute device not ready");
        return;
    }

    /* ACTIVE_LEVEL==0 表示低电平静音，所以拉到相反电平 = 放音 */
    level = (PA_MUTE_ACTIVE_LEVEL != 0) ? LISA_GPIO_LOW : LISA_GPIO_HIGH;
    init_flag = (level == LISA_GPIO_HIGH) ? LISA_GPIO_OUTPUT_INIT_HIGH
                                          : LISA_GPIO_OUTPUT_INIT_LOW;

    if (lisa_gpio_configure(dev, PA_MUTE_PIN, LISA_GPIO_OUTPUT | init_flag) == 0) {
        (void)lisa_gpio_write_pin(dev, PA_MUTE_PIN, level);
        LISA_LOGI(TAG, "PA enabled (pin %d -> %s)", PA_MUTE_PIN,
                  (level == LISA_GPIO_HIGH) ? "HIGH" : "LOW");
    } else {
        LISA_LOGW(TAG, "PA mute GPIO configure failed");
    }
#endif
}

int arcade_audio_init(void)
{
    arcade_audio_pa_enable();
    lisa_audio_play_config_t cfg;
    int analog_gain;
    int digital_gain;
    int vol;
    int kv_gain;
    int ret;

    if (s_ctx != NULL) {
        return 0;
    }

    s_dev = lisa_device_get(AUDIO_DEVICE_NAME);
    if (s_dev == NULL || !lisa_device_ready(s_dev)) {
        LISA_LOGE(TAG, "audio device %s not ready", AUDIO_DEVICE_NAME);
        return -1;
    }

    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 96u * 1024u) {
        LISA_LOGE(TAG, "not enough psram for audio buffers");
        return -1;
    }

    s_ctx = (struct arcade_audio_ctx *)exram_malloc(4, sizeof(*s_ctx));
    if (s_ctx == NULL) {
        LISA_LOGE(TAG, "audio context alloc failed");
        return -1;
    }
    memset(s_ctx, 0, sizeof(*s_ctx));
    /* 进游戏前先把比例控制状态放在合理初值上（flush 也会再设一次） */
    s_ctx->ff_q16 = (uint32_t)READ_START << 16;
    s_ctx->read_q16 = (uint32_t)READ_START << 16;
    s_ctx->level_ema_q8 = (int32_t)LEVEL_CENTER << 8;
    s_ctx->block_period_q8 = (1000u << 8) / BLOCKS_PER_SEC;   /* 标称 16.67ms */

    s_ctx->ring = (int16_t *)exram_malloc(4, sizeof(int16_t) * RING_SAMPLES);
    s_ctx->stage = (int16_t *)exram_malloc(4, sizeof(int16_t) * STAGE_SAMPLES);
    s_ctx->block = (int16_t *)exram_malloc(4, sizeof(int16_t) * BLOCK_SAMPLES);
    if (s_ctx->ring == NULL || s_ctx->stage == NULL || s_ctx->block == NULL) {
        LISA_LOGE(TAG, "audio buffers alloc failed");
        goto fail;
    }
    memset(s_ctx->ring, 0, sizeof(int16_t) * RING_SAMPLES);
    memset(s_ctx->stage, 0, sizeof(int16_t) * STAGE_SAMPLES);
    memset(s_ctx->block, 0, sizeof(int16_t) * BLOCK_SAMPLES);

    vol = VOL_DEFAULT;
    if (lisa_kv_get_int(KV_KEY_VOLUME, &vol) == 0 && vol >= 0 && vol <= VOL_MAX) {
        s_vol = (uint32_t)vol;
    }

    /* 和语音助手用同一套喇叭模拟增益（用户可能在 KV 里改过） */
    analog_gain = CONFIG_AUDIO_DEFAULT_SPK_GAIN_A_DB;
    digital_gain = CONFIG_AUDIO_DEFAULT_SPK_GAIN_D_DB;
    if (lisa_kv_get_int(KV_KEY_SPK_GAIN, &kv_gain) == 0) {
        if (kv_gain < -24) {
            kv_gain = -24;
        } else if (kv_gain > 6) {
            kv_gain = 6;
        }
        analog_gain = kv_gain;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.format.sample_rate = (lisa_audio_rate_t)ARCADE_AUDIO_RATE;
    cfg.format.channels = LISA_AUDIO_CH_LEFT;
    cfg.format.sample_bits = LISA_AUDIO_BIT_16;
    cfg.gain.analog_gain = (int8_t)analog_gain;
    cfg.gain.digital_gain = (int8_t)digital_gain;
    cfg.buffer_count = BLOCK_COUNT;
    cfg.buffer_samples = BLOCK_SAMPLES;

    ret = lisa_audio_play_config(s_dev, &cfg);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "play_config failed: %d", ret);
        goto fail;
    }

    ret = lisa_audio_play_start(s_dev);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "play_start failed: %d", ret);
        goto fail;
    }

    s_active = false;
    s_task_run = true;

    if (xTaskCreate(arcade_audio_task, "arcade_audio", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, NULL) != pdPASS) {
        LISA_LOGE(TAG, "audio task create failed");
        s_task_run = false;
        lisa_audio_play_stop(s_dev);
        goto fail;
    }

    LISA_LOGI(TAG, "audio ready: %d Hz, %d x %d samples, vol %u%%, gain %d/%d dB",
              ARCADE_AUDIO_RATE, BLOCK_COUNT, BLOCK_SAMPLES,
              (unsigned)s_vol, analog_gain, digital_gain);
    return 0;

fail:
    if (s_ctx != NULL) {
        if (s_ctx->block != NULL) {
            exram_free(s_ctx->block);
        }
        if (s_ctx->stage != NULL) {
            exram_free(s_ctx->stage);
        }
        if (s_ctx->ring != NULL) {
            exram_free(s_ctx->ring);
        }
        exram_free(s_ctx);
        s_ctx = NULL;
    }
    s_dev = NULL;
    return -1;
}

void arcade_audio_deinit(void)
{
    if (s_ctx == NULL) {
        return;
    }

    s_active = false;

    if (s_task_run) {
        s_task_run = false;

        /* 先停播放：DMA 停下后 play_write 就不会再阻塞，任务能走到退出分支 */
        if (s_dev != NULL) {
            lisa_audio_play_stop(s_dev);
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    if (s_ctx->block != NULL) {
        exram_free(s_ctx->block);
    }
    if (s_ctx->stage != NULL) {
        exram_free(s_ctx->stage);
    }
    if (s_ctx->ring != NULL) {
        exram_free(s_ctx->ring);
    }
    exram_free(s_ctx);
    s_ctx = NULL;
    s_dev = NULL;
}

bool arcade_audio_ready(void)
{
    return s_ctx != NULL;
}

void arcade_audio_set_active(bool active)
{
    if (s_ctx == NULL) {
        return;
    }

    if (active) {
        arcade_audio_flush();
    }

    s_active = active;
}

void arcade_audio_flush(void)
{
    struct arcade_audio_ctx *c = s_ctx;

    if (c == NULL) {
        return;
    }

    taskENTER_CRITICAL();
    c->rd_total = c->wr_total;
    taskEXIT_CRITICAL();

    c->frac_q16 = 0;
    c->dc_x1 = 0;
    c->dc_y1 = 0;
    /* 进游戏/切 ROM 时产出率还不知道：前馈回到典型值，等 400ms 后的实测值接管；
     * 产出率打点也作废（旧游戏的帧率不能用在新游戏上）。 */
    c->rate_count = 0;
    c->ff_q16 = (uint32_t)READ_START << 16;
    c->read_q16 = (uint32_t)READ_START << 16;
    c->level_ema_q8 = (int32_t)LEVEL_CENTER << 8;
    c->cal_ready = false;
}

void arcade_audio_set_volume(uint32_t vol)
{
    if (vol > VOL_MAX) {
        vol = VOL_MAX;
    }

    s_vol = vol;
    (void)lisa_kv_set_int(KV_KEY_VOLUME, (int)vol);
}

uint32_t arcade_audio_volume(void)
{
    return s_vol;
}

void arcade_audio_reset_stats(void)
{
    struct arcade_audio_ctx *c = s_ctx;

    if (c == NULL) {
        return;
    }

    taskENTER_CRITICAL();
    c->blocks = 0;
    c->silence_blocks = 0;
    c->underruns = 0;
    c->overruns = 0;
    c->write_err = 0;
    taskEXIT_CRITICAL();

    memset(c->peak, 0, sizeof(c->peak));
    memset(c->mean_sq, 0, sizeof(c->mean_sq));
    c->stat_pos = 0;
}

int arcade_audio_report(char *out, uint32_t out_len)
{
    struct arcade_audio_ctx *c = s_ctx;
    uint32_t wr;
    uint32_t rd;
    uint32_t blocks;
    uint32_t silence;
    uint32_t under;
    uint32_t over;
    uint32_t errs;
    uint32_t ratio;
    uint32_t target;
    uint32_t level;
    uint32_t blk_ms;
    uint32_t level_ema;
    int32_t trim;
    int32_t slew;
    int n;

    if (out == NULL || out_len < 64) {
        return -1;
    }

    if (c == NULL) {
        return snprintf(out, out_len, "audio: 未初始化\n");
    }

    taskENTER_CRITICAL();
    wr = c->wr_total;
    rd = c->rd_total;
    blocks = c->blocks;
    silence = c->silence_blocks;
    under = c->underruns;
    over = c->overruns;
    errs = c->write_err;
    ratio = c->ratio_permille;
    target = c->ratio_target_permille;
    trim = c->ff_diff_q16;
    level_ema = (uint32_t)(c->level_ema_q8 >> 8);
    blk_ms = (c->block_period_q8 * 100u) / 256u;   /* ms × 100 */
    slew = c->ratio_slew_permille_s;
    taskEXIT_CRITICAL();

    level = wr - rd;

    n = snprintf(out, out_len,
                 "rate=%d block=%dx%d ring=%d target=%d level=%u\n",
                 ARCADE_AUDIO_RATE, BLOCK_COUNT, BLOCK_SAMPLES,
                 RING_SAMPLES, LEVEL_CENTER, (unsigned)level);
    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "vol=%u%% read=%u.%03ux ff=%u.%03ux trim=%s%u.%02u blk=%u.%02ums slew=%s%u.%u%%/s active=%d\n",
                  (unsigned)s_vol, (unsigned)(ratio / 1000u), (unsigned)(ratio % 1000u),
                  (unsigned)(target / 1000u), (unsigned)(target % 1000u),
                  trim < 0 ? "-" : "+",
                  (unsigned)((trim < 0 ? -trim : trim) >> 16),
                  (unsigned)((((trim < 0 ? -trim : trim) & 0xFFFFu) * 100u) >> 16),
                  (unsigned)(blk_ms / 100u), (unsigned)(blk_ms % 100u),
                  slew < 0 ? "-" : "+",
                  (unsigned)((slew < 0 ? -slew : slew) / 10),
                  (unsigned)((slew < 0 ? -slew : slew) % 10),
                  s_active ? 1 : 0);
    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "blocks=%u silence=%u underrun=%u (%u.%u%%) overrun=%u err=%u iram_free=%u KB\n",
                  (unsigned)blocks, (unsigned)silence, (unsigned)under,
                  (unsigned)(blocks ? (under * 1000u / blocks) / 10u : 0),
                  (unsigned)(blocks ? (under * 1000u / blocks) % 10u : 0),
                  (unsigned)over, (unsigned)errs,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    return n;
}

int arcade_audio_peak_report(char *out, uint32_t out_len)
{
    struct arcade_audio_ctx *c = s_ctx;
    uint32_t peak = 0;
    uint64_t sumsq = 0;
    uint32_t used = 0;
    uint32_t rms;
    int n;

    if (out == NULL || out_len < 48) {
        return -1;
    }

    if (c == NULL) {
        return snprintf(out, out_len, "audio: 未初始化\n");
    }

    for (uint32_t i = 0; i < STAT_BLOCKS; i++) {
        uint16_t p = c->peak[i];
        uint32_t m = c->mean_sq[i];

        if (p > peak) {
            peak = p;
        }
        if (m != 0 || p != 0) {
            sumsq += m;
            used++;
        }
    }

    rms = used ? isqrt64(sumsq / used) : 0;

    n = snprintf(out, out_len,
                 "window=%u/%d blocks peak=%u (%u.%u%%) rms=%u (%u.%u%%)\n",
                 (unsigned)(used ? used : 0), STAT_BLOCKS,
                 (unsigned)peak, (unsigned)(peak * 100u / 32768u),
                 (unsigned)(((uint64_t)peak * 1000u / 32768u) % 10u),
                 (unsigned)rms, (unsigned)(rms * 100u / 32768u),
                 (unsigned)(((uint64_t)rms * 1000u / 32768u) % 10u));

    return n;
}
