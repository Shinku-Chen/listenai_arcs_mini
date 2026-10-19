#include <string.h>

#include "sysheap.h"
#include "lisa_log.h"

#include "arcade_video.h"

#define TAG "arcade-video"

/* GB 原生 160x144 -> 240x216（3:2 整数最近邻放大） */
#define SRC_WIDTH   160
#define SRC_HEIGHT  144
#define SCALE_NUM   3
#define SCALE_DEN   2

static lv_color_t *s_fb;

/*
 * 经典 DMG 绿。档位 0 最亮、3 最暗。
 * 不用纯黑白是为了让 LCD 观感更接近掌机。
 */
static const uint8_t s_palette_rgb[4][3] = {
    {224, 248, 208},
    {136, 192, 112},
    { 52, 104,  86},
    {  8,  24,  32},
};

/* 预先算好的 LVGL 颜色，逐帧只做一次查表 */
static lv_color_t s_palette[4];
static bool s_palette_ready;

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

int arcade_video_init(void)
{
    palette_prepare();

    if (s_fb == NULL) {
        s_fb = (lv_color_t *)exram_malloc(4, ARCADE_FB_WIDTH * ARCADE_FB_HEIGHT * sizeof(lv_color_t));
        if (s_fb == NULL) {
            LISA_LOGE(TAG, "framebuffer alloc failed (%d bytes)",
                      ARCADE_FB_WIDTH * ARCADE_FB_HEIGHT * (int)sizeof(lv_color_t));
            return -1;
        }
    }

    arcade_video_fill_black();
    LISA_LOGI(TAG, "framebuffer %dx%d at %p", ARCADE_FB_WIDTH, ARCADE_FB_HEIGHT, s_fb);

    return 0;
}

void arcade_video_deinit(void)
{
    if (s_fb != NULL) {
        exram_free(s_fb);
        s_fb = NULL;
    }
}

lv_color_t *arcade_video_get_fb(void)
{
    return s_fb;
}

void arcade_video_fill_black(void)
{
    if (s_fb == NULL) {
        return;
    }

    for (int i = 0; i < ARCADE_FB_WIDTH * ARCADE_FB_HEIGHT; i++) {
        s_fb[i] = s_palette[3];
    }
}

void arcade_video_draw_line(int src_line, const uint8_t *shades)
{
    if (s_fb == NULL || shades == NULL) {
        return;
    }

    if (src_line < 0 || src_line >= SRC_HEIGHT) {
        return;
    }

    /* 源行号 src 覆盖目标行 [src*1.5, (src+1)*1.5)，取整后天然铺满 216 行 */
    int dst_y0 = (src_line * SCALE_NUM) / SCALE_DEN;
    int dst_y1 = ((src_line + 1) * SCALE_NUM) / SCALE_DEN;
    if (dst_y1 > ARCADE_FB_HEIGHT) {
        dst_y1 = ARCADE_FB_HEIGHT;
    }

    for (int dst_y = dst_y0; dst_y < dst_y1; dst_y++) {
        lv_color_t *row = s_fb + (dst_y * ARCADE_FB_WIDTH);

        for (int dst_x = 0; dst_x < ARCADE_FB_WIDTH; dst_x++) {
            int src_x = (dst_x * SCALE_DEN) / SCALE_NUM;
            if (src_x >= SRC_WIDTH) {
                src_x = SRC_WIDTH - 1;
            }
            row[dst_x] = s_palette[shades[src_x] & 0x03];
        }
    }
}
