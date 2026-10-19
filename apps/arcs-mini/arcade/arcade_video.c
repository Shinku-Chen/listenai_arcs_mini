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
    arcade_video_fill(3);
}

void arcade_video_fill(int shade)
{
    if (s_fb == NULL) {
        return;
    }

    if (shade < 0 || shade > 3) {
        shade = 3;
    }

    for (int i = 0; i < ARCADE_FB_WIDTH * ARCADE_FB_HEIGHT; i++) {
        s_fb[i] = s_palette[shade];
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

/*
 * 一帧 RGBA（binjgb 输出格式：低字节 R、然后 G、B、A）直接缩放成 240x216。
 *
 * 和 arcade_video_draw_line() 的区别：那条路是 Peanut-GB 的 DMG 灰度
 * （每像素 2 bit 写进 4 档绿盘），这里已经把调色板算好了（CGB 是游戏自己
 * 的 15bit 色），所以只需要最近邻放大 + RGB565 打包。
 */
void arcade_video_draw_frame_rgba(const uint32_t *rgba)
{
    static uint16_t s_xmap[ARCADE_FB_WIDTH];
    static uint8_t s_ymap[ARCADE_FB_HEIGHT];
    static bool s_map_ready;

    if (s_fb == NULL || rgba == NULL) {
        return;
    }

    if (!s_map_ready) {
        for (int x = 0; x < ARCADE_FB_WIDTH; x++) {
            s_xmap[x] = (uint16_t)((x * SCALE_DEN) / SCALE_NUM);
        }
        for (int y = 0; y < ARCADE_FB_HEIGHT; y++) {
            s_ymap[y] = (uint8_t)((y * SCALE_DEN) / SCALE_NUM);
        }
        s_map_ready = true;
    }

    for (int y = 0; y < ARCADE_FB_HEIGHT; y++) {
        const uint32_t *src = rgba + (size_t)s_ymap[y] * SRC_WIDTH;
        lv_color_t *row = s_fb + (size_t)y * ARCADE_FB_WIDTH;

        for (int x = 0; x < ARCADE_FB_WIDTH; x++) {
            uint32_t c = src[s_xmap[x]];

            row[x] = lv_color_make((uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16));
        }
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
