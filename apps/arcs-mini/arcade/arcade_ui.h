#ifndef __ARCADE_UI_H__
#define __ARCADE_UI_H__

#include <stdint.h>
#include <stdbool.h>

#include "lvgl.h"

/**
 * 初始化 LVGL（显示驱动 + 两个页面）。
 * 不依赖 apps-ui，只用到 SDK 里的 lv_port_disp 移植层。
 */
int arcade_ui_init(void);

/** 显示 ROM 列表页 */
void arcade_ui_show_list(void);

/** 显示游戏画面页 */
void arcade_ui_show_player(void);

/**
 * 刷新列表页内容。
 * @param selected 当前选中项下标
 */
void arcade_ui_list_refresh(int selected);

/** 列表页底部的提示文字（加载失败等信息也走这里） */
void arcade_ui_list_set_hint(const char *text);

/** 游戏页重绘（每帧调用） */
void arcade_ui_player_invalidate(void);

/**
 * 在游戏页中间显示一行提示，传 NULL 或空串则隐藏。
 */
void arcade_ui_player_message(const char *text);

/** 返回 LVGL 建议的下一次 lv_timer_handler() 间隔（毫秒） */
uint32_t arcade_ui_handler(void);

/**
 * 调试用：把播放页 canvas 的挂载状态写进 buf。
 * 必须在 UI 任务里调用（会读 LVGL 对象）。
 */
void arcade_ui_player_debug(char *buf, uint32_t len);

#endif /* __ARCADE_UI_H__ */
