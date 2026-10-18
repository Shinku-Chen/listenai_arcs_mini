/*
 * 电台播放器实现，见 radio_player.h 的设计说明。
 *
 * 焦点策略（踩过坑之后的定版）：
 *  - 不把「焦点状态」当成要播放的前置条件，否则自己 stop() 释放焦点会把
 *    s_focus 永久置成 false，让状态机再也起不来（真机实测：卡在 STOPPED，
 *    同时疯狂重试刷日志把控制台打死）。
 *  - 焦点回调里先分辨「是不是自己引起的」：by_which == 自己时直接忽略。
 *  - 只有「被别的播放器抢走」（TTS / 提示音 / 云端音乐）才置 s_hold，停止拉流；
 *    对方释放后清 s_hold 重新拉一次新流（直播流不做 resume）。
 *  - 起播尝试有最小间隔，避免任何异常状态下变成忙循环。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define TAG "radio.player"
#include "lisa_log.h"

#include "app_player.h"
#include "app_player_focus.h"
#include "voice_player_comm.h"

#include "radio_catalog.h"
#include "radio_player.h"

/* ==================== 配置 ==================== */

#define RADIO_AUDIO_TASK_STACK 4096
#define RADIO_AUDIO_TASK_PRIO  4
/* 状态机轮询周期：直播流不需要更细的粒度，128 ms 足够跟手 */
#define RADIO_AUDIO_TICK_MS    128
/* 两次起播尝试之间的最小间隔（防止异常状态下忙循环刷日志） */
#define RADIO_AUDIO_RETRY_MIN_MS 2000
/* 起播被别的播放器抢焦点取消时的快速重试间隔（开机提示音就是这种情况） */
#define RADIO_AUDIO_FOCUS_RETRY_MS 300
/* 单台连续失败几次后自动换台（公共目录里死链很多） */
#define RADIO_AUDIO_FAIL_LIMIT  3
/* 失败后的退避：attempt * 1000 ms */
#define RADIO_AUDIO_BACKOFF_MS  1000
/* 被别的播放器抢占后，隔多久试探性重新申请焦点。
 * 开机提示音只有 1s 左右，这里不能设 5s（否则播放在开机后要等 5s 才起来） */
#define RADIO_AUDIO_HOLD_PROBE_MS 1200

/* ==================== 状态 ==================== */

static app_player_t *s_player;
static SemaphoreHandle_t s_signal;
static TaskHandle_t s_task;

static char s_url[RADIO_URL_MAX];
static volatile bool s_wanted;
static volatile bool s_net_ok;
static volatile bool s_hold;                 /* 音频被更高优先级播放器占着 */
static volatile radio_pb_state_t s_state = RADIO_PB_STOPPED;
static volatile uint8_t s_fail_count;
static volatile uint32_t s_retry_at_ms;
static volatile uint32_t s_last_attempt_ms;
static volatile uint32_t s_hold_since_ms;
/* 每次换 URL 递增；player 任务用它区分「当前流是不是这个 URL 的」 */
static volatile uint32_t s_url_gen = 1;
static volatile uint32_t s_playing_gen;
static radio_audio_fail_cb_t s_fail_cb;

static uint32_t radio_audio_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void radio_audio_kick(void)
{
    if (s_signal != NULL) {
        (void)xSemaphoreGive(s_signal);
    }
}

static bool radio_audio_pipeline_active(app_player_state_t state)
{
    return state == APP_PLAYER_STATE_PREPARING || state == APP_PLAYER_STATE_PREPARED ||
           state == APP_PLAYER_STATE_PLAYING || state == APP_PLAYER_STATE_PAUSED;
}

static void radio_audio_set_state(radio_pb_state_t state)
{
    if (s_state == state) {
        return;
    }
    s_state = state;
    LOGI("state -> %u", (unsigned)state);
}

/* ==================== 焦点 ==================== */

/*
 * 焦点变化回调：只记录「是否被别的播放器抢走」，返回 true 表示完全接管
 * （默认的 pause 策略对直播流没有意义，我们自己决定 stop / 重拉）。
 */
