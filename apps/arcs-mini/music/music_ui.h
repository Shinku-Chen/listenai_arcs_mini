/*
 * 网易云音乐界面（240x240，导航 id 93）。
 *
 * 与 radio_ui 同一套约定：只通过 music_app_get_model() 读快照，
 * 显隐/导航必须在 UI 线程里做（外部用 music_ui_show/hide）。
 */
#ifndef __MUSIC_UI_H__
#define __MUSIC_UI_H__

/* 注册 nav screen（必须在 lisa_ui / LVGL 就绪后调用） */
int music_ui_init(void);

/* 本 screen 的导航 id（按键分发用） */
int music_ui_screen_id(void);

/* 当前置顶页面 id（判断自己是否在栈顶） */
int music_ui_top_id(void);

/* 进 / 出音乐页（内部投递到 UI 线程由调用方负责） */
void music_ui_show(void);
void music_ui_hide(void);

#endif /* __MUSIC_UI_H__ */
