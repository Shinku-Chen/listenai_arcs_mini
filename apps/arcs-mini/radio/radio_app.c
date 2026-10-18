/*
 * 网络电台应用层实现，见 radio_app.h 的说明。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define TAG "radio.app"
#include "lisa_log.h"

#include <lvgl.h>

#include "cJSON.h"
#include "lisa_kv.h"
#include "shell.h"
#include "sysutils.h"

#include "voice_msg.h"
#include "sys_network_manager.h"
#include "sys_wifi.h"
#include "service_volume.h"
#include "service_power_policy.h"

#include "lisa_ui_nav_scr_ids.h"

#include "radio_catalog.h"
#include "radio_net.h"
#include "radio_player.h"
#include "radio_ui.h"
#include "radio_app.h"

/* ==================== 配置 ==================== */

#define RADIO_APP_TASK_STACK 4096
#define RADIO_APP_TASK_PRIO  4
#define RADIO_APP_TICK_MS    200

/** 开机后延后多久自己进电台页：给助手主页 / 配网引导页留出首次展示机会 */
#define RADIO_AUTOSTART_DELAY_MS 1200

/* 进/出页是异步的：如果 400 ms 后还没到位就再投递一次 */
#define RADIO_NAV_RETRY_MS 400

/* 卷加一档 */
#define RADIO_VOLUME_STEP 5

#define RADIO_KV_KEY_STATION "radio.station"
#define RADIO_KV_KEY_CITY    "radio.city"

/* leo-radio 的定时播放档位（分钟）；0 = 关闭，且不跨重启保留 */
static const uint8_t s_sleep_choices[] = {0, 15, 30, 60, 90};

/* ==================== 状态 ==================== */

typedef struct {
    bool inited;
    bool foreground;

    radio_station_t list[RADIO_MAX_STATIONS];
    uint8_t count;
    uint8_t index;
    char remembered[RADIO_NAME_MAX];

    char source[RADIO_SOURCE_MAX];
    char detail[RADIO_DETAIL_MAX];

    uint8_t page;
    uint8_t menu_index;
    uint8_t list_index;
    uint8_t city_index;

    uint8_t sleep_choice;
    uint32_t sleep_left_ms;

    bool net_ok;
    bool busy;
    bool auto_queried;
    bool user_paused;
    bool nav_confirmed;  /* 电台页已经真的上到前台一次 */

#ifdef CONFIG_RADIO_AUTO_START
    bool autostart_pending;
    uint32_t autostart_at_ms;
#endif

    uint32_t last_nav_ms;
} radio_state_t;

/* 约 3.9 KB（台单数组 + 名字缓冲），SRAM 只剩十几 KB，整块放 PSRAM */
static radio_state_t s_state __psram_bss__;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

/* ==================== 小工具 ==================== */

static uint32_t radio_app_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void radio_app_set_detail(const char *detail)
{
    snprintf(s_state.detail, sizeof(s_state.detail), "%s", (detail != NULL) ? detail : "");
}

static void radio_app_set_source(const char *source)
{
    snprintf(s_state.source, sizeof(s_state.source), "%s", (source != NULL) ? source : "内置");
}

/* 锁内取当前台；调用方必须已经持锁 */
static const radio_station_t *radio_app_station_locked(uint8_t index)
{
    if (s_state.count == 0) {
        return NULL;
    }
    return &s_state.list[index % s_state.count];
}

static void radio_app_remember_locked(void)
{
    const radio_station_t *station = radio_app_station_locked(s_state.index);
    if (station != NULL) {
        snprintf(s_state.remembered, sizeof(s_state.remembered), "%s", station->name);
    }
}

/* ==================== 台单安装 ==================== */

/* 锁内执行：装入台单并决定选中项（优先沿用 remembered 里的台名） */
static void radio_app_install_locked(const radio_station_t *list, uint8_t count,
                                     const char *source)
{
    uint8_t selected = 0;

    if (list == NULL || count == 0) {
        return;
    }
    if (count > RADIO_MAX_STATIONS) {
        count = RADIO_MAX_STATIONS;
    }

    memset(s_state.list, 0, sizeof(s_state.list));
    memcpy(s_state.list, list, (size_t)count * sizeof(radio_station_t));
    s_state.count = count;

    if (s_state.remembered[0] != '\0') {
        for (uint8_t i = 0; i < count; ++i) {
            if (strcmp(s_state.list[i].name, s_state.remembered) == 0) {
                selected = i;
                break;
            }
        }
    }
    s_state.index = selected;
    s_state.list_index = selected;
    radio_app_set_source(source);
    radio_app_remember_locked();
}

