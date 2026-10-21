#ifndef __ARCADE_GFX_H__
#define __ARCADE_GFX_H__

#include <stdint.h>

/**
 * 内置小游戏共用的绘制原语（arcade_dino.c / arcade_jump.c）。
 *
 * 目标缓冲固定是 240x240 的 RGB565（arcade_video 全屏直写模式），
 * 全部整数运算：RV32 没有 FPU，float 会变成软件浮点，一帧几百次调用就废了。
 */

#define ARCADE_GFX_W 240
#define ARCADE_GFX_H 240

/** RGB565 常量（r/g/b 各 8 位） */
#define ARCADE_GFX_RGB565(r, g, b)                                          \
    ((uint16_t)((((r) * 31 / 255) << 11) | (((g) * 63 / 255) << 5) |        \
                ((b) * 31 / 255)))

/** 填充矩形（自动裁到屏幕内；32 位对齐后成对写，一次写两个像素） */
void arcade_gfx_fill(uint16_t *fb, int x, int y, int w, int h, uint16_t color);

/** 画一张 '#'/其它 的点阵图（每行以 '\0' 结尾，scale = 最近邻放大倍数） */
void arcade_gfx_sprite(uint16_t *fb, int x, int y, const char *const *rows,
                       int nrows, int scale, uint16_t color);

/** 画一个 5x7 点阵字符（只支持 0-9 A-Z a-z - :） */
void arcade_gfx_char(uint16_t *fb, int x, int y, char ch, int scale, uint16_t color);

/** 画一串 5x7 点阵字符 */
void arcade_gfx_text(uint16_t *fb, int x, int y, const char *s, int scale,
                     uint16_t color);

/** 字符串宽度（px），用来做右对齐 / 居中 */
int arcade_gfx_text_width(const char *s, int scale);

/** 水平居中画一串字符 */
void arcade_gfx_text_center(uint16_t *fb, int y, const char *s, int scale,
                            uint16_t color);

#endif /* __ARCADE_GFX_H__ */
