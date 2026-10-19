/*
 * 复古模拟器统一核心接口。
 *
 * 目前只实现了 GB/GBC 后端（Peanut-GB）。NES 后端会以同样的
 * arcade_core_* 接口接入，应用层不需要知道具体是哪种机器。
 */
#ifndef __ARCADE_CORE_H__
#define __ARCADE_CORE_H__

#include <stdint.h>
#include <stdbool.h>

#include "lvgl.h"

/* 输出画布尺寸：GB 是 160x144，按 1.5 倍最近邻放大到 240x216 */
#define ARCADE_FB_WIDTH   240
#define ARCADE_FB_HEIGHT  216

/* GB 按键位，和 Peanut-GB 的 JOYPAD_* 保持一致 */
#define ARCADE_KEY_A       0x01
#define ARCADE_KEY_B       0x02
#define ARCADE_KEY_SELECT  0x04
#define ARCADE_KEY_START   0x08
#define ARCADE_KEY_RIGHT   0x10
#define ARCADE_KEY_LEFT    0x20
#define ARCADE_KEY_UP      0x40
#define ARCADE_KEY_DOWN    0x80

typedef enum {
    ARCADE_SYS_GB = 0,
    ARCADE_SYS_NES,
    ARCADE_SYS_MAX,
} arcade_sys_t;

/**
 * 挂载并扫描 ROM 目录，返回可用 ROM 数量。
 */
int arcade_rom_scan(void);

/** 已扫描到的 ROM 数量 */
int arcade_rom_count(void);

/** 第 index 个 ROM 的显示名（basename） */
const char *arcade_rom_name(int index);

/** 第 index 个 ROM 的完整路径 */
const char *arcade_rom_path(int index);

/** 第 index 个 ROM 的字节数 */
uint32_t arcade_rom_size(int index);

/** ROM 列表是否已经扫描过 */
bool arcade_rom_scanned(void);

/** 让目录缓存失效，下次 arcade_rom_scan() 重新读卡 */
void arcade_rom_invalidate(void);

/**
 * 根据 ROM 文件名后缀猜测机器类型。
 */
arcade_sys_t arcade_core_guess_sys(const char *path);

/**
 * 加载 ROM 并完成模拟器初始化。
 * @return 0 成功，负值失败
 */
int arcade_core_start(const char *path);

/** 释放当前 ROM 与模拟器上下文 */
void arcade_core_stop(void);

/** 是否已有 ROM 在跑 */
bool arcade_core_running(void);

/**
 * 执行一帧（CPU + LCD 扫描线回调）。
 * 扫描线回调会把像素写进 arcade_video_get_fb() 返回的缓冲区。
 */
void arcade_core_run_frame(void);

/** 设置当前按下的按键位图（ARCADE_KEY_*） */
void arcade_core_set_input(uint32_t mask);

/** 当前机器类型的名字，用于 UI 展示 */
const char *arcade_core_sys_name(void);

#endif /* __ARCADE_CORE_H__ */
