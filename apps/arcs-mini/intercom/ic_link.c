/*
 * 对讲机 BLE 链路实现（RW 协议栈 / lisa_bluetooth）
 *
 * 设计要点（与参考实现 oc_link.c 对齐的部分和差异）：
 *  - 对齐：只有"已连接 + 已订阅"才允许发送；发送走环 + 独立任务，采集/业务任务
 *    不因射频背压阻塞；RX 只喂字节流，分帧交给 ic_proto。
 *  - 差异：本仓 BLE 栈跑在 CP 核，AP 侧是 IPC；连接/断开/配对回调由
 *    src/middleware/ble/app_ble_common.c 统一分发（SDK 回调是单槽位，
 *    直接再注册会顶掉 BLE 配网）。
 *  - 差异：广播名保持固件既有的 "ARCS"（BLE 配网小程序按 mfg data 识别），
 *    对讲机身份放在扫描响应里的 128-bit 服务 UUID + "LING-xxxx" 名字，
 *    安卓按 serviceUuid 过滤即可命中。
 */
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "sysutils.h"

#include "ble_gatt.h"
#include "ble_prf.h"
#include "ble_gap.h"
#include "lisa_bluetooth.h"
#include "lisa_ble_api.h"
#include "ble_adv_data.h"

#include "ic_app.h"
#include "ic_link.h"
#include "ic_proto.h"

#define TAG "ic.link"
#include "lisa_log.h"

/* GATT 属性表下标（hdl - start_hdl） */
enum {
    IC_IDX_SVC = 0,
    IC_IDX_RX_CHAR,
    IC_IDX_RX_VAL,
    IC_IDX_TX_CHAR,
    IC_IDX_TX_VAL,
    IC_IDX_TX_CFG,
    IC_IDX_NB,
};

/* 单帧 payload 的 ATT 上限：手机侧长文本一帧最大 2048 */
#define IC_RX_ATT_MAX 2048U
#define IC_PREF_MTU 247U
#define IC_TX_TASK_STACK 3072U
#define IC_TX_TASK_PRIO 5U
#define IC_EVT_SENT_TIMEOUT_MS 300U

static const uint8_t k_svc_uuid[BLE_GATT_UUID_128_LEN] = IC_LINK_SERVICE_UUID;
static const uint8_t k_rx_uuid[BLE_GATT_UUID_128_LEN] = IC_LINK_RX_UUID;
static const uint8_t k_tx_uuid[BLE_GATT_UUID_128_LEN] = IC_LINK_TX_UUID;

/* 16-bit 声明类 UUID 在 128-bit 空间里的写法（0x2800 / 0x2803 / 0x2902） */
#define IC_DECL_UUID128(uuid16)                                                        \
    {                                                                                  \
        (uint8_t)((uuid16) & 0xFF), (uint8_t)(((uuid16) >> 8) & 0xFF), 0x00, 0x00,      \
            0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB       \
    }

typedef struct {
    uint16_t len;
    uint8_t data[IC_LINK_TX_SLOT_MAX];
} ic_tx_slot_t;

static struct {
    bool inited;
    uint8_t user_lid;
    uint16_t start_hdl;
    bool cccd_on;
    uint8_t conidx;
    bool connected;

    QueueHandle_t free_q;
    QueueHandle_t filled_q;
    SemaphoreHandle_t sent_sem;
    ic_tx_slot_t slots[IC_LINK_TX_SLOTS];

    ic_link_stat_t stat;
} s_link __psram_bss__;

static const uint8_t k_att_props[IC_IDX_NB] = {
    [IC_IDX_SVC] = BLE_PROP(RD),
    [IC_IDX_RX_CHAR] = BLE_PROP(RD),
    /* 手机 -> 设备：write with response + write without response */
    [IC_IDX_RX_VAL] = (uint8_t)(BLE_PROP(WR) | BLE_PROP(WC)),
    [IC_IDX_TX_CHAR] = BLE_PROP(RD),
    /* 设备 -> 手机：notify */
    [IC_IDX_TX_VAL] = (uint8_t)BLE_PROP(N),
    [IC_IDX_TX_CFG] = (uint8_t)(BLE_PROP(RD) | BLE_PROP(WR)),
};

/* 属性表（UUID 在 ic_link_init 里填：128-bit 特征 UUID 放不进 const 字面量列表） */
static ble_gatt_att_desc_t k_att_db[IC_IDX_NB];