static bool radio_audio_focus_cb(app_player_t *player, app_player_focus_state_t new_state,
                                 app_player_t *by_which, void *user_data)
{
    (void)user_data;

    /* 自己 stop / pause 释放焦点时 by_which 指向自己：忽略，否则会把自己锁死 */
    if (by_which == player) {
        return true;
    }

    const bool foreground = (new_state == APP_PLAYER_FOCUS_FOREGROUND);
    if (foreground == !s_hold) {
        return true; /* 没有变化 */
    }

    s_hold = !foreground;
    if (s_hold) {
        s_hold_since_ms = radio_audio_now_ms();
    }
    LOGI("focus %s", foreground ? "foreground" : "taken by other player");
    radio_audio_kick();
    return true;
}

/* ==================== 事件 ==================== */

static void radio_audio_notify_failure(const char *reason)
{
    const uint8_t attempt = (uint8_t)(s_fail_count + 1u);

    s_fail_count = attempt;
    radio_audio_set_state(RADIO_PB_ERROR);
    s_retry_at_ms = radio_audio_now_ms() + (uint32_t)attempt * RADIO_AUDIO_BACKOFF_MS;
    LOGW("%s (attempt=%u)", reason, (unsigned)attempt);

    if (attempt >= RADIO_AUDIO_FAIL_LIMIT) {
        s_fail_count = 0;
        s_retry_at_ms = radio_audio_now_ms() + 800u;
        if (s_fail_cb != NULL) {
            s_fail_cb();
        }
    }
    radio_audio_kick();
}

static void radio_audio_event_cb(app_player_t *player, app_player_event_t event, void *user_data)
{
    (void)player;
    (void)user_data;

    switch (event) {
    case APP_PLAYER_EVENT_PLAYING:
        s_fail_count = 0;
        radio_audio_set_state(RADIO_PB_PLAYING);
        break;
    case APP_PLAYER_EVENT_ERROR:
        radio_audio_notify_failure("播放中断");
        break;
    case APP_PLAYER_EVENT_COMPLETED:
        /* 直播流理论上没有 COMPLETED；真收到说明服务端把流关了，按失败重连 */
        radio_audio_notify_failure("流已结束");
        break;
    case APP_PLAYER_EVENT_STOPPED:
        if (s_state == RADIO_PB_PLAYING || s_state == RADIO_PB_CONNECTING) {
            radio_audio_set_state(RADIO_PB_STOPPED);
        }
        break;
    case APP_PLAYER_EVENT_PREPARED:
        /* PREPARED → PLAYING 只有几十毫秒，界面状态等 PLAYING 再切 */
        break;
    default:
        break;
    }
}

/* ==================== 状态机 ==================== */

static bool radio_audio_should_play(void)
{
    return s_wanted && s_net_ok && !s_hold && s_url[0] != '\0';
}

static void radio_audio_step(void)
{
    if (s_player == NULL) {
        return;
    }

    const app_player_state_t state = app_player_get_state(s_player);
    const uint32_t generation = s_url_gen;
    const uint32_t now = radio_audio_now_ms();

    /* 被别的播放器抢占太久：试探性放行，起播会重新申请一次焦点 */
    if (s_hold && (uint32_t)(now - s_hold_since_ms) >= RADIO_AUDIO_HOLD_PROBE_MS) {
        s_hold = false;
    }

    if (!radio_audio_should_play()) {
        if (radio_audio_pipeline_active(state)) {
            (void)app_player_stop(s_player);
        }
        if (s_state != RADIO_PB_STOPPED) {
            radio_audio_set_state(RADIO_PB_STOPPED);
        }
        s_fail_count = 0;
        return;
    }

    if (state == APP_PLAYER_STATE_PREPARING || state == APP_PLAYER_STATE_PREPARED ||
        state == APP_PLAYER_STATE_PLAYING) {
        if (s_playing_gen != generation) {
            /* URL 换成另一个台了，但旧流还在跑：先停掉，下一轮重新拉 */
            (void)app_player_stop(s_player);
            radio_audio_set_state(RADIO_PB_CONNECTING);
            return;
        }
        radio_audio_set_state(RADIO_PB_PLAYING);
        return;
    }

    if (state == APP_PLAYER_STATE_PAUSED) {
        /* 不做 resume：直播流恢复后可能是一段旧数据，直接重拉更可靠 */
        (void)app_player_stop(s_player);
        radio_audio_set_state(RADIO_PB_CONNECTING);
        return;
    }

    if ((int32_t)(now - s_retry_at_ms) < 0) {
        return; /* 退避窗口里 */
    }
    if ((int32_t)(now - s_last_attempt_ms) < (int32_t)RADIO_AUDIO_RETRY_MIN_MS) {
        return; /* 起播间隔没到，避免忙循环 */
    }

    s_last_attempt_ms = now;
    s_playing_gen = generation;
    radio_audio_set_state(RADIO_PB_CONNECTING);
    const int ret = app_player_play(s_player, s_url);
    if (ret != APP_PLAYER_OK) {
        /* 拉流失败（DNS / 连不上 / 超时）：不立刻重试，交给退避窗口 */
        radio_audio_notify_failure("拉流失败");
    } else if (!radio_audio_pipeline_active(app_player_get_state(s_player))) {
        /* play() 返回 OK 但没进管线，说明只是存了 pending URL（焦点被别人占着，
         * 比如开机提示音）。这种情况不要把重试拖满 2 s。 */
        s_last_attempt_ms = now - (RADIO_AUDIO_RETRY_MIN_MS - RADIO_AUDIO_FOCUS_RETRY_MS);
    }
}

