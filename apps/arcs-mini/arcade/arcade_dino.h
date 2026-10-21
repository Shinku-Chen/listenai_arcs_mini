#ifndef __ARCADE_DINO_H__
#define __ARCADE_DINO_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * 内置小游戏：断网小恐龙（Chrome / Edge 那只）。
 *
 * 完全跑在 240x240 的 RGB565 帧缓冲上，不经过模拟器和 LVGL：
 * 每帧先算逻辑（固定 60Hz 步长），再从 arcade_video 的全屏缓冲池里取一块
 * 自己画、画完提交送屏。取不到空缓冲就跳过这一帧的绘制（面板保留上一帧），
 * 逻辑一帧不落 —— 和模拟器的跳帧保速是同一套哲学。
 */

typedef enum {
    ARCADE_DINO_RUN = 0,   /* 奔跑中 */
    ARCADE_DINO_DEAD,      /* 撞了，等重开 */
} arcade_dino_state_t;

/** 进入游戏：重置局面并开始跑（调用前由上层切成全屏直写模式） */
void arcade_dino_enter(void);

/** 退出游戏：只清状态，屏幕/模式由上层切回 */
void arcade_dino_leave(void);

/** 当前是否在游戏中 */
bool arcade_dino_active(void);

/** 主循环每帧调用一次：逻辑 + 尝试渲染（不阻塞） */
void arcade_dino_tick(void);

/** 一次「跳跃」输入：地面起跳 / 空中加速下落 / 死亡后重开 */
void arcade_dino_click(void);

/** 自动跳（障碍接近就跳），无人值守压测用 */
void arcade_dino_set_auto(bool on);
bool arcade_dino_auto(void);

/** 重开一局 */
void arcade_dino_reset(void);

/* ---- 调试读取（shell 任务直接读，都是对齐的 32 位量） ---- */
arcade_dino_state_t arcade_dino_state(void);
uint32_t arcade_dino_score(void);
uint32_t arcade_dino_best(void);
int arcade_dino_height(void);          /* 恐龙离地高度（px） */
bool arcade_dino_airborne(void);
int arcade_dino_obstacle_count(void);
int arcade_dino_obstacle_x(int index); /* 障碍左边缘（屏幕 px），越界返回 -1 */
int arcade_dino_speed_px(void);        /* 当前速度（px/s） */
uint32_t arcade_dino_logic_fps_x10(void);
uint32_t arcade_dino_render_fps_x10(void);
uint32_t arcade_dino_frames(void);
uint32_t arcade_dino_renders(void);
uint32_t arcade_dino_skips(void);

#endif /* __ARCADE_DINO_H__ */
