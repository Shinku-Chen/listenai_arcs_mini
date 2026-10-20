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
 *   这里用「比例锁定」：目标比例 = 模拟器实际产出率的 1/8 指数平均
 *   （≈ 每块产出样本数），当前比例按 ±3%/s 的速度向目标滑。比例小于 1 时
 *   DAC 每块需要多少输出就拉多长的源数据，也就是音调变低；换来的是不爆音，
 *   而且音画同步——视频本来就是慢放，声音跟着一起慢才对得上。
 *
 *   历史教训：以前是按水位做 PI 闭环、每块都改比例，音高一直在抖（vibrato），
 *   听起来像走调。现在水位只做极端保护（<15% / >90% 才推一小步）。
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
/* 目标水位（样本）：1200 ≈ 25ms，给帧抖动留余量 */
#define TARGET_LEVEL        1200
/* staging：一块最多取 BLOCK*1.4 个样本，再多留几个给插值尾部 */
#define STAGE_SAMPLES       (BLOCK_SAMPLES * 14 / 10 + 8)
/* 每块最多取多少 / 最少取多少（±40%，再大就明显跑调了） */
#define READ_MIN            (BLOCK_SAMPLES * 6 / 10)
#define READ_MAX            (BLOCK_SAMPLES * 14 / 10)
/*
 * 比例（音高）的最大变化率，千分比/秒。
 * 原来的 PI 每块都改比例，而模拟器只有实机 70% 速度，比例被拉到 0.6x
 * 之后还在持续摆动 —— 听起来就是音高在抖（vibrato）。
 * 限制到 3%/s 后音高是稳的：慢速滑过去，之后不再动。
 */
#define RATIO_SLEW_PERMILLE_PER_S   30
/* 每秒多少个 DAC 块（48000/800 = 60） */
#define BLOCKS_PER_SEC      (ARCADE_AUDIO_RATE / BLOCK_SAMPLES)
/* 水位只做极端保护：低于 15% 才略微加快，高于 90% 才略微放慢 */
#define LEVEL_LOW           (RING_SAMPLES * 15 / 100)
#define LEVEL_HIGH          (RING_SAMPLES * 90 / 100)
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
     * 比例控制（音高）：read_q16 是「每块从环形缓冲取多少样本」的当前值
     * （Q16 定点，允许亚样本级慢速滑动），prod_ema_q8 是模拟器每块产出
     * 样本数的 1/8 指数平均（Q8），用它算目标比例。
     */
    uint32_t read_q16;
    int32_t prod_ema_q8;
    int32_t level_ema_q8;
    uint32_t last_wr;
    bool ratio_primed;

    /* DC 阻断（一阶高通）状态 */
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

    taskEXIT_CRITICAL();
}

/* ---------------- 消费者：音频任务 ---------------- */

/*
 * 比例控制：算出这一块应该从环形缓冲取多少样本。
 *
 * 旧做法是按水位做 PI，每块都改 —— 音高持续抖。现在分两层：
 *   1. 目标比例 = 模拟器实际产出率（产出样本/块）的 1/8 指数平均，
 *      再叠一个「水位慢修正」（水位 EMA 与目标水位的差 /2，它是常量偏置，
 *      不是每块大摆的 P 项；修正同时受下面的速率限制）。
 *      模拟器快、产出多，就每块多取一点（音高升）；这是决定音高的量。
 *   2. 当前比例按 ±3%/s 的速度向目标滑——稳定不抖，只有模拟速度变化时才
 *      听到一次很慢的滑音。
 * 为什么不能直接拿瞬时水位算：模拟器每帧一次性推 803 个样本，水位是锯齿，
 * 逐块跟它算就是那个抖动（vibrato）的根源。所以水位先过 EMA 再用。
 */