static void radio_app_install_builtin(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_state.list, 0, sizeof(s_state.list));
    s_state.count = (uint8_t)radio_catalog_builtin(s_state.list, RADIO_MAX_STATIONS);
    s_state.index = 0;
    s_state.list_index = 0;
    for (uint8_t i = 0; i < s_state.count; ++i) {
        if (s_state.remembered[0] != '\0' &&
            strcmp(s_state.list[i].name, s_state.remembered) == 0) {
            s_state.index = i;
            s_state.list_index = i;
            break;
        }
    }
    radio_app_set_source(radio_catalog_builtin_source());
    radio_app_remember_locked();
    xSemaphoreGive(s_lock);
}

/* 锁外执行：按当前选中项拉起播放 */
static void radio_app_play_current(void)
{
    char url[RADIO_URL_MAX] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const radio_station_t *station = radio_app_station_locked(s_state.index);
    if (station != NULL) {
        snprintf(url, sizeof(url), "%s", station->url);
    }
    s_state.user_paused = false;
    xSemaphoreGive(s_lock);

    if (url[0] != '\0') {
        radio_audio_play(url);
    }
}

static void radio_app_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.user_paused = true;
    xSemaphoreGive(s_lock);
    radio_audio_stop();
}

static void radio_app_select(uint8_t index)
{
    char url[RADIO_URL_MAX] = {0};
    char name[RADIO_NAME_MAX] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.count > 0) {
        s_state.index = (uint8_t)(index % s_state.count);
        s_state.list_index = s_state.index;
        const radio_station_t *station = radio_app_station_locked(s_state.index);
        if (station != NULL) {
            snprintf(url, sizeof(url), "%s", station->url);
            snprintf(name, sizeof(name), "%s", station->name);
        }
        s_state.user_paused = false;
        radio_app_set_detail(NULL);
        radio_app_remember_locked();
    }
    xSemaphoreGive(s_lock);

    if (url[0] == '\0') {
        return;
    }
    /* 只在用户主动选台时落盘，自动扫描不写 flash */
    if (name[0] != '\0') {
        (void)lisa_kv_set_string(RADIO_KV_KEY_STATION, name);
    }
    radio_audio_play(url);
}

static void radio_app_next(void)
{
    uint8_t next = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.count > 0) {
        next = (uint8_t)((s_state.index + 1u) % s_state.count);
    }
    xSemaphoreGive(s_lock);

    radio_app_select(next);
}

/* 重复失败（死链）时由播放器任务回调：换下一台 */
static void radio_app_on_player_failed(void)
{
    radio_app_next();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    radio_app_set_detail("电台无法播放 已换台");
    xSemaphoreGive(s_lock);
}

/* ==================== 目录查询 ==================== */

static void radio_app_start_auto_query(void)
{
    if (s_lock == NULL) {
        return;
    }

    if (s_state.auto_queried || !s_state.net_ok) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.auto_queried = true;
    s_state.busy = true;
    radio_app_set_detail(NULL);
    xSemaphoreGive(s_lock);

    if (radio_net_request_auto() != 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.busy = false;
        s_state.auto_queried = false;
        xSemaphoreGive(s_lock);
        LOGW("auto query not available");
    }
}

static void radio_app_start_city_query(uint8_t city_index)
{
    const radio_city_t *city = radio_catalog_city(city_index);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.city_index = city_index;
    s_state.busy = true;
    radio_app_set_detail(NULL);
    xSemaphoreGive(s_lock);

    (void)lisa_kv_set_int(RADIO_KV_KEY_CITY, (int)city_index);

    /* 「自动定位」= 走一次 IP 定位 */
    if (city == NULL || city->query == NULL) {
        (void)radio_net_request_auto();
    } else {
        (void)radio_net_request_city(city->query, city->display);
    }
}

