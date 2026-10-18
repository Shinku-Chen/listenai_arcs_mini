/*
 * 对讲机应用层：会话状态、控制/事件 JSON、按键语义
 *
 * 与参考实现（ai-passport oc_app.c）对齐的行为：
 *  - 连接建立后设备主动发 hello（proto / caps / model / fw）；
 *  - 手机 hello 到达才算"握手完成"，此时才允许依赖链路的交互；
 *  - 按住说话：按下立刻开一轮（屏幕变红），收到 turn_ready 变绿，松开结束本轮；
 *    短按只当亮屏，发 turn_cancel 撤销本轮；
 *  - 文本按角色上屏；网关状态由手机上报。
 *
 * 本期不做（见 docs/development/intercom-firmware.md 的"已知限制"）：
 *  - 上行语音（需要 Opus 编码器）、下行 TTS（需要 Opus 解码器）；
 *  - 6 位配对码 + 加密门禁（安卓档案用 Just Works）。
 */
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "voice_msg.h"
#include "lisa_btn.h"
#include "battery/battery.h"

#include "cJSON.h"

#include "ic_proto.h"
#include "ic_link.h"
#include "ic_ui.h"
#include "ic_app.h"

#define TAG "ic.app"
#include "lisa_log.h"

/* 对讲机固件版本（hello 里上报；协议增量不改它） */
#define IC_APP_FW_VERSION "0.1.0"
#define IC_APP_MODEL "Ling-Arcs-mini3"

/* 短按撤销阈值：按下到松开的时长（ms）。参考实现用约 350ms */
#define IC_TURN_MIN_HOLD_MS 350U
/* 发出 turn_start 后等手机 turn_ready 的兜底超时 */
#define IC_TURN_READY_TIMEOUT_MS 2500U

static struct {
    bool inited;
    bool hello_sent;
    bool handshake_done;
    bool foreground;
    uint8_t turn_state;      /* IC_TURN_* */
    bool turn_active;
    uint32_t turn_started_ms;
    uint32_t turn_frames;
    int volume;
    int mic_gain_db;
} s_app;

/* ---------------- 发送辅助 ---------------- */

static void send_event_json(cJSON *obj)
{
    if (obj == NULL) {
        return;
    }
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (txt == NULL) {
        return;
    }
    const size_t len = strlen(txt);
    if (len > 0 && len <= IC_MAX_PAYLOAD) {
        ic_link_send(IC_T_EVENT, 0, txt, (uint16_t)len);
    } else {
        LISA_LOGW(TAG, "event too long: %u", (unsigned)len);
    }
    cJSON_free(txt);
}

static void send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return;
    }
    cJSON_AddStringToObject(o, "ev", "hello");
    cJSON_AddNumberToObject(o, "proto", IC_PROTO_VERSION);
    /* 能力表如实反映本期实现：只有文本与 PCM 兜底的上行占位，没有 opus / tts_opus */
    cJSON *caps = cJSON_AddArrayToObject(o, "caps");
    if (caps != NULL) {
        cJSON_AddItemToArray(caps, cJSON_CreateString("text"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("status"));
    }
    cJSON_AddStringToObject(o, "model", IC_APP_MODEL);
    cJSON_AddStringToObject(o, "fw", IC_APP_FW_VERSION);
    send_event_json(o);
    s_app.hello_sent = true;
}

static void send_status(void)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return;
    }
    cJSON_AddStringToObject(o, "ev", "status");
    cJSON_AddNumberToObject(o, "battery", battery_get_pct_raw());
    cJSON_AddNumberToObject(o, "volume", s_app.volume);
    cJSON_AddNumberToObject(o, "mic_gain_db", s_app.mic_gain_db);
    cJSON_AddStringToObject(o, "link", ic_link_notify_enabled() ? "ready" : "idle");
    send_event_json(o);

    ic_ui_set_battery(battery_get_pct_raw());
}

/* ---------------- 轮次 ---------------- */

static void ui_turn(uint8_t state)
{
    s_app.turn_state = state;
    ic_ui_set_turn(state);
}