static void build_att_db(void)
{
    static const uint8_t decl_primary[16] = IC_DECL_UUID128(BLE_GATT_DECL_PRIMARY_SERVICE);
    static const uint8_t decl_char[16] = IC_DECL_UUID128(BLE_GATT_DECL_CHARACTERISTIC);
    static const uint8_t decl_cccd[16] = IC_DECL_UUID128(BLE_GATT_DESC_CLIENT_CHAR_CFG);

    memset(k_att_db, 0, sizeof(k_att_db));
    memcpy(k_att_db[IC_IDX_SVC].uuid, decl_primary, 16);
    memcpy(k_att_db[IC_IDX_RX_CHAR].uuid, decl_char, 16);
    memcpy(k_att_db[IC_IDX_RX_VAL].uuid, k_rx_uuid, 16);
    memcpy(k_att_db[IC_IDX_TX_CHAR].uuid, decl_char, 16);
    memcpy(k_att_db[IC_IDX_TX_VAL].uuid, k_tx_uuid, 16);
    memcpy(k_att_db[IC_IDX_TX_CFG].uuid, decl_cccd, 16);

    for (uint32_t i = 0; i < IC_IDX_NB; i++) {
        k_att_db[i].info = k_att_props[i];
        k_att_db[i].ext_info = 0;
    }
    k_att_db[IC_IDX_RX_VAL].ext_info = IC_RX_ATT_MAX;
    k_att_db[IC_IDX_TX_VAL].ext_info = IC_LINK_NOTIFY_MAX;
    k_att_db[IC_IDX_TX_CFG].ext_info = BLE_OPT(NO_OFFSET);
}

static void send_task(void *arg);

static void tx_push(ic_tx_slot_t *slot)
{
    if (xQueueSend(s_link.filled_q, &slot, 0) != pdPASS) {
        /* 环满：丢最旧的，保住新帧（新帧是用户刚需要的） */
        ic_tx_slot_t *stale = NULL;
        if (xQueueReceive(s_link.filled_q, &stale, 0) == pdPASS && stale != NULL) {
            xQueueSend(s_link.free_q, &stale, 0);
        }
        s_link.stat.tx_dropped++;
        if (xQueueSend(s_link.filled_q, &slot, 0) != pdPASS) {
            xQueueSend(s_link.free_q, &slot, 0);
        }
    }
}

int ic_link_send(uint8_t type, uint8_t flags, const void *payload, uint16_t len)
{
    if (!s_link.inited) {
        return -1;
    }
    if (!s_link.connected || !s_link.cccd_on) {
        s_link.stat.tx_dropped++;
        return -2;
    }

    ic_tx_slot_t *slot = NULL;
    if (xQueueReceive(s_link.free_q, &slot, 0) != pdPASS || slot == NULL) {
        s_link.stat.tx_dropped++;
        return -3;
    }

    const size_t n = ic_proto_pack(slot->data, sizeof(slot->data), type, flags, payload, len);
    if (n == 0) {
        xQueueSend(s_link.free_q, &slot, 0);
        return -4;
    }
    slot->len = (uint16_t)n;
    tx_push(slot);
    return 0;
}

bool ic_link_connected(void)
{
    return s_link.connected;
}

bool ic_link_notify_enabled(void)
{
    return s_link.connected && s_link.cccd_on;
}

const ic_link_stat_t *ic_link_stat(void)
{
    return &s_link.stat;
}

/* ---------------- GATT 回调 ---------------- */

static void cb_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t dummy, uint16_t status)
{
    (void)conidx;
    (void)user_lid;
    (void)dummy;
    if (status == 0) {
        s_link.stat.tx_frames++;
    } else {
        s_link.stat.tx_dropped++;
    }
    if (s_link.sent_sem != NULL) {
        xSemaphoreGive(s_link.sent_sem);
    }
}

static void cb_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
                            uint16_t offset, uint16_t max_length)
{
    (void)offset;
    (void)max_length;
    const uint16_t idx = (uint16_t)(hdl - s_link.start_hdl);

    if (idx == IC_IDX_TX_CFG) {
        uint16_t v = s_link.cccd_on ? 0x0001 : 0x0000;
        ble_gatt_srv_att_read_get_cfm(conidx, user_lid, token, 0, sizeof(v), sizeof(v), (uint8_t *)&v);
        return;
    }
    ble_gatt_srv_att_read_get_cfm(conidx, user_lid, token, BLE_PRF_ERR_INVALID_PARAM, 0, 0, NULL);
}