static uint32_t audio_read_count(struct arcade_audio_ctx *c, uint32_t avail)
{
    int32_t target_q16;
    int32_t cur;
    int32_t max_step;
    int32_t step;
    int32_t level_err;
    uint32_t delta;

    /* 产出量估计：上一块到现在模拟器推进了多少样本 */
    delta = c->wr_total - c->last_wr;
    c->last_wr = c->wr_total;
    if (delta > 2u * BLOCK_SAMPLES) {
        /* 切游戏/快进时的大跳变不参与估计 */
        delta = 2u * BLOCK_SAMPLES;
    }

    /* 水位也过一遍 EMA（同样是 1/8），把每帧推一大块的锯齿磨平 */
    c->level_ema_q8 += (((int32_t)avail << 8) - c->level_ema_q8) / 8;

    if (!c->ratio_primed) {
        /* 第一块直接拿实际产出量当初值，省掉从 1.0x 慢滑到 0.7x 的滑音 */
        c->prod_ema_q8 = (int32_t)delta << 8;
        c->level_ema_q8 = (int32_t)avail << 8;
        c->ratio_primed = true;
    } else {
        c->prod_ema_q8 += (((int32_t)delta << 8) - c->prod_ema_q8) / 8;
    }

    target_q16 = (int32_t)((uint32_t)c->prod_ema_q8 >> 8) << 16;

    /* 水位慢修正：只把目标往「回到 TARGET_LEVEL」推一点点势 */
    level_err = (int32_t)((uint32_t)c->level_ema_q8 >> 8) - (int32_t)TARGET_LEVEL;
    target_q16 += level_err / 2;

    /* 速率限制（每块最大能改多少，Q16 定点） */
    max_step = (int32_t)(((int64_t)RATIO_SLEW_PERMILLE_PER_S * BLOCK_SAMPLES * 65536) /
                         (1000 * BLOCKS_PER_SEC));

    /* 极端水位：直接按最大速率推，防止长时间漂到吃穿/滞死 */
    if (avail < LEVEL_LOW) {
        target_q16 = (int32_t)c->read_q16 + max_step;
        c->level_ema_q8 = (int32_t)avail << 8;
    } else if (avail > LEVEL_HIGH) {
        target_q16 = (int32_t)c->read_q16 - max_step;
        c->level_ema_q8 = (int32_t)avail << 8;
    }

    if (target_q16 < ((int32_t)READ_MIN << 16)) {
        target_q16 = (int32_t)READ_MIN << 16;
    } else if (target_q16 > ((int32_t)READ_MAX << 16)) {
        target_q16 = (int32_t)READ_MAX << 16;
    }

    cur = (int32_t)c->read_q16;
    if (cur == 0) {
        cur = target_q16;
    }

    step = target_q16 - cur;
    if (step > max_step) {
        step = max_step;
    } else if (step < -max_step) {
        step = -max_step;
    }
    cur += step;

    if (cur < ((int32_t)READ_MIN << 16)) {
        cur = (int32_t)READ_MIN << 16;
    } else if (cur > ((int32_t)READ_MAX << 16)) {
        cur = (int32_t)READ_MAX << 16;
    }

    c->read_q16 = (uint32_t)cur;
    c->ratio_permille = (uint32_t)(((uint64_t)cur * 1000u) / ((uint32_t)BLOCK_SAMPLES << 16));
    c->ratio_target_permille =
        (uint32_t)(((uint64_t)target_q16 * 1000u) / ((uint32_t)BLOCK_SAMPLES << 16));
    /* 实际变化率（千分比/秒），隔一会儿再跑一条 `arcade audio` 对比即知是否还在抖 */
    c->ratio_slew_permille_s =
        (int32_t)(((int64_t)step * 1000 * BLOCKS_PER_SEC) / BLOCK_SAMPLES);

    return (uint32_t)(cur >> 16);
}

/*
 * 从环形缓冲里取一块出来，做线性插值重采样填满 BLOCK_SAMPLES。
 */
static void audio_fill_block(struct arcade_audio_ctx *c)
{
    uint32_t avail;
    uint32_t rd;
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
        c->last_wr = c->wr_total;
        c->blocks++;
        stat_store(c, 0, 0);
        return;
    }

    read_count = audio_read_count(c, avail);

    if (read_count > avail) {
        read_count = avail;
    }
    if (read_count < 2) {
        read_count = 2;
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
    step = ((uint64_t)read_count << 16) / BLOCK_SAMPLES;

    for (i = 0; i < BLOCK_SAMPLES; i++) {
        uint32_t idx = (uint32_t)(pos >> 16);
        int32_t f = (int32_t)(pos & 0xFFFFu);
        int32_t a = c->stage[idx];
        int32_t b = c->stage[idx + 1];
        int32_t v = a + (int32_t)(((int64_t)(b - a) * f) >> 16);
        int32_t y = v - c->dc_x1 + c->dc_y1 - (c->dc_y1 >> 7);
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
    /* 进游戏/切 ROM 时产出率变了，重新学：下一块直接对齐实际产出量 */
    c->ratio_primed = false;
    c->read_q16 = 0;
    c->last_wr = c->wr_total;
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
    slew = c->ratio_slew_permille_s;
    taskEXIT_CRITICAL();

    level = wr - rd;

    n = snprintf(out, out_len,
                 "rate=%d block=%dx%d ring=%d target=%d level=%u\n",
                 ARCADE_AUDIO_RATE, BLOCK_COUNT, BLOCK_SAMPLES,
                 RING_SAMPLES, TARGET_LEVEL, (unsigned)level);
    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "vol=%u%% ratio=%u.%03ux target=%u.%03ux slew=%s%d.%02u%%/s active=%d\n",
                  (unsigned)s_vol, (unsigned)(ratio / 1000u),
                  (unsigned)(ratio % 1000u),
                  (unsigned)(target / 1000u), (unsigned)(target % 1000u),
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