/* 网络任务回调（在 radio.net 任务上下文里执行） */
static void radio_app_on_net(radio_net_event_t event, const radio_station_t *list,
                             uint8_t count, const radio_location_t *location,
                             const char *source, const char *detail)
{
    (void)location;

    if (event == RADIO_NET_EVENT_STATIONS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        radio_app_install_locked(list, count, source);
        s_state.busy = false;
        radio_app_set_detail(NULL);
        const bool paused = s_state.user_paused;
        xSemaphoreGive(s_lock);

        LOGI("installed %u stations from '%s'", (unsigned)count, (source != NULL) ? source : "?");
        if (!paused) {
            radio_app_play_current();
        }
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.busy = false;
    /* 查不到就继续用现有台单（启动时就是内置 6 台），不把红字横幅钉在状态行上；
     * 真的一个台都没有时才提示。 */
    if (s_state.count == 0) {
        radio_app_set_detail((detail != NULL) ? detail : "没有可用电台");
    }
    /* 失败之后允许下次进页 / 切城市时重新查一次（否则 auto_queried 一直为真） */
    s_state.auto_queried = false;
    xSemaphoreGive(s_lock);
    LOGW("directory query failed: %s", (detail != NULL) ? detail : "unknown");
}

/* ==================== 导航（必须走 UI 线程） ==================== */

static void radio_app_nav_show_worker(void *arg)
{
    (void)arg;
    radio_ui_show();
}

static void radio_app_nav_hide_worker(void *arg)
{
    (void)arg;
    radio_ui_hide();
}

static void radio_app_open(void)
{
    if (s_state.foreground) {
        return;
    }
    s_state.foreground = true;
    s_state.nav_confirmed = false;
    s_state.page = RADIO_PAGE_PLAYER;
    s_state.menu_index = RADIO_MENU_LIST;
    s_state.list_index = s_state.index;
    s_state.last_nav_ms = radio_app_now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    radio_app_set_detail(s_state.net_ok ? NULL : "等待网络");
    xSemaphoreGive(s_lock);

    lv_async_call(radio_app_nav_show_worker, NULL);

    /* 电台页在前台时不进 IDLE/HIBERNATE：与小程序前台同一策略，
     * 否则播放中会被降亮度/挂起 USB，表现为"黑屏 + ADB 掉线" */
    service_power_policy_set_miniapp_active(true);

    if (s_state.count == 0) {
        radio_app_install_builtin();
    }
    radio_app_play_current();

    /* 有网且本次开机还没查过目录：进页就查一次（换城市、地点变了也能自动跟上） */
    if (s_state.net_ok) {
        radio_app_start_auto_query();
    }

    LOGI("radio page opened (%u stations, source=%s)", (unsigned)s_state.count,
         s_state.source);
}

static void radio_app_close(void)
{
    if (!s_state.foreground) {
        return;
    }
    s_state.foreground = false;
    s_state.nav_confirmed = false;

    /* 不清 user_paused：下次进来仍然从当前台开始播 */
    radio_audio_stop();
    (void)radio_net_cancel();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.busy = false;
    xSemaphoreGive(s_lock);

    lv_async_call(radio_app_nav_hide_worker, NULL);
    service_power_policy_set_miniapp_active(false);
    LOGI("radio page closed");
}

/* ==================== 按键 ==================== */

static void radio_app_volume_step(void)
{
    const int current = service_volume_get();
    int next = current + RADIO_VOLUME_STEP;

    if (current >= 100 || next > 100) {
        next = 10; /* 到顶回到底（service_volume 的下限默认 10） */
    }
    service_volume_set(next);
}

static void radio_app_toggle_play(void)
{
    bool play = false;
    char url[RADIO_URL_MAX] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_state.user_paused && radio_audio_wanted()) {
        s_state.user_paused = true;
    } else {
        s_state.user_paused = false;
        play = true;
        const radio_station_t *station = radio_app_station_locked(s_state.index);
        if (station != NULL) {
            snprintf(url, sizeof(url), "%s", station->url);
        }
    }
    xSemaphoreGive(s_lock);

    if (play) {
        if (url[0] != '\0') {
            radio_audio_play(url);
        }
    } else {
        radio_audio_stop();
    }
}

/* 单击：按当前页语义执行 */
static void radio_app_handle_click(void)
{
    switch ((radio_page_t)s_state.page) {
    case RADIO_PAGE_MENU:
        s_state.menu_index = (uint8_t)((s_state.menu_index + 1u) % RADIO_MENU_COUNT);
        break;
    case RADIO_PAGE_LIST:
        if (s_state.count > 0) {
            s_state.list_index = (uint8_t)((s_state.list_index + 1u) % s_state.count);
        }
        break;
    case RADIO_PAGE_CITY:
        s_state.city_index = (uint8_t)((s_state.city_index + 1u) % radio_app_city_count());
        break;
    case RADIO_PAGE_VOLUME:
        radio_app_volume_step();
        break;
    case RADIO_PAGE_PLAYER:
    default:
        radio_app_next();
        break;
    }
}

