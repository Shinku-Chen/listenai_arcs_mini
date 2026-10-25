#ifndef __ARCADE_EAT_H__
#define __ARCADE_EAT_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * 内置小游戏：「今天吃啥」老虎机。
 *
 * 素材是两套索引色帧序列（去哪个店吃 / 吃什么菜），**按源尺寸 200x200 1:1
 * 居中贴到 240x240**（四周各 20 px 黑边），固件里不做任何缩放 —— 非整数倍
 * 放大是上一版「图片发虚」的直接原因。
 *
 * 交互（按键用**原始事件**，不走多击判定窗口，否则按住/松手都要等几百毫秒）：
 *   - 按住（PRESS_DOWN） → 立刻从随机一帧开始滚动「本次该抽的那一组」
 *   - 松手（SHORT_UP / LONG_UP） → 立刻定格，屏上那帧就是结果
 *   - 再按一次           → 抽另一组
 *   - 两组**严格交替**：开机第一次是 g1（去处盘 PLACE），第二次 g2（菜品盘
 *     DISH），第三次又回 g1……重启后重新从 g1 开始。组内起始帧仍然随机。
 *   - 长按**不会退出**："今天吃啥"单应用固件里没有任何长按退出/关机路径
 *     （CONFIG_ARCADE_APP_EATONLY=y，见 eat_logic() 里的编译开关）
 */

typedef enum {
    ARCADE_EAT_IDLE = 0,   /* 待机，屏上是首帧海报 + 「HOLD KEY」提示条 */
    ARCADE_EAT_ROLLING,    /* 按住中，图片快速循环 */
    ARCADE_EAT_RESULT,     /* 松手定格，屏上就是结果 */
} arcade_eat_state_t;

/** 每次抽取记录的大小上限（`arcade eat auto` 打印分布用） */
#define ARCADE_EAT_LOG_MAX 64

/** 每抽一次的结果（type: 0 = 去处盘, 1 = 菜品盘；frame: 该类型的帧序号） */
typedef struct {
    uint8_t type;
    uint8_t frame;
} arcade_eat_pick_t;

/** 进入：重置局面并开始（调用前由上层切成全屏直写模式） */
void arcade_eat_enter(void);

/** 退出：只清状态，屏幕/模式由上层切回 */
void arcade_eat_leave(void);

/** 当前是否在「今天吃啥」里 */
bool arcade_eat_active(void);

/** 主循环每帧调用一次：逻辑 + 尝试渲染（不阻塞） */
void arcade_eat_tick(void);

/** 按下：开始滚动（IDLE / RESULT 都有效） */
void arcade_eat_press(void);

/** 松开：定格（只有滚动中才有效） */
void arcade_eat_release(void);

/**
 * 调试/自动抽：按下并保持 ms 毫秒后自动松手。
 * 不需要真按键就能把整套「抽类型 → 滚 → 定格」跑一遍（`arcade eat hold <ms>`）。
 */
void arcade_eat_hold(int ms);

/**
 * 自动连抽 n 次（每次按住时长随机），用来统计类型分布。
 * 抽完 n 次后自动停在 IDLE，`arcade_eat_auto_done()` 变 true。
 */
void arcade_eat_auto(int n);

bool arcade_eat_auto_pending(void);
int arcade_eat_auto_done(void);

/** 回到待机（清掉上一次的结果画面） */
void arcade_eat_reset(void);

/**
 * 按住超过 3 秒还没松手 → 返回 true（读一次就清掉）。
 * 用自己数的时间而不是按键驱动的 LONG_HOLD（那个 1600ms 就报，会和
 * 「按住滚动」撞车）。全功能固件里上层拿到 true 就退出回列表；
 * 单应用固件（CONFIG_ARCADE_APP_EATONLY=y）里这个逻辑整段关掉，长按永远不退出。
 */
bool arcade_eat_exit_requested(void);

/* ---- 调试读取（shell 任务直接读，都是对齐的 32 位量） ---- */
arcade_eat_state_t arcade_eat_state(void);
int arcade_eat_type(void);                   /* 0 = 去处盘, 1 = 菜品盘 */
int arcade_eat_frame(void);                  /* 当前帧序号 */
int arcade_eat_frame_count(int type);        /* 该类型的总帧数 */
int arcade_eat_hold_ms_cur(void);            /* 本次按住了多久 */
uint32_t arcade_eat_draws(void);             /* 进入后一共抽了多少次 */
uint32_t arcade_eat_hist(int type);          /* 该类型被抽中多少次 */

/** 下次按下会抽哪一组（0 = 去处盘, 1 = 菜品盘），待机画面据此提示 */
int arcade_eat_next_type(void);

/** 第 i 次抽取的结果（i 从 0 开始，只保留最近 ARCADE_EAT_LOG_MAX 次） */
bool arcade_eat_log(uint32_t i, arcade_eat_pick_t *out);

uint32_t arcade_eat_logic_fps_x10(void);
uint32_t arcade_eat_render_fps_x10(void);
uint32_t arcade_eat_frames(void);
uint32_t arcade_eat_renders(void);
uint32_t arcade_eat_skips(void);

#endif /* __ARCADE_EAT_H__ */
