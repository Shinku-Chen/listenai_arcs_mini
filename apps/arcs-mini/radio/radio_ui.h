/*
 * 电台界面：独立 LVGL screen（240x240），导航 id 92。
 *
 * 硬约束（沿用 intercom 与 docs/firmware/conventions.md 的结论）：
 *  - 不改 apps-ui 的助手页/配网页；本 screen 只通过导航管理器进/出；
 *  - LVGL 非线程安全：所有状态由 radio_app 持有，本文件只在 LVGL 定时器里
 *    读一份快照并刷新控件；外部只允许调用 radio_ui_show/hide（必须投递到 UI 线程）。
 *
 * 视觉对齐 leo-radio 的 build_main（LEO RADIO 标题 / CH 编号 / 刻度盘 / 电平条 /
 * 状态行），但画布只有 240x240，所以整体压缩并去掉原版的时钟秒数与日期。
 */
#ifndef __RADIO_UI_H__
#define __RADIO_UI_H__

#include <stdbool.h>
#include <stdint.h>

/* 注册 nav screen（必须在 lisa_ui 就绪后调用） */
int radio_ui_init(void);

/* 以下两个函数必须在 LVGL/UI 线程里调用（内部只做显隐与导航） */
void radio_ui_show(void);
void radio_ui_hide(void);

/* 当前置顶页面 id（用于“是否被别的页面占屏”判断） */
int radio_ui_top_id(void);

#endif /* __RADIO_UI_H__ */
