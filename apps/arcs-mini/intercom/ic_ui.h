/*
 * 对讲机界面：独立 LVGL screen（240x240）
 *
 * 硬约束（沿用 radio_ui 的结论）：
 *  - 不修改 apps-ui 的助手页与扫码配网；本 screen 通过导航管理器进/出；
 *  - LVGL 非线程安全：所有对外接口都从任意任务调用，内部一律 lv_async_call 投递，
 *    数据在堆上传递，LVGL 线程执行完再释放。
 */
#ifndef __IC_UI_H__
#define __IC_UI_H__

#include <stdint.h>
#include <stdbool.h>

/* 轮次指示：0=空闲 1=已按下准备中 2=可以说话了 */
enum { IC_TURN_IDLE = 0, IC_TURN_PREPARE = 1, IC_TURN_READY = 2 };

/* 注册 nav screen（必须在 LVGL / lisa_ui 就绪后调用） */
int ic_ui_init(void);

void ic_ui_show(void);
void ic_ui_hide(void);

/* 追加一条对话气泡（role 见 ic_proto.h 的 IC_ROLE_*） */
void ic_ui_add_text(uint8_t role, const char *utf8, uint16_t len);

void ic_ui_clear(void);
void ic_ui_set_link(bool connected, bool ready);
void ic_ui_set_gateway(const char *state, const char *detail);
void ic_ui_set_turn(uint8_t state);
void ic_ui_set_battery(uint8_t pct);

#endif /* __IC_UI_H__ */
