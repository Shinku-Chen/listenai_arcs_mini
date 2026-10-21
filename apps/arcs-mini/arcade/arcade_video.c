/*
 * 帧输出：模拟器 RGBA 帧 → RGB565 → 面板。
 *
 * 为什么不走 LVGL：
 *   LVGL 那条路每帧要把画面先画进 canvas，再复制进 draw buffer，最后交给 flush
 *   线程走 SPI。实测 lv_task_handler 单独就吃掉 8ms/帧（103KB 的 PSRAM 拷贝 +
 *   对象/脏区处理），而模拟器本身只有 20~30fps，这 8ms 直接换算成 fps。
 *
 * 现在的结构（三级流水 + 源帧双缓冲）：
 *
 *   模拟器任务：把 160x144 的帧直接渲染进 src[0/1]（binjgb 的 frame_buffer
 *               换成指针，帧与帧交替），渲染完只投递索引，不做缩放
 *   缩放任务  ：src → RGB565 1.5 倍放大 → dst[0..2]（1.7ms/帧）
 *   送屏任务  ：lisa_display_write(dst)（SPI + DMA，阻塞 ~17.6ms/帧）
 *
 * 关键点：缩放（1.9ms）原来串在模拟器任务里，把 16.5ms 的模拟硬生生拖成
 * 18.4ms 的帧周期。拆成三级之后帧周期由最慢的一级决定（模拟 16.5ms /
 * 送屏 17.7ms），缩放被完全藏进流水线。各级之间用队列传递缓冲索引：
 * 队列空就说明下游还没消化完，上游等一等 —— 天然限流，不会撕裂、不会丢帧。
 *
 * 目标缓冲为什么是 3 块而不是 2 块：
 *   2 块时，「一块在 DMA 上、一块在送屏队列里」就把两块都占住了，缩放任务
 *   拿不到空块、只能一直攥着源缓冲等，模拟器随后在 begin_frame() 里被拖住。
 *   实测这一等就是 2.9ms/渲染帧，渲染帧率从 33 掉到 17（presents 0 vs 1）。
 *   3 块则任何时刻都有一块可写，送屏彻底从模拟路径上解耦。
 *
 * 模拟器那一侧**永不阻塞**：begin_frame() 用 0 超时取源缓冲，取不到就把
 * 这一帧画到模拟器内部缓冲里（画面少一帧，逻辑一帧不落）。这是「逻辑/音频
 * 不被送屏拖慢」的硬保证。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "sysheap.h"
#include "esp_heap_caps.h"
#include "lisa_device.h"
#include "lisa_display.h"
#include "lisa_spi.h"
#include "lisa_log.h"

#include "arcade_video.h"

#define TAG "arcade-video"

/* GB 原生 160x144 */
#define SRC_WIDTH   160
#define SRC_HEIGHT  144

/* 送屏任务优先级：高于模拟器任务（4），低于音频任务（6） */
#define PRESENT_TASK_PRIO    5
#define PRESENT_TASK_STACK   3072

/* 1.5 倍时目标 240x216；1 倍时 160x144 原尺寸居中 */
#define SCALE_1X     1
#define SCALE_1_5X   2

/*
 * 经典 DMG 绿。档位 0 最亮、3 最暗。
 * 不用纯黑白是为了让 LCD 观感更接近掌机。
 */
static const uint8_t s_palette_rgb[4][3] = {
    {224, 248, 208},
    {136, 192, 112},
    { 52, 104,  86},
    {  8,  24, 32},
};

/* 预先算好的 LVGL 颜色，逐帧只做一次查表 */
static lv_color_t s_palette[4];
static bool s_palette_ready;

/* ---- 送屏管线 ---- */
#define PRESENT_BUF_CNT  3
static lv_color_t *s_buf[PRESENT_BUF_CNT];
static int s_buf_cnt;                       /* 实际分到的目标缓冲块数：2/3 */
static uint32_t s_buf_bytes;

