/*
 * 帧输出：模拟器 RGBA 帧 → RGB565 → 面板。
 *
 * 为什么不走 LVGL：
 *   LVGL 那条路每帧要把画面先画进 canvas，再复制进 draw buffer，最后交给 flush
 *   线程走 SPI。实测 lv_task_handler 单独就吃掉 8ms/帧（103KB 的 PSRAM 拷贝 +
 *   对象/脏区处理），而模拟器本身只有 20~30fps，这 8ms 直接换算成 fps。
 *
 * 现在的结构（双缓冲 + 独立送屏任务）：
 *
 *   模拟器任务：emu → 把 RGBA 转 RGB565 写进 back buffer → 投递索引
 *   送屏任务 ：lisa_display_write(back buffer)（等 TE + SPI，阻塞）
 *
 * 转码（CPU）和 SPI 传输（DMA）并行，屏上的开销基本被模拟器吃掉。
 * 两块缓冲用「空闲队列」管理：模拟器取一块填，送屏任务发完把索引还回队列；
 * 队列空说明两块都在用（例如 1.5 倍 240x216 的 SPI 传输慢于模拟），
 * 这时模拟器任务会阻塞等 —— 天然限流，不会撕裂、不会丢帧。
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
static lv_color_t *s_buf[2];
static uint32_t s_buf_bytes;
static int s_scale = SCALE_1_5X;
static int s_stride = ARCADE_FB_WIDTH;      /* 一行多少像素（u16） */
static int s_fb_w = ARCADE_FB_WIDTH;
static int s_fb_h = ARCADE_FB_HEIGHT;
static int s_rect_x;                        /* 面板上的位置 */
static int s_rect_y = (240 - ARCADE_FB_HEIGHT) / 2;
static lv_color_t *s_last;                  /* 最近一次提交的缓冲（调试用） */

static lisa_device_t *s_disp;
static QueueHandle_t s_free_q;              /* 可写的缓冲索引 */
static QueueHandle_t s_tx_q;                /* 待送的缓冲索引 */
static TaskHandle_t s_task;

/* 计时（ms tick 累计，按帧平均） */
static volatile TickType_t s_perf_blit;
static volatile TickType_t s_perf_wait;
static volatile TickType_t s_perf_tx;
static volatile uint32_t s_perf_frames;
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
        TickType_t t0;

        if (xQueueReceive(s_tx_q, &idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        memset(&desc, 0, sizeof(desc));
        desc.width = (uint16_t)s_fb_w;
        desc.height = (uint16_t)s_fb_h;
        desc.pitch = (uint16_t)(s_fb_w * (int)sizeof(uint16_t));
        desc.buf_size = (uint32_t)s_fb_w * (uint32_t)s_fb_h * sizeof(uint16_t);

        t0 = xTaskGetTickCount();
        if (s_disp != NULL) {
            int ret = lisa_display_write(s_disp, (uint16_t)s_rect_x, (uint16_t)s_rect_y,
                                         &desc, s_buf[idx]);

            if (ret != 0 && s_perf_tx_frames == 0) {
                LISA_LOGW(TAG, "display write failed: %d", ret);
            }
        }
        s_perf_tx += xTaskGetTickCount() - t0;
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
        s_free_q = xQueueCreate(2, sizeof(int));
        s_tx_q = xQueueCreate(2, sizeof(int));
        if (s_free_q == NULL || s_tx_q == NULL) {
            LISA_LOGE(TAG, "present queue create failed");
            return false;
        }

        for (int i = 0; i < 2; i++) {
            xQueueSend(s_free_q, &i, 0);
        }
    }

    if (xTaskCreate(present_task, "arcade_disp", PRESENT_TASK_STACK, NULL,
                    PRESENT_TASK_PRIO, &s_task) != pdPASS) {
        LISA_LOGE(TAG, "present task create failed");
        s_task = NULL;
        return false;
    }

    LISA_LOGI(TAG, "present task ready, %dx%d scale 1/%d", s_fb_w, s_fb_h,
              s_scale == SCALE_1X ? 1 : 2);
    return true;
}

int arcade_video_init(void)
{
    size_t need = (size_t)ARCADE_FB_WIDTH * ARCADE_FB_HEIGHT * sizeof(lv_color_t);

    palette_prepare();
    geometry_update();

    if (s_buf[0] == NULL) {
        size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        /* 两块 240x216 RGB565 ≈ 202KB；余量不够就只开一块（无并行送屏） */
        if (free_bytes < need * 2 + 128 * 1024) {
            LISA_LOGE(TAG, "not enough psram for frame buffers (%u KB free)",
                      (unsigned)(free_bytes / 1024));
            return -1;
        }

        for (int i = 0; i < 2; i++) {
            s_buf[i] = (lv_color_t *)exram_malloc(4, need);
            if (s_buf[i] == NULL) {
                LISA_LOGE(TAG, "framebuffer alloc failed (%u bytes)", (unsigned)need);
                if (i == 1) {
                    exram_free(s_buf[0]);
                    s_buf[0] = NULL;
                }
                return -1;
            }
        }
        s_buf_bytes = (uint32_t)need;
    }

    arcade_video_fill_black();

    LISA_LOGI(TAG, "framebuffer %dx%d x2 at %p/%p", ARCADE_FB_WIDTH, ARCADE_FB_HEIGHT,
              (void *)s_buf[0], (void *)s_buf[1]);

    return 0;
}