static void cb_att_info_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl)
{
    const uint16_t idx = (uint16_t)(hdl - s_link.start_hdl);
    uint16_t len = 0;

    switch (idx) {
    case IC_IDX_RX_VAL:
        len = IC_RX_ATT_MAX;
        break;
    case IC_IDX_TX_VAL:
        len = IC_LINK_NOTIFY_MAX;
        break;
    case IC_IDX_TX_CFG:
        len = 2;
        break;
    default:
        break;
    }
    ble_gatt_srv_att_info_get_cfm(conidx, user_lid, token, 0, len);
}

static void cb_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
                           uint16_t offset, void *p_data)
{
    const uint16_t idx = (uint16_t)(hdl - s_link.start_hdl);
    const uint16_t len = ble_co_buf_data_len(p_data);
    const uint8_t *buf = ble_co_buf_data(p_data);

    if (idx == IC_IDX_RX_VAL) {
        if (len > 0 && buf != NULL) {
            s_link.stat.rx_bytes += len;
            /* 手机侧可能一帧拆成多次 ATT 写：直接喂字节流，重组在 ic_proto 里做 */
            ic_proto_rx(buf, len);
        }
    } else if (idx == IC_IDX_TX_CFG) {
        if (len >= 2) {
            const uint16_t cfg = ble_co_read16p(buf);
            const bool on = ((cfg & 0x0001U) != 0);
            if (on != s_link.cccd_on) {
                s_link.cccd_on = on;
                LISA_LOGI(TAG, "notify %s (conidx=%u)", on ? "on" : "off", conidx);
                if (on) {
                    /* 握手放在订阅之后：手机未开 notify 时任何帧都发不出去 */
                    ic_app_on_link_up();
                }
            }
        }
    }

    ble_gatt_srv_att_val_set_cfm(conidx, user_lid, token, 0);
    (void)offset;
}

static const ble_gatt_srv_cb_t k_gatt_cb = {
    .cb_event_sent = cb_event_sent,
    .cb_att_read_get = cb_att_read_get,
    .cb_att_event_get = NULL, /* 本服务不做长通知（单帧 ≤ MTU-3） */
    .cb_att_info_get = cb_att_info_get,
    .cb_att_val_set = cb_att_val_set,
};

static void send_task(void *arg)
{
    (void)arg;
    for (;;) {
        ic_tx_slot_t *slot = NULL;
        if (xQueueReceive(s_link.filled_q, &slot, portMAX_DELAY) != pdPASS || slot == NULL) {
            continue;
        }

        if (s_link.connected && s_link.cccd_on) {
            const uint16_t hdl = (uint16_t)(s_link.start_hdl + IC_IDX_TX_VAL);
            const uint16_t status = ble_gatt_srv_event_send(s_link.conidx, s_link.user_lid, 0,
                                                            BLE_GATT_NOTIFY, hdl, slot->data,
                                                            slot->len);
            if (status == 0) {
                /* 等 cb_event_sent：协议栈要求上一帧完成后才能发下一帧 */
                if (xSemaphoreTake(s_link.sent_sem, pdMS_TO_TICKS(IC_EVT_SENT_TIMEOUT_MS)) != pdTRUE) {
                    s_link.stat.tx_dropped++;
                }
            } else {
                s_link.stat.tx_dropped++;
            }
        } else {
            s_link.stat.tx_dropped++;
        }

        xQueueSend(s_link.free_q, &slot, 0);
    }
}

/* ---------------- 广播数据：扫描响应带 128-bit 服务 UUID + 名字 ---------------- */

static uint8_t s_scan_rsp[31];
static uint8_t s_scan_rsp_len;
static char s_name[16] = "LING";

static void build_scan_rsp(void)
{
    extern int get_current_device_id(char *device_id, int max_len);

    char device_id[40] = {0};
    if (get_current_device_id(device_id, sizeof(device_id)) == 0 && strlen(device_id) >= 4) {
        snprintf(s_name, sizeof(s_name), "LING-%c%c%c%c",
                 device_id[strlen(device_id) - 4], device_id[strlen(device_id) - 3],
                 device_id[strlen(device_id) - 2], device_id[strlen(device_id) - 1]);
    }

    uint8_t n = 0;
    /* Complete list of 128-bit Service UUIDs */
    s_scan_rsp[n++] = (uint8_t)(1 + 16);
    s_scan_rsp[n++] = GAP_AD_TYPE_128BIT_COMPLETE;
    memcpy(&s_scan_rsp[n], k_svc_uuid, 16);
    n += 16;

    /* Complete local name（扫描响应里的名字覆盖广播里的 "ARCS"） */
    const uint8_t name_len = (uint8_t)strlen(s_name);
    s_scan_rsp[n++] = (uint8_t)(1 + name_len);
    s_scan_rsp[n++] = GAP_AD_TYPE_LOCAL_NAME_COMPLETE;
    memcpy(&s_scan_rsp[n], s_name, name_len);
    n += name_len;

    s_scan_rsp_len = n;
    LISA_LOGI(TAG, "scan rsp: name=%s len=%u", s_name, n);
}