/* ---- 源帧（模拟器渲染目标）双缓冲 ---- */
#define SRC_BUF_CNT  2
static arcade_fbpixel_t *s_src[SRC_BUF_CNT];
static int s_src_slots;                     /* 实际分到的块数：0/1/2 */
static QueueHandle_t s_src_free_q;          /* 可以拿来渲染的源缓冲索引 */
static QueueHandle_t s_src_q;               /* 待缩放的源缓冲索引 */
static int s_src_cur = -1;                  /* 本帧正在渲染的源索引，-1 = 没取到 */
static int s_scale = SCALE_1_5X;
static int s_stride = ARCADE_FB_WIDTH;      /* 一行多少像素（u16） */
static int s_fb_w = ARCADE_FB_WIDTH;
static int s_fb_h = ARCADE_FB_HEIGHT;
static int s_rect_x;                        /* 面板上的位置 */
static int s_rect_y = (240 - ARCADE_FB_HEIGHT) / 2;
static lv_color_t *s_last;                  /* 最近一次提交的缓冲（调试用） */
/* 180° 硬件翻转的行偏移覆盖（-1 = 驱动自动推算），调试用 */
static int16_t s_rotate_row_shift = -1;
/* 当前（上一次接受成功的）面板 SPI 时钟，改频失败时用来回滚 */
static uint32_t s_spi_freq_backup = 50000000U;

static lisa_device_t *s_disp;
static QueueHandle_t s_free_q;              /* 可写的目标缓冲索引 */
static QueueHandle_t s_tx_q;                /* 待送的缓冲索引 */
static TaskHandle_t s_task;                 /* 送屏任务 */
static TaskHandle_t s_blit_task;

static void blit_task(void *arg);

/* 计时（cycle 累计，按帧平均；rdcycle 精度 1/300µs，tick 只有 1ms 差太多） */
static volatile uint64_t s_perf_blit;
static volatile uint64_t s_perf_wait;
static volatile uint64_t s_perf_tx;
static volatile uint32_t s_perf_frames;      /* 模拟器提交的帧数 */
static volatile uint32_t s_perf_blit_frames;
static volatile uint32_t s_perf_tx_frames;

static void palette_prepare(void)
{
    if (s_palette_ready) {
        return;
    }

    for (int i = 0; i < 4; i++) {
        s_palette[i] = lv_color_make(s_palette_rgb[i][0],
                                     s_palette_rgb[i][1],
                                     s_palette_rgb[i][2]);
    }
    s_palette_ready = true;
}

/* 缩放比例变化时重算几何：缓冲首地址不变，只是行宽和送屏矩形不同 */
static void geometry_update(void)
{
    if (s_scale == SCALE_1X) {
        s_stride = SRC_WIDTH;
        s_fb_w = SRC_WIDTH;
        s_fb_h = SRC_HEIGHT;
        s_rect_x = (240 - SRC_WIDTH) / 2;
        s_rect_y = (240 - SRC_HEIGHT) / 2;
    } else {
        s_stride = ARCADE_FB_WIDTH;
        s_fb_w = ARCADE_FB_WIDTH;
        s_fb_h = ARCADE_FB_HEIGHT;
        s_rect_x = 0;
        s_rect_y = (240 - ARCADE_FB_HEIGHT) / 2;
    }
}

