#ifndef __ARCADE_VIDEO_H__
#define __ARCADE_VIDEO_H__

#include <stdint.h>

#include "lvgl.h"

#include "arcade_core.h"

/**
 * 申请仿真帧缓冲（240x216，LVGL true-color）。
 * 缓冲区放在 PSRAM，由 canvas 直接引用，不需要额外拷贝。
 */
int arcade_video_init(void);

void arcade_video_deinit(void);

/** 返回帧缓冲首地址（lv_color_t 数组，长度 240*216） */
lv_color_t *arcade_video_get_fb(void);

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
 * 调试用：把整块帧缓冲填成调色板第 shade 档（0 最亮 / 3 最暗）。
 * 用来区分「帧缓冲数据错」和「没刷到屏上」。
 */
void arcade_video_fill(int shade);

/** 调色板第 shade 档的 RGB565 原始值（调试用） */
uint16_t arcade_video_palette_raw(int shade);

/** 4 档 DMG 绿盘原始 RGB（模拟器内核要给 binjgb 设 DMG 调色板时用） */
const uint8_t (*arcade_video_dmg_palette_rgb(void))[3];

/**
 * binjgb 帧输出：160x144 的 RGBA（0xAABBGGRR）按 1.5 倍最近邻放大到
 * 240x216 帧缓冲。每帧调一次。
 */
void arcade_video_draw_frame_rgba(const uint32_t *rgba);

#endif /* __ARCADE_VIDEO_H__ */