static void turn_begin(void)
{
    if (!ic_link_notify_enabled()) {
        return;
    }

    s_app.turn_active = true;
    s_app.turn_frames = 0;
    s_app.turn_started_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    cJSON *o = cJSON_CreateObject();
    if (o != NULL) {
        cJSON_AddStringToObject(o, "ev", "turn_start");
        send_event_json(o);
    }
    ui_turn(IC_TURN_PREPARE);
}

static void turn_cancel(void)
{
    if (!s_app.turn_active) {
        return;
    }
    s_app.turn_active = false;

    cJSON *o = cJSON_CreateObject();
    if (o != NULL) {
        cJSON_AddStringToObject(o, "ev", "turn_cancel");
        send_event_json(o);
    }
    ui_turn(IC_TURN_IDLE);
}

static void turn_end(void)
{
    if (!s_app.turn_active) {
        return;
    }
    s_app.turn_active = false;

    cJSON *o = cJSON_CreateObject();
    if (o != NULL) {
        cJSON_AddStringToObject(o, "ev", "turn_end");
        cJSON_AddNumberToObject(o, "frames", s_app.turn_frames);
        cJSON_AddStringToObject(o, "codec", "pcm");
        cJSON_AddNumberToObject(o, "dropped", ic_link_stat()->tx_dropped);
        send_event_json(o);
    }
    ui_turn(IC_TURN_IDLE);
}

/* ---------------- 控制消息 ---------------- */

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void handle_hello(const cJSON *o)
{
    const cJSON *proto = cJSON_GetObjectItemCaseSensitive(o, "proto");
    const int their = cJSON_IsNumber(proto) ? proto->valueint : 0;
    s_app.handshake_done = true;

    if (their != IC_PROTO_VERSION) {
        LISA_LOGW(TAG, "proto mismatch: device=%d phone=%d", IC_PROTO_VERSION, their);
    }
    LISA_LOGI(TAG, "handshake done (proto=%d)", their);

    send_status();
    ic_ui_set_link(true, s_app.handshake_done);
}

static void handle_gateway(const cJSON *o)
{
    const char *state = jstr(o, "state");
    const char *detail = jstr(o, "detail");
    if (state == NULL) {
        return;
    }
    ic_ui_set_gateway(state, detail);
    ic_ui_set_link(ic_link_connected(), (strcmp(state, "ready") == 0));
}

static void handle_audio(const cJSON *o)
{
    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(o, "volume");
    const cJSON *mic = cJSON_GetObjectItemCaseSensitive(o, "mic_gain_db");

    if (cJSON_IsNumber(mic)) {
        s_app.mic_gain_db = mic->valueint;
    }
    if (cJSON_IsNumber(vol)) {
        int v = vol->valueint;
        if (v < 0) {
            v = 0;
        } else if (v > 100) {
            v = 100;
        }
        s_app.volume = v;
        extern void service_volume_set(int volume);
        service_volume_set(v);
    }
    /* 回显：状态以设备实际值为准 */
    send_status();
}

void ic_app_on_control(const char *json, uint16_t len)
{
    (void)len;
    cJSON *o = cJSON_Parse(json);
    if (o == NULL) {
        LISA_LOGW(TAG, "bad control json");
        return;
    }

    const char *cmd = jstr(o, "cmd");
    const char *ev = jstr(o, "ev");

    if (cmd != NULL) {
        if (strcmp(cmd, "hello") == 0) {
            handle_hello(o);
        } else if (strcmp(cmd, "status") == 0) {
            send_status();
        } else if (strcmp(cmd, "clear_display") == 0) {
            ic_ui_clear();
        } else if (strcmp(cmd, "gateway") == 0) {
            handle_gateway(o);
        } else if (strcmp(cmd, "audio") == 0) {
            handle_audio(o);
        } else if (strcmp(cmd, "turn_start") == 0) {
            /* 手机已接受本轮并开始识别 */
            ui_turn(IC_TURN_READY);
        } else if (strcmp(cmd, "time") == 0) {
            /* 本期界面不显示时钟，忽略（协议要求未知/无用的命令静默忽略） */
        } else {
            LISA_LOGI(TAG, "ignore unknown cmd=%s", cmd);
        }
    } else if (ev != NULL) {
        if (strcmp(ev, "turn_ready") == 0) {
            ui_turn(IC_TURN_READY);
        } else if (strcmp(ev, "tts_start") == 0 || strcmp(ev, "tts_stop") == 0 ||
                   strcmp(ev, "tts_abort") == 0) {
            /* 本期不声明 tts_opus 能力：手机不应下发。收到就记一笔，不做播放 */
            LISA_LOGI(TAG, "tts event ignored: %s", ev);
        } else {
            LISA_LOGI(TAG, "ignore unknown ev=%s", ev);
        }
    }

    cJSON_Delete(o);
}

