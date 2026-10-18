/*
 * 对讲机应用层（会话状态 / 按键语义 / 控制事件）
 *
 * 与 BLE 链路层（ic_link）和界面（ic_ui）解耦：链路只搬字节，界面只画像素。
 */
#ifndef __IC_APP_H__
#define __IC_APP_H__

#include <stdint.h>
#include <stdbool.h>

#include "lisa_btn.h"

/* 初始化：注册 GATT 服务、界面与协议回调。必须在 BLE 栈就绪、LVGL 就绪之后调用。 */
int ic_app_init(void);

/* 链路事件（由 ic_link 调用） */
void ic_app_on_link_up(void);
void ic_app_on_link_down(void);

/* 协议层回调 */
void ic_app_on_text(uint8_t role, const char *utf8, uint16_t len, bool truncated);
void ic_app_on_control(const char *json, uint16_t len);

/* 按键原始边沿（service_button.c 的驱动回调分发进来；需要按下/松开，不走消息总线） */
void ic_app_on_raw_button(lisa_btn_event_t event, uint8_t btn_id);

/* 点击序列拦截：对讲页在前台时返回 true，表示该动作已被消费 */
bool ic_app_intercept_action(int action);

#endif /* __IC_APP_H__ */
