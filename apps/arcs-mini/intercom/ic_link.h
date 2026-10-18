/*
 * 对讲机 BLE 链路：Nordic UART Service（128-bit UUID）+ 发送环
 *
 * 契约见 docs/development/intercom-wire-protocol.md。本层只负责搬字节：
 *  - 注册 NUS 服务（service 0x6e400001 / RX 0x6e400002 / TX 0x6e400003）；
 *  - 收到的写入原样喂给协议层（分帧在 ic_proto）；
 *  - 发送走有界环 + 独立任务，采集/业务任务永不因射频背压阻塞。
 *
 * 平台差异（相对 ESP32/NimBLE 的参考实现）：
 *  - 本仓 BLE 协议栈是 CP 核上的 RW 栈，AP 侧通过 IPC 调用；GATT 服务用
 *    ble_gatt_db_svc_add() 注册属性表，notify 用 ble_gatt_srv_event_send()。
 *  - 连接/断开/配对回调由 src/middleware/ble/app_ble_common.c 统一分发到这里
 *    （SDK 的回调注册是单槽位，直接再注册会顶掉 BLE 配网）。
 */
#ifndef __IC_LINK_H__
#define __IC_LINK_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* NUS 单次 notify 上限（MTU 247 - 3），发送环的槽也必须按它切 */
#define IC_LINK_NOTIFY_MAX 244U
/* 单帧上限：协议头 6 + payload 上限；发送环按整帧存放 */
#define IC_LINK_FRAME_MAX (6U + 2048U)
/* 发送槽上限：设备只发 EVENT/CONTROL（JSON），544 足够；超长帧直接拒绝 */
#define IC_LINK_TX_SLOT_MAX 544U
/* 发送环深度：够吸收一次 UI 突发，满了丢最旧的并在统计里体现 */
#define IC_LINK_TX_SLOTS 8U

#define IC_LINK_SERVICE_UUID                                                      \
    {                                                                             \
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
            0x01, 0x00, 0x40, 0x6e                                                     \
    }
#define IC_LINK_RX_UUID                                                           \
    {                                                                             \
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
            0x02, 0x00, 0x40, 0x6e                                                     \
    }
#define IC_LINK_TX_UUID                                                           \
    {                                                                             \
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,   \
            0x03, 0x00, 0x40, 0x6e                                                     \
    }

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_dropped;   /* 环满 / 未订阅 / 射频拒绝 */
    uint32_t rx_bytes;
    uint32_t connects;
    uint32_t disconnects;
} ic_link_stat_t;

/* 注册 GATT 服务并起发送任务。必须在 BLE 协议栈就绪后调用。 */
int ic_link_init(void);

/* 发一帧（type/flags/payload 见 ic_proto）。返回 0 = 已入队。 */
int ic_link_send(uint8_t type, uint8_t flags, const void *payload, uint16_t len);

bool ic_link_connected(void);
bool ic_link_notify_enabled(void);

const ic_link_stat_t *ic_link_stat(void);

/* 由 app_ble_common.c 的连接/断开/配对回调分发进来 */
void ic_link_on_connected(uint8_t conidx, const void *peer_addr);
void ic_link_on_disconnected(uint8_t conidx, uint16_t reason);
void ic_link_on_bond(uint8_t conidx, uint8_t info, uint8_t value);

#endif /* __IC_LINK_H__ */
