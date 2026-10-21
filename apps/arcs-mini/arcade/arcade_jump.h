#ifndef __ARCADE_JUMP_H__
#define __ARCADE_JUMP_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * 内置小游戏：跳一跳（微信那个）。
 *
 * 和小恐龙一样完全跑在 240x240 的 RGB565 帧缓冲上，不经过模拟器和 LVGL：
 * 每帧先算逻辑（固定 60Hz 步长），再从 arcade_video 的全屏缓冲池里取一块
 * 自己画、画完提交送屏。取不到空缓冲就跳过这一帧的绘制（面板保留上一帧），
 * 逻辑一帧不落。
 *
 * 按键语义（用的是按键驱动的**原始事件**，不走多击判定窗口，否则单击要等
 * 300ms 才能确定，蓄力根本没法玩）：
 *   - 按下（PRESS_DOWN）      → 开始蓄力
 *   - 松开（SHORT_UP）        → 起跳，蓄力越久跳越远
 *   - 长按（LONG_HOLD）       → 由上层退出回列表
 *   - GAME OVER 后按一下       → 重开
 */

typedef enum {
    ARCADE_JUMP_READY = 0,   /* 站在平台上，可以蓄力 */
    ARCADE_JUMP_CHARGE,      /* 按住蓄力中 */
    ARCADE_JUMP_FLY,         /* 抛物线飞行 */
    ARCADE_JUMP_PAN,         /* 落地了，相机跟随到新平台 */
    ARCADE_JUMP_FALL,        /* 踩空，正在掉下去 */
    ARCADE_JUMP_DEAD,        /* GAME OVER，等重开 */
} arcade_jump_state_t;

/** 进入游戏：重置局面并开始（调用前由上层切成全屏直写模式） */
void arcade_jump_enter(void);

/** 退出游戏：只清状态，屏幕/模式由上层切回 */
void arcade_jump_leave(void);

/** 当前是否在游戏中 */
bool arcade_jump_active(void);

/** 主循环每帧调用一次：逻辑 + 尝试渲染（不阻塞） */
void arcade_jump_tick(void);

/** 按下：开始蓄力（READY）/ 死亡后重开 */
void arcade_jump_press(void);

/** 松开：起跳（只有蓄力中才有效） */
void arcade_jump_release(void);

/**
 * 调试/自动玩：按下并蓄力 ms 毫秒后自动松手。
 * 不需要真按键就能把整套判定跑一遍（`arcade jump charge <ms>`）。
 */
void arcade_jump_charge(int ms);

/** 自动玩（按平台距离反算蓄力时长 + 一点随机误差），无人值守压测用 */
void arcade_jump_set_auto(bool on);
bool arcade_jump_auto(void);

/** 重开一局 */
void arcade_jump_reset(void);

/* ---- 调试读取（shell 任务直接读，都是对齐的 32 位量） ---- */
arcade_jump_state_t arcade_jump_state(void);
uint32_t arcade_jump_score(void);
uint32_t arcade_jump_best(void);
uint32_t arcade_jump_combo(void);
int arcade_jump_charge_pct(void);     /* 当前蓄力（0-100） */
int arcade_jump_gap(void);            /* 当前平台到下一平台的距离（沿轴 px） */
int arcade_jump_radius(void);         /* 当前平台半宽（px） */
int arcade_jump_next_radius(void);    /* 下一平台半宽（px） */
int arcade_jump_dir(void);            /* +1 = 右上，-1 = 左上 */
int arcade_jump_land_err(void);       /* 上一次落点偏差（px，正 = 跳过头） */
uint32_t arcade_jump_logic_fps_x10(void);
uint32_t arcade_jump_render_fps_x10(void);
uint32_t arcade_jump_frames(void);
uint32_t arcade_jump_renders(void);
uint32_t arcade_jump_skips(void);

#endif /* __ARCADE_JUMP_H__ */