/* 双击：按当前页语义执行 */
static void radio_app_handle_double_click(void)
{
    switch ((radio_page_t)s_state.page) {
    case RADIO_PAGE_MENU:
        switch ((radio_menu_t)s_state.menu_index) {
        case RADIO_MENU_LIST:
            s_state.list_index = s_state.index;
            s_state.page = RADIO_PAGE_LIST;
            break;
        case RADIO_MENU_CITY:
            s_state.page = RADIO_PAGE_CITY;
            break;
        case RADIO_MENU_SLEEP: {
            const uint8_t count = radio_app_sleep_choice_count();
            const uint8_t choice = (uint8_t)((s_state.sleep_choice + 1u) % count);
            const uint32_t minutes = s_sleep_choices[choice];
            s_state.sleep_choice = choice;
            s_state.sleep_left_ms = minutes * 60u * 1000u;
            break;
        }
        case RADIO_MENU_VOLUME:
        default:
            s_state.page = RADIO_PAGE_VOLUME;
            break;
        }
        break;
    case RADIO_PAGE_LIST:
        radio_app_select(s_state.list_index);
        s_state.page = RADIO_PAGE_PLAYER;
        break;
    case RADIO_PAGE_CITY:
        radio_app_start_city_query(s_state.city_index);
        s_state.page = RADIO_PAGE_PLAYER;
        break;
    case RADIO_PAGE_VOLUME:
        s_state.page = RADIO_PAGE_MENU;
        break;
    case RADIO_PAGE_PLAYER:
    default:
        radio_app_toggle_play();
        break;
    }
}

/* 三击：返回上一级；播放页直接退出电台 */
static void radio_app_handle_triple_click(void)
{
    switch ((radio_page_t)s_state.page) {
    case RADIO_PAGE_VOLUME:
        s_state.page = RADIO_PAGE_MENU;
        break;
    case RADIO_PAGE_LIST:
    case RADIO_PAGE_CITY:
    case RADIO_PAGE_MENU:
        s_state.page = RADIO_PAGE_PLAYER;
        break;
    case RADIO_PAGE_PLAYER:
    default:
        radio_app_close();
        break;
    }
}

/* 长按：播放页进设置菜单；子页回上一级（不落到「长按=关机」） */
static void radio_app_handle_long_hold(void)
{
    switch ((radio_page_t)s_state.page) {
    case RADIO_PAGE_PLAYER:
        s_state.menu_index = RADIO_MENU_LIST;
        s_state.page = RADIO_PAGE_MENU;
        break;
    case RADIO_PAGE_VOLUME:
        s_state.page = RADIO_PAGE_MENU;
        break;
    case RADIO_PAGE_LIST:
    case RADIO_PAGE_CITY:
    case RADIO_PAGE_MENU:
    default:
        s_state.page = RADIO_PAGE_PLAYER;
        break;
    }
}

bool radio_app_intercept_action(int action)
{
    if (!s_state.inited) {
        return false;
    }

    /* 只有电台页真的在最上面时才独占按键。若它被别的页面（如扫码绑定页）
     * 盖住，就退回普通行为；四击在任何时候都能把电台重新提到最前。 */
    const bool on_top = s_state.foreground && radio_ui_is_visible();

    if (!on_top) {
        if (action == VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK) {
            radio_app_open();
            return true;
        }
        return false;
    }

    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
        radio_app_handle_click();
        return true;
    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
        radio_app_handle_double_click();
        return true;
    case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
        radio_app_handle_triple_click();
        return true;
    case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
        radio_app_close();
        return true;
    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        /* 电台页里长按不再区分「关机」，只用做进/出设置 */
        radio_app_handle_long_hold();
        return true;
    case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
        /* 放行给 BLE 配网：先退出电台，避免两套页面打架 */
        radio_app_close();
        return false;
    case VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK:
        /* 连击 8+ = 恢复出厂，电台页里吞掉，避免误触 */
        return true;
    default:
        return false;
    }
}

/* ==================== 任务 ==================== */

/* 这些页面是“闹钟 / OTA 确认 / 小应用”这类系统或用户明确要求的页面，
 * 必须让它盖住电台页：（OTA 确认页要等用户按键，顶掉它会把升级流程卡死）。
 * 其余页面（云端推的二维码页、信息页、被 nav 回主页）一律把电台页拉回来。 */
