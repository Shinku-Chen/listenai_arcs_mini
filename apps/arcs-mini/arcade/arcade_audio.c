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
 *   但实测模拟器只有 52~54 fps（比实机 59.7 慢），也就是每秒只能喂
 *   41.8k 采样，而 DAC 每秒固定吃掉 48k。不做处理的话每秒会欠载 6k 采样，
 *   声音就是持续爆音。
 *   这里用「环形缓冲水位闭环」的做法：DAC 每写一块就按水位决定这一块要从
 *   环形缓冲里取多少采样（PI 控制器，长期积分项跟上「模拟器比实时慢」的稳态
 *   偏差）。取多了音调升、取少了音调降，换来的是不爆音，而且音画同步
 *   —— 视频本来就是 52fps 慢放，声音跟着一起慢才对得上。
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

    /* 消费者私有：插值小数位、水位积分项 */
    uint32_t frac_q16;
    int32_t bias;

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
    volatile int32_t bias_snapshot;
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
 * 从环形缓冲里按水位取一块出来，做线性插值重采样填满 BLOCK_SAMPLES。
 * 水位高 → 这一块多取一点（音调略升）；水位低 → 少取一点（音调略降）。
 */
static void audio_fill_block(struct arcade_audio_ctx *c)
{
    uint32_t avail;
    uint32_t rd;
    uint32_t read_count;
    uint32_t span;
    uint32_t consumed;
    uint32_t i;
    int32_t err;
    int32_t rc;
    uint64_t pos;
    uint64_t step;
    int32_t peak = 0;
    uint64_t sumsq = 0;
    int16_t *out = c->block;

    avail = c->wr_total - c->rd_total;

    if (avail < 16) {
        /* 欠载：输出静音，并把插值/DC 状态复位，避免下一块从旧数据接上来。
         * 注意不能清零 bias：那是「模拟器恒定偏慢/偏快」的学习值，
         * 一欠载就清会让它永远学不会比例，反复欠载。 */
        memset(out, 0, sizeof(int16_t) * BLOCK_SAMPLES);
        c->frac_q16 = 0;
        c->dc_x1 = 0;
        c->dc_y1 = 0;
        c->underruns++;
        c->ratio_permille = 0;
        c->bias_snapshot = c->bias;
        c->blocks++;
        stat_store(c, 0, 0);
        return;
    }

    /* PI 控制：err 是当前水位与目标的偏差。
     * bias 是积分项，负责吃掉「模拟器速度恒定偏慢/偏快」的长期偏差；
     * err/4 是比例项，负责把短时抖动拉回来。 */
    err = (int32_t)avail - TARGET_LEVEL;
    c->bias += err / 32;
    if (c->bias > 400) {
        c->bias = 400;
    } else if (c->bias < -400) {
        c->bias = -400;
    }

    rc = (int32_t)BLOCK_SAMPLES + c->bias + err / 4;
    if (rc < READ_MIN) {
        rc = READ_MIN;
    } else if (rc > READ_MAX) {
        rc = READ_MAX;
    }
    if (rc > (int32_t)avail) {
        rc = (int32_t)avail;
    }

    read_count = (uint32_t)rc;
    c->ratio_permille = read_count * 1000u / BLOCK_SAMPLES;
    c->bias_snapshot = c->bias;

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
    c->bias = 0;
    c->dc_x1 = 0;
    c->dc_y1 = 0;
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
    uint32_t level;
    int32_t bias;
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
    bias = c->bias_snapshot;
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
                  "vol=%u%% ratio=%u.%03ux bias=%d active=%d\n",
                  (unsigned)s_vol, (unsigned)(ratio / 1000u),
                  (unsigned)(ratio % 1000u), (int)bias, s_active ? 1 : 0);
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