void ic_app_on_text(uint8_t role, const char *utf8, uint16_t len, bool truncated)
{
    if (truncated) {
        LISA_LOGW(TAG, "text truncated by device buffer");
    }
    ic_ui_add_text(role, utf8, len);
}

/* 手机 hello 到达 = 用户打开了 App：把对讲页推到前台 */
static void maybe_foreground(void)
{
    if (s_app.foreground) {
        return;
    }
    s_app.foreground = true;
    ic_ui_show();
}

/* ---------------- 链路事件 ---------------- */

void ic_app_on_link_up(void)
{
    s_app.hello_sent = false;
    s_app.handshake_done = false;
    ic_ui_set_link(true, false);
    send_hello();
}

void ic_app_on_link_down(void)
{
    s_app.hello_sent = false;
    s_app.handshake_done = false;
    s_app.turn_active = false;
    ui_turn(IC_TURN_IDLE);
    ic_ui_set_link(false, false);
}

/* ---------------- 按键 ---------------- */

/* 原始边沿（来自 service_button.c 的驱动回调）：按下 / 松开 */
void ic_app_on_raw_button(lisa_btn_event_t event, uint8_t btn_id)
{
    if (!s_app.inited || btn_id != 0) {
        return;
    }
    if (!s_app.foreground) {
        return;
    }

    switch (event) {
    case LISA_BTN_PRESS_DOWN:
        turn_begin();
        break;
    case LISA_BTN_PRESS_SHORT_UP:
    case LISA_BTN_PRESS_CLICK: {
        /* 短按：只当亮屏，撤销本轮 */
        const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const uint32_t held = now - s_app.turn_started_ms;
        if (held < IC_TURN_MIN_HOLD_MS) {
            turn_cancel();
        }
        break;
    }
    case LISA_BTN_PRESS_LONG_UP:
    case LISA_BTN_PRESS_LONG_HOLD_UP:
        turn_end();
        break;
    default:
        break;
    }
}

/* 对讲页在前台时独占点击序列：避免按住说话触发助手页/关机 */
bool ic_app_intercept_action(int action)
{
    if (!s_app.foreground) {
        return false;
    }

    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
        /* 三击：退出对讲，回到默认页 */
        s_app.foreground = false;
        ic_ui_hide();
        return true;
    case VOICE_MSG_BUTTON_ACTION_CLICK:
    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK:
    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        /* 对讲页里这些动作不落到助手逻辑（长按按住在说话，不能关机） */
        return true;
    default:
        return false;
    }
}

/* ---------------- 初始化 ---------------- */

int ic_app_init(void)
{
    if (s_app.inited) {
        return 0;
    }

    memset(&s_app, 0, sizeof(s_app));

    static const ic_proto_cb_t proto_cb = {
        .on_text = ic_app_on_text,
        .on_control = ic_app_on_control,
    };
    ic_proto_init(&proto_cb);

    /* GATT 服务不在主任务注册：BLE 协议栈就绪时由 platform.c 的
     * platform_ble_ready_cb (BT 任务上下文) 调 ic_link_init()。 */

    const int ui_ret = ic_ui_init();
    if (ui_ret != 0) {
        LISA_LOGE(TAG, "ic_ui_init failed: %d", ui_ret);
        return ui_ret;
    }

    s_app.volume = 50;
    s_app.mic_gain_db = 0;
    s_app.turn_state = IC_TURN_IDLE;
    s_app.inited = true;

    ic_ui_set_battery(battery_get_pct_raw());
    LISA_LOGI(TAG, "intercom app ready (proto=%d)", IC_PROTO_VERSION);
    return 0;
}
