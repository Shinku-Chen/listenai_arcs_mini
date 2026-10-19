#ifndef __ARCADE_INPUT_H__
#define __ARCADE_INPUT_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * 读取当前按键位图（ARCADE_KEY_*）。
 *
 * USB 键盘注入的“点按”会在 ARCADE_INPUT_PULSE_MS 后自动释放，
 * 这样游戏才能看到一次完整的按下/松开。
 */
uint32_t arcade_input_mask(void);

/** 常按状态（按住不放），用于键盘等有真实按下/抬起事件的输入源 */
void arcade_input_hold(uint32_t keys, bool pressed);

/**
 * 注入一次点按：立刻按下，ARCADE_INPUT_PULSE_MS 后自动释放。
 * 物理按键和 adb shell 调试都走这里。
 */
void arcade_input_tap(uint32_t keys);

/** 释放所有按键 */
void arcade_input_release_all(void);

/** 名字到按键位，返回 0 表示未知 */
uint32_t arcade_input_key_from_name(const char *name);

/** 按键位到名字，用于调试输出 */
const char *arcade_input_name(uint32_t key);

/**
 * 取走 USB 键盘的最新状态（由 HID host 报告回调写入）。
 * @param exit_request 非 NULL 时返回是否按下了 ESC（请求退出游戏）
 */
uint32_t arcade_input_take_keyboard(bool *exit_request);

/**
 * USB 手柄（HID 非 boot 设备）的原始报告快照，给 `arcade pad` 调试用。
 * 手柄没有固定报告布局，只能靠看原始字节确定按键位序。
 *
 * @param raw      输出缓冲区（NULL 表示只要计数）
 * @param raw_max  缓冲区长度
 * @param raw_len  最近一份报告的实际长度
 * @param is_xinput 是否识别为 XInput 布局（20 字节，头部 00 14）
 * @return 收到的报告总数
 */
uint32_t arcade_input_pad_info(uint8_t *raw, int raw_max, int *raw_len, bool *is_xinput);

#endif /* __ARCADE_INPUT_H__ */