/* 覆盖 SDK 的 weak 默认实现（只放 UUID16 电池服务） */
const uint8_t *lisa_bt_get_scan_rsp_data(uint8_t *len)
{
    if (s_scan_rsp_len == 0) {
        build_scan_rsp();
    }
    if (len != NULL) {
        *len = s_scan_rsp_len;
    }
    return s_scan_rsp;
}

/* ---------------- 初始化与连接事件 ---------------- */

int ic_link_init(void)
{
    if (s_link.inited) {
        return 0;
    }

    memset(&s_link, 0, sizeof(s_link));

    s_link.free_q = xQueueCreate(IC_LINK_TX_SLOTS, sizeof(ic_tx_slot_t *));
    s_link.filled_q = xQueueCreate(IC_LINK_TX_SLOTS, sizeof(ic_tx_slot_t *));
    s_link.sent_sem = xSemaphoreCreateBinary();
    if (s_link.free_q == NULL || s_link.filled_q == NULL || s_link.sent_sem == NULL) {
        LISA_LOGE(TAG, "queue/sem alloc failed");
        return -1;
    }
    for (uint32_t i = 0; i < IC_LINK_TX_SLOTS; i++) {
        ic_tx_slot_t *slot = &s_link.slots[i];
        xQueueSend(s_link.free_q, &slot, 0);
    }

    uint8_t user_lid = BLE_GATT_INVALID_USER_LID;
    uint16_t start_hdl = 0;

    uint16_t status = ble_gatt_user_register(IC_PREF_MTU, 0, &k_gatt_cb, &user_lid);
    if (status != 0) {
        LISA_LOGE(TAG, "gatt user register failed: %d", status);
        return -2;
    }

    /* 运行期填 128-bit 特征 UUID（声明类属性用 16-bit 空间里的写法） */
    build_att_db();

    status = ble_gatt_db_svc_add(user_lid, 0, k_svc_uuid, IC_IDX_NB, NULL, k_att_db, IC_IDX_NB, &start_hdl);
    if (status != 0) {
        LISA_LOGE(TAG, "gatt svc add failed: %d", status);
        ble_gatt_user_unregister(user_lid);
        return -3;
    }

    s_link.user_lid = user_lid;
    s_link.start_hdl = start_hdl;
    s_link.inited = true;

    if (xTaskCreate(send_task, "ic_tx", IC_TX_TASK_STACK, NULL, IC_TX_TASK_PRIO, NULL) != pdPASS) {
        LISA_LOGE(TAG, "tx task create failed");
        return -4;
    }

    (void)build_scan_rsp();

    LISA_LOGI(TAG, "NUS registered: user_lid=%u start_hdl=0x%04X", user_lid, start_hdl);
    return 0;
}

void ic_link_on_connected(uint8_t conidx, const void *peer_addr)
{
    (void)peer_addr;
    s_link.conidx = conidx;
    s_link.connected = true;
    s_link.cccd_on = false;
    s_link.stat.connects++;
    LISA_LOGI(TAG, "connected conidx=%u", conidx);
    /* 上层的 hello/status 要等手机订阅（CCCD 打开）后再发，见 cb_att_val_set */
}

void ic_link_on_disconnected(uint8_t conidx, uint16_t reason)
{
    if (s_link.connected && s_link.conidx == conidx) {
        s_link.connected = false;
        s_link.cccd_on = false;
        s_link.stat.disconnects++;
        LISA_LOGI(TAG, "disconnected conidx=%u reason=0x%04X", conidx, reason);
        ic_app_on_link_down();
    }
}

void ic_link_on_bond(uint8_t conidx, uint8_t info, uint8_t value)
{
    LISA_LOGI(TAG, "bond event conidx=%u info=%u value=%u", conidx, info, value);
}
