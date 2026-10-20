#ifndef __ARCADE_VIDEO_H__
#define __ARCADE_VIDEO_H__

#include <stdint.h>

#include "lvgl.h"

#include "arcade_core.h"
#include "arcade_fb.h"

/** 模拟器帧缓冲的像素类型：RGB565(u16) 或 RGBA(u32)，由 ARCADE_FB_RGB565 决定 */
#if ARCADE_FB_RGB565
typedef uint16_t arcade_fbpixel_t;
#else
typedef uint32_t arcade_fbpixel_t;
#endif

/**
 * 申请两块仿真帧缓冲（240x216 RGB565，PSRAM）并准备送屏。
 * 画面不再经过 LVGL：改由独立送屏任务直接写面板，和模拟器并行。
 */
int arcade_video_init(void);

void arcade_video_deinit(void);

/** 最近一帧送屏的缓冲首地址（调试用，长度 = fb_width()*fb_height()） */
lv_color_t *arcade_video_get_fb(void);

/** 当前缩放下缓冲的有效宽高（1x = 160x144，1.5x = 240x216） */
int arcade_video_fb_width(void);
int arcade_video_fb_height(void);

/**
 * Peanut-GB 的逐行回调：把 160 像素的一行按 1.5 倍最近邻放大成 1~2 行写入
 * 帧缓冲。
 * @param src_line 源行号 0..143
 * @param shades   160 个像素，低 2 位是灰度档
 */
void arcade_video_draw_line(int src_line, const uint8_t *shades);

/** 把整块帧缓冲刷成黑 */
void arcade_video_fill_black(void);

/**
 * 调试用：把帧缓冲填成调色板第 shade 档（0 最亮 / 3 最暗）。
 * 用来区分「帧缓冲数据错」和「没刷到屏上」。
 */
void arcade_video_fill(int shade);

/** 调色板第 shade 档的 RGB565 原始值（调试用） */
uint16_t arcade_video_palette_raw(int shade);

/** 4 档 DMG 绿盘原始 RGB（模拟器内核要给 binjgb 设 DMG 调色板时用） */
const uint8_t (*arcade_video_dmg_palette_rgb(void))[3];

/**
 * binjgb 帧输出：160x144 的帧缓冲（RGB565 或 RGBA，见 arcade_fb.h）缩放后写进
 * 空闲缓冲，交给送屏任务；和上一帧的 SPI 传输并行。每帧调一次。
 */
void arcade_video_present_frame(const arcade_fbpixel_t *src);

/** 把当前缓冲内容立即送屏一次（`arcade fb fill` 自检用） */
void arcade_video_present_now(void);

/** 等在途的帧全部送完（切回 LVGL 页面前调用，避免被游戏帧盖掉） */
void arcade_video_present_drain(void);

/** 缩放：1 = 原尺寸 160x144 居中；2 = 1.5 倍 240x216（默认） */
void arcade_video_set_scale(int scale);
int arcade_video_scale(void);

/**
 * 调试：180° 硬件翻转的行偏移覆盖（像素），-1 = 自动推算。
 * 改完下一帧就生效（不改 MADCTL，只改窗口起始行）。
 */
void arcade_video_set_rotate_row_shift(int16_t shift);
int16_t arcade_video_rotate_row_shift(void);

void arcade_video_reset_perf(void);
void arcade_video_perf(char *out, uint32_t len);

#endif /* __ARCADE_VIDEO_H__ */