static bool radio_app_foreign_page_wins(int top_id)
{
    switch (top_id) {
    case LISA_UI_NAV_SCR_ID_ALARM_RING:
    case LISA_UI_NAV_SCR_ID_MINIAPP:
#ifdef CONFIG_OTA
    case LISA_UI_NAV_SCR_ID_OTA:
#endif
#ifdef CONFIG_LOG_UPLOAD
    case LISA_UI_NAV_SCR_ID_LOG_UPLOAD:
#endif
        return true;
    default:
        return false;
    }
}

static void radio_app_task(void *argument)
{
    (void)argument;

    for (;;) {
        /* 1. 网络状态：变化时同步给播放器，刚连上就查一次目录 */
        sys_network_status_t status;
        bool ok = false;
        if (sys_network_get_status(&status) == 0) {
            ok = status.connected;
        }
        if (ok != s_state.net_ok) {
            s_state.net_ok = ok;
            radio_audio_set_network(ok);
            if (ok) {
                s_state.auto_queried = false;
                if (s_state.foreground) {
                    radio_app_start_auto_query();
                }
            } else {
                s_state.auto_queried = false;
                s_state.busy = false;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                radio_app_set_detail("等待网络");
                xSemaphoreGive(s_lock);
            }
            LOGI("network -> %s", ok ? "up" : "down");
        }

        /* 2. 休闲定时 */
        if (s_state.sleep_left_ms > 0) {
            /* 只在真的在播时才扣时间，暂停期间不计时 */
            if (radio_audio_wanted()) {
                if (s_state.sleep_left_ms > RADIO_APP_TICK_MS) {
                    s_state.sleep_left_ms -= RADIO_APP_TICK_MS;
                } else {
                    s_state.sleep_left_ms = 0;
                    radio_app_stop();
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    radio_app_set_detail("定时播放结束");
                    xSemaphoreGive(s_lock);
                }
            }
        }

        /* 3. 电台页被别的页面压下去了就拉回来（云端推的二维码页、被 nav 回主页…），
         *    但闹钟 / OTA 确认 / 小应用这类页面必须让它先执行。 */
        if (s_state.foreground) {
            if (radio_ui_is_visible()) {
                s_state.nav_confirmed = true;
            } else if (radio_app_foreign_page_wins(radio_ui_top_id())) {
                s_state.nav_confirmed = false;
            } else {
                if (s_state.nav_confirmed) {
                    s_state.nav_confirmed = false;
                    LOGI("radio page covered, restoring");
                }
                const uint32_t now = radio_app_now_ms();
                if ((uint32_t)(now - s_state.last_nav_ms) >= RADIO_NAV_RETRY_MS) {
                    s_state.last_nav_ms = now;
                    lv_async_call(radio_app_nav_show_worker, NULL);
                }
            }
        }

#ifdef CONFIG_RADIO_AUTO_START
        /* 4. 开机自动进电台页 */
        if (s_state.autostart_pending &&
            (int32_t)(radio_app_now_ms() - s_state.autostart_at_ms) >= 0) {
            s_state.autostart_pending = false;
            if (!s_state.foreground) {
                LOGI("autostart radio page");
                radio_app_open();
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(RADIO_APP_TICK_MS));
    }
}

/* ==================== 渲染侧取数 ==================== */

bool radio_app_get_model(radio_ui_model_t *out)
{
    if (out == NULL || !s_state.inited || s_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->page = s_state.page;
    out->menu_index = s_state.menu_index;
    out->list_index = s_state.list_index;
    out->city_index = s_state.city_index;
    out->sleep_choice = s_state.sleep_choice;
    out->network_ok = s_state.net_ok ? 1u : 0u;
    out->busy = s_state.busy ? 1u : 0u;
    out->playback = (uint8_t)radio_audio_get_state();
    out->station_index = s_state.index;
    out->station_count = s_state.count;
    out->sleep_left_ms = s_state.sleep_left_ms;
    snprintf(out->source, sizeof(out->source), "%s", s_state.source);
    snprintf(out->detail, sizeof(out->detail), "%s", s_state.detail);
    const radio_station_t *station = radio_app_station_locked(s_state.index);
    if (station != NULL) {
        snprintf(out->station_name, sizeof(out->station_name), "%s", station->name);
        snprintf(out->station_full, sizeof(out->station_full), "%s", station->full);
        snprintf(out->station_desc, sizeof(out->station_desc), "%s", station->desc);
        out->freq_decihz = station->freq_decihz;
    }

    xSemaphoreGive(s_lock);

    out->volume = (uint8_t)service_volume_get();
    if (service_volume_get() < 0) {
        out->volume = 0;
    }
    return true;
}

bool radio_app_get_station(uint8_t index, radio_station_t *out)
{
    bool found = false;

    if (out == NULL || s_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    if (index < s_state.count) {
        *out = s_state.list[index];
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}

uint8_t radio_app_city_count(void)
{
    return (uint8_t)radio_catalog_city_count();
}

const char *radio_app_city_name(uint8_t index)
{
    const radio_city_t *city = radio_catalog_city(index);
    return (city != NULL) ? city->display : "";
}

uint8_t radio_app_sleep_choice_count(void)
{
    return (uint8_t)(sizeof(s_sleep_choices) / sizeof(s_sleep_choices[0]));
}

const char *radio_app_sleep_text(uint8_t choice)
{
    static char text[16];

    if (choice >= radio_app_sleep_choice_count()) {
        choice = 0;
    }
    if (s_sleep_choices[choice] == 0) {
        return "关闭";
    }
    snprintf(text, sizeof(text), "%u 分钟", (unsigned)s_sleep_choices[choice]);
    return text;
}

/* ==================== 调试命令 ==================== */

/* 现场验证入口（不依赖按键）：
 *   radio             / radio stat   打状态
 *   radio key <动作>                模拟一次按键（click/double/triple/quad/five/long）
 *   radio show|hide                 直接进/出电台页
 *   radio next|play|pause           换台 / 播放 / 暂停
 *   radio city <n>                   按城市表索引查一次目录
 *   radio list                       打印当前台单
 */
static int radio_cmd_key(const char *name)
{
    static const struct {
        const char *name;
        int action;
    } map[] = {
        {"click", VOICE_MSG_BUTTON_ACTION_CLICK},
        {"double", VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK},
        {"triple", VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK},
        {"quad", VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK},
        {"five", VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK},
        {"long", VOICE_MSG_BUTTON_ACTION_LONG_HOLD},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcmp(name, map[i].name) == 0) {
            const bool consumed = radio_app_intercept_action(map[i].action);
            LOGI("key %s -> %s", name, consumed ? "radio" : "system");
            return 0;
        }
    }
    LOGE("unknown key: %s", name);
    return -1;
}

static int radio_cmd_stat(void)
{
    radio_ui_model_t model;
    Shell *shell = shellGetCurrent();

    if (!radio_app_get_model(&model)) {
        if (shell != NULL) {
            shellPrint(shell, "radio: model busy\n");
        }
        return -1;
    }
    if (shell == NULL) {
        return 0;
    }
    shellPrint(shell, "radio: fg=%u page=%u count=%u index=%u net=%u busy=%u pb=%u vol=%u\n",
               (unsigned)s_state.foreground, (unsigned)model.page,
               (unsigned)model.station_count, (unsigned)model.station_index + 1u,
               (unsigned)model.network_ok, (unsigned)model.busy, (unsigned)model.playback,
               (unsigned)model.volume);
    shellPrint(shell, "  station=%s freq=%u source=%s sleep=%u detail=%s\n",
               model.station_name, (unsigned)model.freq_decihz, model.source,
               (unsigned)model.sleep_choice,
               (model.detail[0] != '\0') ? model.detail : "-");
    shellPrint(shell, "  full=%s\n  meta=%s\n", model.station_full, model.station_desc);
    shellPrint(shell, "  wanted=%u paused=%u auto=%u played_gen=%u url_gen=%u\n",
               (unsigned)radio_audio_wanted(), (unsigned)s_state.user_paused,
               (unsigned)s_state.auto_queried, (unsigned)radio_audio_debug_generation(0),
               (unsigned)radio_audio_debug_generation(1));
    return 0;
}

static int radio_cmd_list(void)
{
    Shell *shell = shellGetCurrent();

    for (uint8_t i = 0; i < RADIO_MAX_STATIONS; ++i) {
        radio_station_t station;
        if (!radio_app_get_station(i, &station)) {
            break;
        }
        if (shell != NULL) {
            shellPrint(shell, "[%u] %s | %s | %u | %s | %s | %s\n", (unsigned)i + 1u,
                       station.name, station.url, (unsigned)station.freq_decihz, station.desc,
                       station.lang, station.full);
        } else {
            LOGI("[%u] %s | %s | %u | %s | %s | %s", (unsigned)i + 1u, station.name,
                 station.url, (unsigned)station.freq_decihz, station.desc, station.lang,
                 station.full);
        }
    }
    return 0;
}

static int radio_cmd(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "stat";

    if (strcmp(sub, "stat") == 0) {
        return radio_cmd_stat();
    }
    if (strcmp(sub, "list") == 0) {
        return radio_cmd_list();
    }
    if (strcmp(sub, "key") == 0 && argc >= 3) {
        return radio_cmd_key(argv[2]);
    }
    if (strcmp(sub, "show") == 0) {
        (void)radio_app_intercept_action(VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK);
        return 0;
    }
    if (strcmp(sub, "hide") == 0) {
        radio_app_close();
        return 0;
    }
    if (strcmp(sub, "next") == 0) {
        radio_app_next();
        return 0;
    }
    if (strcmp(sub, "play") == 0) {
        radio_app_play_current();
        return 0;
    }
    if (strcmp(sub, "pause") == 0) {
        radio_app_stop();
        return 0;
    }
    if (strcmp(sub, "city") == 0 && argc >= 3) {
        const int index = atoi(argv[2]);
        if (index < 0 || index >= (int)radio_app_city_count()) {
            LOGE("city index out of range (0..%d)", (int)radio_app_city_count() - 1);
            return -1;
        }
        radio_app_start_city_query((uint8_t)index);
        return 0;
    }
    LOGE("usage: radio [stat|list|key <name>|show|hide|next|play|pause|city <n>]");
    return -1;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 radio, radio_cmd,
                 radio stat|list|key <name>|show|hide|next|play|pause|city <n>);

/* ==================== 初始化 ==================== */


static void radio_app_restore_kv(void)
{
    char *value = NULL;
    int city = 0;

    if (lisa_kv_get_string(RADIO_KV_KEY_STATION, &value) == 0 && value != NULL) {
        snprintf(s_state.remembered, sizeof(s_state.remembered), "%s", value);
        lisa_kv_free(value);
        LOGI("restored station '%s'", s_state.remembered);
    }
    if (lisa_kv_get_int(RADIO_KV_KEY_CITY, &city) == 0) {
        if (city < 0 || city >= radio_app_city_count()) {
            city = 0;
        }
        s_state.city_index = (uint8_t)city;
    }
}

int radio_app_init(void)
{
    if (s_state.inited) {
        return 0;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        LOGE("failed to create state lock");
        return -1;
    }

    s_state.page = RADIO_PAGE_PLAYER;
    s_state.net_ok = false;
    radio_app_set_source(radio_catalog_builtin_source());

    radio_app_restore_kv();
    radio_app_install_builtin();

    if (radio_ui_init() != 0) {
        LOGE("radio_ui_init failed");
        return -1;
    }
    if (radio_net_init(radio_app_on_net) != 0) {
        LOGE("radio_net_init failed");
        return -1;
    }
    if (radio_audio_init() != 0) {
        LOGE("radio_audio_init failed");
        return -1;
    }
    radio_audio_set_fail_cb(radio_app_on_player_failed);

    s_state.inited = true;

#ifdef CONFIG_RADIO_AUTO_START
    /* 只有配过网的设备才自动进电台页：从没配网的设备必须停在助手主页，
     * 否则用户看不到扫码绑定 / 配网引导，也不好进 BLE 配网。 */
    if (sys_wifi_has_ap()) {
        s_state.autostart_pending = true;
        s_state.autostart_at_ms = radio_app_now_ms() + RADIO_AUTOSTART_DELAY_MS;
        LOGI("radio will autostart in %u ms", (unsigned)RADIO_AUTOSTART_DELAY_MS);
    } else {
        LOGW("no saved wifi, skip radio autostart");
    }
#endif

    if (xTaskCreate(radio_app_task, "radio.app", RADIO_APP_TASK_STACK, NULL,
                    RADIO_APP_TASK_PRIO, &s_task) != pdPASS) {
        LOGE("failed to create app task");
        s_task = NULL;
        s_state.inited = false;
        return -1;
    }

    LOGI("radio ready (%u builtin stations)", (unsigned)s_state.count);
    return 0;
}