void arcade_video_deinit(void)
{
    /* 送屏任务不销毁（帧缓冲是静态生命周期），这里只释放缓冲 */
    for (int i = 0; i < 2; i++) {
        if (s_buf[i] != NULL) {
            exram_free(s_buf[i]);
            s_buf[i] = NULL;
        }
    }
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

    for (int b = 0; b < 2; b++) {
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
    s_scale = (scale == SCALE_1X) ? SCALE_1X : SCALE_1_5X;
    geometry_update();
    LISA_LOGI(TAG, "scale %s (%dx%d)", s_scale == SCALE_1X ? "1x" : "1.5x",
              s_fb_w, s_fb_h);
}

int arcade_video_scale(void)
{
    return s_scale;
}

/* RGBA(0xAABBGGRR) → RGB565，和 LVGL 的 lv_color_make 出同样的位模式 */
#define RGBA_TO_565(c)                                  \
    ((uint16_t)((((c) >> 3) & 0x001FU) |                \
                ((((c) >> 10) & 0x003FU) << 5) |        \
                ((((c) >> 19) & 0x001FU) << 11)))

/*
 * 1 倍：160x144 原样，一条源行对一条目标行。
 * 1.5 倍：水平方向 2 个源像素铺 3 个目标像素（偶数列复制一份），
 *         垂直方向偶数源行铺 1 行、奇数源行铺 2 行 —— 144 条源行正好铺满 216 行。
 * 两个循环都是「按源顺序读、按目标顺序写」，没有随机跳读，也没有逐像素查表。
 */
static void blit_frame(uint16_t *dst, const uint32_t *src)
{
    if (s_scale == SCALE_1X) {
        for (int y = 0; y < SRC_HEIGHT; y++) {
            uint16_t *d = dst + (size_t)y * SRC_WIDTH;
            const uint32_t *s = src + (size_t)y * SRC_WIDTH;

            for (int x = 0; x < SRC_WIDTH; x++) {
                d[x] = RGBA_TO_565(s[x]);
            }
        }
        return;
    }

    int dy = 0;

    for (int y = 0; y < SRC_HEIGHT; y++) {
        uint16_t *d = dst + (size_t)dy * ARCADE_FB_WIDTH;
        const uint32_t *s = src + (size_t)y * SRC_WIDTH;

        for (int x = 0; x < SRC_WIDTH; x += 2) {
            uint16_t a = RGBA_TO_565(s[x]);
            uint16_t b = RGBA_TO_565(s[x + 1]);

            d[0] = a;
            d[1] = a;
            d[2] = b;
            d += 3;
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

void arcade_video_present_frame(const uint32_t *rgba)
{
    int idx;
    TickType_t t0;
    TickType_t t1;

    if (rgba == NULL || s_buf[0] == NULL) {
        return;
    }

    if (!present_start()) {
        return;
    }

    t0 = xTaskGetTickCount();
    if (xQueueReceive(s_free_q, &idx, pdMS_TO_TICKS(200)) != pdTRUE) {
        /* 两块缓冲都在送屏（屏比模拟器慢），这一帧丢掉，别把模拟器拖住 */
        s_perf_frames++;
        return;
    }
    t1 = xTaskGetTickCount();
    s_perf_wait += t1 - t0;

    blit_frame((uint16_t *)s_buf[idx], rgba);

    s_last = s_buf[idx];
    s_perf_blit += xTaskGetTickCount() - t1;
    s_perf_frames++;

    xQueueSend(s_tx_q, &idx, 0);
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
    int got[2];
    int n = 0;

    if (s_free_q == NULL || s_task == NULL) {
        return;
    }

    while (n < 2 &&
           xQueueReceive(s_free_q, &got[n], pdMS_TO_TICKS(500)) == pdTRUE) {
        n++;
    }

    for (int i = 0; i < n; i++) {
        xQueueSend(s_free_q, &got[i], 0);
    }
}

void arcade_video_reset_perf(void)
{
    s_perf_blit = 0;
    s_perf_wait = 0;
    s_perf_tx = 0;
    s_perf_frames = 0;
    s_perf_tx_frames = 0;
}

void arcade_video_perf(char *out, uint32_t len)
{
    uint32_t f = s_perf_frames ? s_perf_frames : 1;
    uint32_t t = s_perf_tx_frames ? s_perf_tx_frames : 1;

    if (out == NULL || len == 0) {
        return;
    }

    snprintf(out, len,
             "scale=%s %dx%d at %d,%d\n"
             "present blit=%u.%ums wait=%u.%ums tx=%u.%ums frames=%u txf=%u",
             s_scale == SCALE_1X ? "1x" : "1.5x", s_fb_w, s_fb_h, s_rect_x, s_rect_y,
             (unsigned)(s_perf_blit / f), (unsigned)((s_perf_blit * 10 / f) % 10),
             (unsigned)(s_perf_wait / f), (unsigned)((s_perf_wait * 10 / f) % 10),
             (unsigned)(s_perf_tx / t), (unsigned)((s_perf_tx * 10 / t) % 10),
             (unsigned)s_perf_frames, (unsigned)s_perf_tx_frames);
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