static void radio_audio_task(void *argument)
{
    (void)argument;

    for (;;) {
        radio_audio_step();
        (void)xSemaphoreTake(s_signal, pdMS_TO_TICKS(RADIO_AUDIO_TICK_MS));
    }
}

/* ==================== 对外接口 ==================== */

int radio_audio_init(void)
{
    if (s_task != NULL) {
        return 0;
    }

    /* 播放器实例由 voice_player_platform_init() 创建（见 voice_player_comm.c），
     * 这里只接管它：注册事件回调与焦点回调。 */
    s_player = radio_player;
    if (s_player == NULL) {
        LOGE("radio player instance is NULL (CONFIG_APP_PLAYER missing?)");
        return -1;
    }

    s_signal = xSemaphoreCreateBinary();
    if (s_signal == NULL) {
        LOGE("failed to create signal");
        return -1;
    }

    const int cb_ret = app_player_register_callback(s_player, radio_audio_event_cb, NULL);
    if (cb_ret != APP_PLAYER_OK) {
        LOGW("register event callback failed: %d", cb_ret);
    }
    const int focus_ret = app_player_focus_register_callback(s_player, radio_audio_focus_cb, NULL);
    if (focus_ret != 0) {
        /* 没有焦点通道时仍可播放，只是会被 TTS / 提示音盖住 */
        LOGW("register focus callback failed: %d", focus_ret);
    }

    if (xTaskCreate(radio_audio_task, "radio.play", RADIO_AUDIO_TASK_STACK, NULL,
                    RADIO_AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        LOGE("failed to create player task");
        s_task = NULL;
        return -1;
    }
    return 0;
}

void radio_audio_set_fail_cb(radio_audio_fail_cb_t cb)
{
    s_fail_cb = cb;
}

void radio_audio_set_network(bool ok)
{
    if (s_net_ok == ok) {
        return;
    }
    s_net_ok = ok;
    if (ok) {
        /* 刚恢复网络：清掉退避，尽快重连 */
        s_retry_at_ms = 0;
        s_last_attempt_ms = 0;
        s_fail_count = 0;
    }
    radio_audio_kick();
}

void radio_audio_play(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return;
    }

    const bool same = (strcmp(s_url, url) == 0);
    if (same && s_wanted) {
        return; /* 同一地址，已经在播或在重连，不必打断 */
    }

    if (!same) {
        snprintf(s_url, sizeof(s_url), "%s", url);
        s_url_gen++;
        radio_audio_set_state(RADIO_PB_CONNECTING);
    }
    s_wanted = true;
    s_fail_count = 0;
    s_retry_at_ms = 0;
    s_last_attempt_ms = 0; /* 用户主动换台：允许立刻起播 */
    radio_audio_kick();
}

void radio_audio_stop(void)
{
    if (!s_wanted) {
        return;
    }
    s_wanted = false;
    radio_audio_set_state(RADIO_PB_STOPPED);
    radio_audio_kick();
}

bool radio_audio_wanted(void)
{
    return s_wanted;
}

radio_pb_state_t radio_audio_get_state(void)
{
    return s_state;
}

uint32_t radio_audio_debug_generation(uint8_t which)
{
    return (which == 0) ? s_playing_gen : s_url_gen;
}