static void present_task(void *arg)
{
    (void)arg;

    for (;;) {
        int idx;
        lisa_display_buffer_desc_t desc;
        uint32_t t0;

        if (xQueueReceive(s_tx_q, &idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        memset(&desc, 0, sizeof(desc));
        desc.width = (uint16_t)s_fb_w;
        desc.height = (uint16_t)s_fb_h;
        desc.pitch = (uint16_t)(s_fb_w * (int)sizeof(uint16_t));
        desc.buf_size = (uint32_t)s_fb_w * (uint32_t)s_fb_h * sizeof(uint16_t);

        t0 = arcade_cycles();
        if (s_disp != NULL) {
            int ret = lisa_display_write(s_disp, (uint16_t)s_rect_x, (uint16_t)s_rect_y,
                                         &desc, s_buf[idx]);

            if (ret != 0 && s_perf_tx_frames == 0) {
                LISA_LOGW(TAG, "display write failed: %d", ret);
            }
        }
        s_perf_tx += (uint32_t)(arcade_cycles() - t0);
        s_perf_tx_frames++;

        xQueueSend(s_free_q, &idx, portMAX_DELAY);
    }
}

static bool present_start(void)
{
    if (s_task != NULL) {
        return true;
    }

    if (s_disp == NULL) {
        s_disp = lisa_device_get("display");
        if (s_disp == NULL) {
            LISA_LOGE(TAG, "display device not found");
            return false;
        }
    }

    if (s_free_q == NULL) {
        if (s_buf_cnt <= 0) {
            return false;
        }

        s_free_q = xQueueCreate((UBaseType_t)s_buf_cnt, sizeof(int));
        s_tx_q = xQueueCreate((UBaseType_t)s_buf_cnt, sizeof(int));
        if (s_free_q == NULL || s_tx_q == NULL) {
            LISA_LOGE(TAG, "present queue create failed");
            return false;
        }

        for (int i = 0; i < s_buf_cnt; i++) {
            xQueueSend(s_free_q, &i, 0);
        }
    }

    if (s_src_free_q == NULL && s_src_slots > 0) {
        s_src_free_q = xQueueCreate((UBaseType_t)s_src_slots, sizeof(int));
        s_src_q = xQueueCreate((UBaseType_t)s_src_slots, sizeof(int));
        if (s_src_free_q == NULL || s_src_q == NULL) {
            LISA_LOGE(TAG, "source queue create failed");
            s_src_slots = 0;
        } else {
            for (int i = 0; i < s_src_slots; i++) {
                xQueueSend(s_src_free_q, &i, 0);
            }
        }
    }

    if (s_src_slots > 0 && s_blit_task == NULL) {
        if (xTaskCreate(blit_task, "arcade_blit", PRESENT_TASK_STACK, NULL,
                        PRESENT_TASK_PRIO, &s_blit_task) != pdPASS) {
            LISA_LOGE(TAG, "blit task create failed");
            s_blit_task = NULL;
            s_src_slots = 0;
        }
    }

    if (xTaskCreate(present_task, "arcade_disp", PRESENT_TASK_STACK, NULL,
                    PRESENT_TASK_PRIO, &s_task) != pdPASS) {
        LISA_LOGE(TAG, "present task create failed");
        s_task = NULL;
        return false;
    }

    LISA_LOGI(TAG, "present task ready, %dx%d scale 1/%d, src slots %d",
              s_fb_w, s_fb_h, s_scale == SCALE_1X ? 1 : 2, s_src_slots);
    return true;
}

int arcade_video_init(void)
{
    size_t need = (size_t)ARCADE_FB_WIDTH * ARCADE_FB_HEIGHT * sizeof(lv_color_t);

    palette_prepare();
    geometry_update();

    if (s_buf[0] == NULL) {
        size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        int want = PRESENT_BUF_CNT;

        /* 三块 240x216 RGB565 ≈ 304KB；PSRAM 紧就退回 2 块（渲染帧率会掉） */
        while (want > 2 && free_bytes < need * (size_t)want + 128U * 1024U) {
            want--;
        }
        if (free_bytes < need * (size_t)want + 96U * 1024U) {
            LISA_LOGE(TAG, "not enough psram for frame buffers (%u KB free)",
                      (unsigned)(free_bytes / 1024));
            return -1;
        }

        for (int i = 0; i < want; i++) {
            s_buf[i] = (lv_color_t *)exram_malloc(4, need);
            if (s_buf[i] == NULL) {
                LISA_LOGE(TAG, "framebuffer alloc failed (%u bytes)", (unsigned)need);
                for (int j = 0; j < i; j++) {
                    exram_free(s_buf[j]);
                    s_buf[j] = NULL;
                }
                return -1;
            }
        }
        s_buf_cnt = want;
        s_buf_bytes = (uint32_t)need;
    }

    /* 源帧双缓冲：模拟器这一帧写 A，缩放任务同时读上一帧的 B。
     * 拿不到就退化成单块（模拟器和缩放串行，跟以前一样，只是不再丢功能）。 */
    if (s_src[0] == NULL) {
        size_t src_need = (size_t)SRC_WIDTH * SRC_HEIGHT * sizeof(arcade_fbpixel_t);
        int got = 0;

        for (int i = 0; i < SRC_BUF_CNT; i++) {
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < src_need + 96 * 1024) {
                break;
            }
            s_src[i] = (arcade_fbpixel_t *)exram_malloc(4, src_need);
            if (s_src[i] == NULL) {
                break;
            }
            got++;
        }

        s_src_slots = got;
        LISA_LOGI(TAG, "source frames %d x %u bytes", got, (unsigned)src_need);
    }

    arcade_video_fill_black();

    /* 记住面板当前的实际 SPI 频率，`arcade spi` 改频失败时用它回滚 */
    if (arcade_video_spi_freq() != 0) {
        s_spi_freq_backup = arcade_video_spi_freq();
    }

    LISA_LOGI(TAG, "framebuffer %dx%d x%d at %p", ARCADE_FB_WIDTH, ARCADE_FB_HEIGHT,
              s_buf_cnt, (void *)s_buf[0]);

    return 0;
}

void arcade_video_deinit(void)
{
    /* 送屏任务不销毁（帧缓冲是静态生命周期），这里只释放缓冲 */
    for (int i = 0; i < SRC_BUF_CNT; i++) {
        if (s_src[i] != NULL) {
            exram_free(s_src[i]);
            s_src[i] = NULL;
        }
    }
    for (int i = 0; i < PRESENT_BUF_CNT; i++) {
        if (s_buf[i] != NULL) {
            exram_free(s_buf[i]);
            s_buf[i] = NULL;
        }
    }
    s_src_slots = 0;
    s_last = NULL;
}

lv_color_t *arcade_video_get_fb(void)
{
    return s_last != NULL ? s_last : s_buf[0];
}

int arcade_video_fb_width(void)
{
    return s_fb_w;
}

int arcade_video_fb_height(void)
{
    return s_fb_h;
}

void arcade_video_fill_black(void)
{
    arcade_video_fill(3);
}

void arcade_video_fill(int shade)
{
    if (shade < 0 || shade > 3) {
        shade = 3;
    }

    for (int b = 0; b < PRESENT_BUF_CNT; b++) {
        if (s_buf[b] == NULL) {
            continue;
        }

        uint16_t *p = (uint16_t *)s_buf[b];
        int n = s_stride * s_fb_h;

        for (int i = 0; i < n; i++) {
            p[i] = s_palette[shade].full;
        }
    }
}

uint16_t arcade_video_palette_raw(int shade)
{
    lv_color_t c;

    palette_prepare();

    c = s_palette[shade & 0x03];

    return c.full;
}

const uint8_t (*arcade_video_dmg_palette_rgb(void))[3]
{
    return s_palette_rgb;
}

void arcade_video_set_scale(int scale)
{
    /* 几何参数（行宽/送屏矩形）是送屏和缩放两个任务共用的，必须等流水线清空
     * 再改，否则在途的那一帧会按旧几何去缩放/送屏。 */
    if (s_task != NULL) {
        arcade_video_present_drain();
    }

    s_scale = (scale == SCALE_1X) ? SCALE_1X : SCALE_1_5X;
    geometry_update();
    LISA_LOGI(TAG, "scale %s (%dx%d)", s_scale == SCALE_1X ? "1x" : "1.5x",
              s_fb_w, s_fb_h);
}

int arcade_video_scale(void)
{
    return s_scale;
}

void arcade_video_set_rotate_row_shift(int16_t shift)
{
    s_rotate_row_shift = shift;
    lisa_display_set_rotate_row_shift(shift);
    LISA_LOGI(TAG, "rotate row shift %d", (int)shift);
}

int16_t arcade_video_rotate_row_shift(void)
{
    return s_rotate_row_shift;
}

/*
 * 运行期改面板 SPI 时钟（`arcade spi <MHz>`）。
 *
 * 前提：调用之前流水线已经清空（没有在途 DMA），否则 SPI_Control 会回 BUSY。
 * 这里只改 lisa_spi 的当前配置，bus_spi4 下一帧 configure 时会带上新频率。
 */
int arcade_video_set_spi_freq(uint32_t hz)
{
    lisa_device_t *dev = lisa_device_get("spi0");
    lisa_spi_config_t cfg;
    int ret;

    if (dev == NULL) {
        return -1;
    }
    if (lisa_spi_get_config(dev, &cfg) != 0) {
        return -2;
    }
    if (hz == 0) {
        return (int)cfg.frequency;
    }

    cfg.frequency = hz;
    ret = lisa_spi_configure(dev, &cfg);
    if (ret != 0) {
        /* 改失败：把原来的频率写回去，别把屏线路弄成未知状态 */
        LISA_LOGW(TAG, "spi %u Hz rejected (%d), restoring %u Hz",
                  (unsigned)hz, ret, (unsigned)s_spi_freq_backup);
        cfg.frequency = s_spi_freq_backup;
        (void)lisa_spi_configure(dev, &cfg);
        return -3;
    }

    s_spi_freq_backup = hz;
    LISA_LOGI(TAG, "display spi -> %u Hz", (unsigned)hz);
    return 0;
}

uint32_t arcade_video_spi_freq(void)
{
    lisa_device_t *dev = lisa_device_get("spi0");
    lisa_spi_config_t cfg;

    if (dev == NULL || lisa_spi_get_config(dev, &cfg) != 0) {
        return 0;
    }
    return cfg.frequency;
}

/* RGBA(0xAABBGGRR) → RGB565，和 LVGL 的 lv_color_make 出同样的位模式 */
#define RGBA_TO_565(c)                                  \
    ((uint16_t)((((c) >> 3) & 0x001FU) |                \
                ((((c) >> 10) & 0x003FU) << 5) |        \
                ((((c) >> 19) & 0x001FU) << 11)))

/*
 * 1 倍：160x144 原样，一条源行对一条目标行。
 * 1.5 倍：水平方向 2 个源像素铺 3 个目标像素，
 *         垂直方向偶数源行铺 1 行、奇数源行铺 2 行 —— 144 条源行正好铺满 216 行。
 *
 * 水平放大按「4 个源像素 → 6 个目标像素」一组做：
 *   src a,b,c,d  →  dst a,a,b,c,c,d
 * 6 个 u16 = 12 字节，每轮刚好 3 个 4 字节对齐的 32 位写，
 * 比原来每对源像素写 3 次 16 位少一半 store，而且不用拼非对齐的 32 位写。
 *
 * ARCADE_FB_RGB565=1 时源已经是 RGB565，这里只做缩放，不做色彩转换。
 */
static void blit_frame(uint16_t *dst, const arcade_fbpixel_t *src)
{
    if (s_scale == SCALE_1X) {
        for (int y = 0; y < SRC_HEIGHT; y++) {
            uint16_t *d = dst + (size_t)y * SRC_WIDTH;
            const arcade_fbpixel_t *s = src + (size_t)y * SRC_WIDTH;

#if ARCADE_FB_RGB565
            memcpy(d, s, (size_t)SRC_WIDTH * sizeof(uint16_t));
#else
            for (int x = 0; x < SRC_WIDTH; x++) {
                d[x] = RGBA_TO_565(s[x]);
            }
#endif
        }
        return;
    }

    int dy = 0;

    for (int y = 0; y < SRC_HEIGHT; y++) {
        uint16_t *d = dst + (size_t)dy * ARCADE_FB_WIDTH;
        const arcade_fbpixel_t *s = src + (size_t)y * SRC_WIDTH;

        for (int x = 0; x < SRC_WIDTH; x += 4) {
#if ARCADE_FB_RGB565
            uint32_t a = (uint16_t)s[x];
            uint32_t b = (uint16_t)s[x + 1];
            uint32_t c = (uint16_t)s[x + 2];
            uint32_t e = (uint16_t)s[x + 3];
#else
            uint32_t a = RGBA_TO_565(s[x]);
            uint32_t b = RGBA_TO_565(s[x + 1]);
            uint32_t c = RGBA_TO_565(s[x + 2]);
            uint32_t e = RGBA_TO_565(s[x + 3]);
#endif
            uint32_t *p32 = (uint32_t *)d;

            /* dst: a,a,b,c,c,d */
            p32[0] = a | (a << 16);
            p32[1] = b | (c << 16);
            p32[2] = c | (e << 16);
            d += 6;
        }

        dy++;

        if (y & 1) {
            /* 奇数源行铺两行：直接拷上面那一行 */
            memcpy(dst + (size_t)dy * ARCADE_FB_WIDTH,
                   dst + (size_t)(dy - 1) * ARCADE_FB_WIDTH,
                   (size_t)ARCADE_FB_WIDTH * sizeof(uint16_t));
            dy++;
        }
    }
}

static void blit_task(void *arg)
{
    (void)arg;

    for (;;) {
        int src_idx;
        int dst_idx;
        uint32_t t0;
        uint32_t t1;

        if (xQueueReceive(s_src_q, &src_idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        t0 = arcade_cycles();
        if (xQueueReceive(s_free_q, &dst_idx, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* 所有目标缓冲都还在 SPI 上：这一帧丢掉，但源缓冲必须还回去，
             * 否则模拟器任务会少一块可渲染的源缓冲 */
            s_perf_wait += (uint32_t)(arcade_cycles() - t0);
            xQueueSend(s_src_free_q, &src_idx, 0);
            continue;
        }
        t1 = arcade_cycles();
        s_perf_wait += (uint32_t)(t1 - t0);

        blit_frame((uint16_t *)s_buf[dst_idx], s_src[src_idx]);

        s_perf_blit += (uint32_t)(arcade_cycles() - t1);
        s_perf_blit_frames++;

        if (xQueueSend(s_tx_q, &dst_idx, 0) != pdTRUE) {
            /* 送屏队列满（不该发生，块数 ≥ 队列深度）：丢这一帧但绝不丢缓冲，
             * 丢缓冲会让流水线永久少一块，几分钟后彻底停住 */
            xQueueSend(s_free_q, &dst_idx, 0);
        } else {
            s_last = s_buf[dst_idx];
        }

        xQueueSend(s_src_free_q, &src_idx, 0);
    }
}

arcade_fbpixel_t *arcade_video_begin_frame(void)
{
    int idx;

    if (s_src_slots == 0 || s_buf[0] == NULL || !present_start()) {
        return NULL;
    }

    /* 0 超时：模拟器绝不等送屏流水线。取不到就把这一帧画到模拟器内部缓冲
     * （画面少一帧），逻辑/音频一帧不落 —— 这是「速度优先」的硬保证。 */
    if (xQueueReceive(s_src_free_q, &idx, 0) != pdTRUE) {
        return NULL;
    }

    s_src_cur = idx;
    return s_src[idx];
}

void arcade_video_end_frame(bool present)
{
    int idx = s_src_cur;

    s_src_cur = -1;

    if (idx < 0) {
        return;
    }

    s_perf_frames++;

    if (!present || xQueueSend(s_src_q, &idx, 0) != pdTRUE) {
        xQueueSend(s_src_free_q, &idx, 0);
    }
}

void arcade_video_present_now(void)
{
    int idx;

    if (s_buf[0] == NULL || !present_start()) {
        return;
    }

    if (xQueueReceive(s_free_q, &idx, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    s_last = s_buf[idx];
    xQueueSend(s_tx_q, &idx, 0);
}

void arcade_video_present_drain(void)
{
    int got[PRESENT_BUF_CNT];
    int n = 0;

    if (s_free_q == NULL || s_task == NULL) {
        return;
    }

    while (n < s_buf_cnt &&
           xQueueReceive(s_free_q, &got[n], pdMS_TO_TICKS(500)) == pdTRUE) {
        n++;
    }

    for (int i = 0; i < n; i++) {
        xQueueSend(s_free_q, &got[i], 0);
    }

    /* 源缓冲也一并收回，避免改几何/切页面时还有一帧在缩放任务手里 */
    if (s_src_free_q != NULL) {
        int sgot[2];
        int sn = 0;

        while (sn < s_src_slots &&
               xQueueReceive(s_src_free_q, &sgot[sn], pdMS_TO_TICKS(200)) == pdTRUE) {
            sn++;
        }

        for (int i = 0; i < sn; i++) {
            xQueueSend(s_src_free_q, &sgot[i], 0);
        }
    }
}

void arcade_video_reset_perf(void)
{
    s_perf_blit = 0;
    s_perf_wait = 0;
    s_perf_tx = 0;
    s_perf_frames = 0;
    s_perf_blit_frames = 0;
    s_perf_tx_frames = 0;
}

void arcade_video_perf(char *out, uint32_t len)
{
    uint32_t f = s_perf_frames ? s_perf_frames : 1;
    uint32_t b = s_perf_blit_frames ? s_perf_blit_frames : 1;
    uint32_t t = s_perf_tx_frames ? s_perf_tx_frames : 1;
    /* 送屏带宽（按写出字节 / 纯传输耗时） */
    uint32_t tx_rate_kbps = 0;
    uint32_t bytes = (uint32_t)s_fb_w * (uint32_t)s_fb_h * 2U;
    uint32_t blit_us = arcade_cyc_to_us(s_perf_blit / b);
    uint32_t wait_us = arcade_cyc_to_us(s_perf_wait / f);
    uint32_t tx_us = arcade_cyc_to_us(s_perf_tx / t);

    if (out == NULL || len == 0) {
        return;
    }

    if (tx_us > 0) {
        /* tx_us 是单帧平均耗时，bytes 是单帧字节数 → bytes/us = KB/ms */
        tx_rate_kbps = (uint32_t)(((uint64_t)bytes * 1000ULL) / (uint64_t)tx_us);
    }

    snprintf(out, len,
             "scale=%s %dx%d at %d,%d\n"
             "present blit=%u.%uus wait=%u.%uus tx=%u.%uus\n"
             "frames=%u blitf=%u txf=%u txcache=%u KB/s",
             s_scale == SCALE_1X ? "1x" : "1.5x", s_fb_w, s_fb_h, s_rect_x, s_rect_y,
             (unsigned)(blit_us / 1000), (unsigned)((blit_us / 100) % 10),
             (unsigned)(wait_us / 1000), (unsigned)((wait_us / 100) % 10),
             (unsigned)(tx_us / 1000), (unsigned)((tx_us / 100) % 10),
             (unsigned)s_perf_frames, (unsigned)s_perf_blit_frames,
             (unsigned)s_perf_tx_frames, (unsigned)tx_rate_kbps);
}

/* ---------------- Peanut-GB（DMG 内核）用到的逐行接口 ---------------- */

void arcade_video_draw_line(int src_line, const uint8_t *shades)
{
    if (s_buf[0] == NULL || shades == NULL) {
        return;
    }

    if (src_line < 0 || src_line >= SRC_HEIGHT) {
        return;
    }

    uint16_t *fb = (uint16_t *)arcade_video_get_fb();

    /* 源行号 src 覆盖目标行 [src*1.5, (src+1)*1.5)，取整后天然铺满 216 行 */
    int dst_y0 = (src_line * 3) / 2;
    int dst_y1 = ((src_line + 1) * 3) / 2;
    if (dst_y1 > ARCADE_FB_HEIGHT) {
        dst_y1 = ARCADE_FB_HEIGHT;
    }

    for (int dst_y = dst_y0; dst_y < dst_y1; dst_y++) {
        uint16_t *row = fb + (size_t)dst_y * ARCADE_FB_WIDTH;

        for (int dst_x = 0; dst_x < ARCADE_FB_WIDTH; dst_x++) {
            int src_x = (dst_x * 2) / 3;
            if (src_x >= SRC_WIDTH) {
                src_x = SRC_WIDTH - 1;
            }
            row[dst_x] = s_palette[shades[src_x] & 0x03].full;
        }
    }
}
