#ifndef SERVICE_BUTTON_H
#define SERVICE_BUTTON_H

#include <stdint.h>

int service_button_init(void);

/**
 * 原始按键事件回调（按下 / 松开 / 长按……），给「按住蓄力」这类对延迟
 * 敏感、又不能等多击判定窗口（300ms）的界面用（例如街机固件的跳一跳）。
 *
 * 注意：
 *   - 回调在按键扫描任务上下文里触发（周期 20ms），实现方只做落标志之类的
 *     轻量动作，不要在里面画屏 / 加锁 / 打长日志；
 *   - event 是 voice_msg_button_action_t（PRESS_DOWN / SHORT_UP / LONG_UP …），
 *     用 uint8_t 传只是为了避免这个头文件多拉一份包含；
 *   - 只转发 GPIO 上的物理按键，USB 鼠标之类的合成点击不会进来。
 */
typedef void (*service_button_raw_cb_t)(uint8_t btn_id, uint8_t event, void *user);

/** 注册/注销原始事件回调（cb = NULL 表示注销） */
void service_button_set_raw_callback(service_button_raw_cb_t cb, void *user);

#endif
