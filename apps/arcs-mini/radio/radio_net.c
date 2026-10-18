/*
 * 电台目录的网络层实现，见 radio_net.h 的设计说明。
 *
 * HTTP 走 lisa_http（同步、整包读 body）。目录响应约 15–30 KB，
 * body 缓冲由 lisa_http 内部从堆里分配，解析完立刻释放。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define TAG "radio.net"
#include "lisa_log.h"

#include "cJSON.h"
#include "lisa_http.h"
#include "sysutils.h"

#include "radio_catalog.h"
#include "radio_net.h"

/* ==================== 配置 ==================== */

#define RADIO_NET_TASK_STACK 6144
#define RADIO_NET_TASK_PRIO  4

/* lisa_http 的 timeout 单位是秒（直接传给 HTTPClientSendRequest 的 nTimeout） */
#define RADIO_NET_LOCATION_TIMEOUT_S  8
#define RADIO_NET_DIRECTORY_TIMEOUT_S 15

#define RADIO_LOCATION_URL \
    "http://ip-api.com/json/?fields=status,countryCode,regionName,city"

/* ==================== 状态 ==================== */

typedef enum {
    RADIO_REQ_NONE = 0,
    RADIO_REQ_AUTO,
    RADIO_REQ_CITY,
} radio_req_kind_t;

typedef struct {
    radio_req_kind_t kind;
    uint32_t generation;
    char city[RADIO_CITY_MAX];
    char display[24];
} radio_net_req_t;

typedef struct {
    cJSON *json;
} radio_json_sink_t;

static radio_net_cb_t s_cb;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_signal;
static radio_net_req_t s_req;
static uint32_t s_generation;
static volatile bool s_busy;
static TaskHandle_t s_task;

/* 台单缓冲：约 3.7 KB，SRAM 只剩十几 KB，必须放 PSRAM */
static radio_station_t s_stations[RADIO_MAX_STATIONS] __psram_bss__;

/* ==================== 小工具 ==================== */

static bool radio_net_request_current(uint32_t generation)
{
    bool current;

    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    current = (generation == s_generation);
    xSemaphoreGive(s_lock);
    return current;
}

static void radio_net_reply_failed(uint32_t generation, const radio_location_t *location,
                                   const char *source, const char *detail)
{
    if (s_cb != NULL && radio_net_request_current(generation)) {
        s_cb(RADIO_NET_EVENT_FAILED, NULL, 0, location, source, detail);
    }
}

/* lisa_http 的 on_data 回调：整包 body 一次性给我们，直接解析成 cJSON。 */
static void radio_net_json_on_data(lisa_http_data_t *data)
{
    radio_json_sink_t *sink = (radio_json_sink_t *)data->user;

    if (sink == NULL || data->buf == NULL || data->len <= 0) {
        return;
    }
    if (sink->json != NULL) {
        cJSON_Delete(sink->json);
        sink->json = NULL;
    }
    sink->json = cJSON_ParseWithLength((const char *)data->buf, (size_t)data->len);
}

/* 同步 GET 一个 JSON；失败返回 NULL。调用方负责 cJSON_Delete。 */
static cJSON *radio_net_get_json(const char *url, uint32_t timeout_s)
{
    radio_json_sink_t sink = {0};
    lisa_http_request_t req;

    memset(&req, 0, sizeof(req));
    req.method = LISA_HTTP_GET;
    req.url = (uint8_t *)url;
    req.timeout = timeout_s;
    req.headers = NULL; /* 不需要额外请求头，HTTP 客户端会自己补 Host */
    req.on_data = radio_net_json_on_data;
    req.user = &sink;

    lisa_http_t *http = lisa_http_init(&req);
    if (http == NULL) {
        LOGW("http init failed: %s", url);
        return NULL;
    }

    const lisa_http_err_e err = lisa_http_perform(http);
    lisa_http_cleanup(http);

    if (err != LISA_HTTP_OK) {
        LOGW("http perform failed(%d): %s", (int)err, url);
        cJSON_Delete(sink.json);
        return NULL;
    }
    return sink.json;
}

/* 用一条 URL 查目录并把结果解析到 s_stations。返回条数。 */
static size_t radio_net_query_directory(const char *url, const radio_location_t *location,
                                       const char *city_display)
{
    cJSON *json = radio_net_get_json(url, RADIO_NET_DIRECTORY_TIMEOUT_S);
    size_t count;

    if (json == NULL) {
        return 0;
    }
    count = radio_catalog_parse_stations(json, location, city_display,
                                         s_stations, RADIO_MAX_STATIONS);
    cJSON_Delete(json);
    return count;
}

/* ==================== 两条查询流程 ==================== */

static void radio_net_run_auto(uint32_t generation)
{
    radio_location_t location;
    char url[600];
    char source[RADIO_CITY_MAX];
    size_t count = 0;
    const char *city_query = NULL;

    memset(&location, 0, sizeof(location));

    cJSON *location_json = radio_net_get_json(RADIO_LOCATION_URL,
                                             RADIO_NET_LOCATION_TIMEOUT_S);
    if (location_json == NULL) {
        radio_net_reply_failed(generation, NULL, NULL, "定位请求失败");
        return;
    }
    const int parsed = radio_catalog_parse_location(location_json, &location);
    cJSON_Delete(location_json);
    if (parsed != 0) {
        radio_net_reply_failed(generation, NULL, NULL, "定位失败");
        return;
    }
    if (!radio_net_request_current(generation)) {
        return;
    }
    LOGI("located: city=%s region=%s country=%s", location.city, location.region,
         location.country);

    /* 城市名优先换中文（radio-browser 的中文条目更好找）；非 CN 直接用英文城市名 */
    const char *translated = radio_catalog_translate_city(location.country, location.city);
    if (translated != NULL) {
        city_query = translated;
    } else if (strcmp(location.country, "CN") != 0 && location.city[0] != '\0') {
        city_query = location.city;
    }

    if (city_query != NULL && city_query[0] != '\0') {
        snprintf(source, sizeof(source), "%s", city_query);
        if (radio_catalog_build_name_query(location.country, city_query, url, sizeof(url)) == 0) {
            count = radio_net_query_directory(url, &location, source);
        }
        LOGI("city '%s' -> %u stations", source, (unsigned)count);
    }

    if (count == 0 && location.region[0] != '\0' &&
        radio_net_request_current(generation)) {
        snprintf(source, sizeof(source), "%s", location.region);
        if (radio_catalog_build_region_query(location.country, location.region, url,
                                            sizeof(url)) == 0) {
            count = radio_net_query_directory(url, &location, source);
        }
        LOGI("region '%s' -> %u stations", source, (unsigned)count);
    }

    if (!radio_net_request_current(generation)) {
        return;
    }
    if (count == 0) {
        radio_net_reply_failed(generation, &location, NULL, "没有可用电台");
        return;
    }
    if (s_cb != NULL) {
        s_cb(RADIO_NET_EVENT_STATIONS, s_stations, (uint8_t)count, &location, source, NULL);
    }
}

static void radio_net_run_city(uint32_t generation, const char *city, const char *display)
{
    radio_location_t location;
    char url[600];
    size_t count;

    memset(&location, 0, sizeof(location));
    snprintf(location.country, sizeof(location.country), "CN");
    snprintf(location.city, sizeof(location.city), "%s", city);
    snprintf(location.region, sizeof(location.region), "%s", city);

    if (radio_catalog_build_name_query("CN", city, url, sizeof(url)) != 0) {
        radio_net_reply_failed(generation, &location, display, "城市名过长");
        return;
    }

    count = radio_net_query_directory(url, &location, display);
    LOGI("manual city '%s' -> %u stations", display, (unsigned)count);

    if (!radio_net_request_current(generation)) {
        return;
    }
    if (count == 0) {
        radio_net_reply_failed(generation, &location, display, "没有可用电台");
        return;
    }
    if (s_cb != NULL) {
        s_cb(RADIO_NET_EVENT_STATIONS, s_stations, (uint8_t)count, &location, display, NULL);
    }
}

/* ==================== 任务 ==================== */

static void radio_net_task(void *argument)
{
    (void)argument;

    for (;;) {
        if (xSemaphoreTake(s_signal, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        radio_net_req_t request = s_req;
        s_req.kind = RADIO_REQ_NONE;
        s_busy = true;
        xSemaphoreGive(s_lock);

        if (request.kind == RADIO_REQ_NONE ||
            !radio_net_request_current(request.generation)) {
            /* 请求在排队期间被取消：直接丢掉，不要白跑一次 HTTP */
            s_busy = false;
            continue;
        }

        if (request.kind == RADIO_REQ_AUTO) {
            radio_net_run_auto(request.generation);
        } else if (request.kind == RADIO_REQ_CITY) {
            radio_net_run_city(request.generation, request.city, request.display);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_busy = false;
        xSemaphoreGive(s_lock);
    }
}

/* ==================== 对外接口 ==================== */

int radio_net_init(radio_net_cb_t cb)
{
    if (s_task != NULL) {
        s_cb = cb;
        return 0;
    }

    s_cb = cb;
    s_lock = xSemaphoreCreateMutex();
    s_signal = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_signal == NULL) {
        LOGE("failed to create net sync objects");
        return -1;
    }
    memset(&s_req, 0, sizeof(s_req));
    s_generation = 0;
    s_busy = false;

    if (xTaskCreate(radio_net_task, "radio.net", RADIO_NET_TASK_STACK, NULL,
                    RADIO_NET_TASK_PRIO, &s_task) != pdPASS) {
        LOGE("failed to create net task");
        s_task = NULL;
        return -1;
    }
    return 0;
}

static int radio_net_submit(radio_req_kind_t kind, const char *city, const char *display)
{
    if (s_task == NULL || s_signal == NULL || s_lock == NULL) {
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_generation++;
    memset(&s_req, 0, sizeof(s_req));
    s_req.kind = kind;
    s_req.generation = s_generation;
    if (city != NULL) {
        snprintf(s_req.city, sizeof(s_req.city), "%s", city);
    }
    if (display != NULL) {
        snprintf(s_req.display, sizeof(s_req.display), "%s", display);
    }
    xSemaphoreGive(s_lock);

    /* 覆盖式队列：旧令牌没被消费也无所谓，任务醒来会读最新请求 */
    (void)xSemaphoreGive(s_signal);
    return 0;
}

int radio_net_request_auto(void)
{
    return radio_net_submit(RADIO_REQ_AUTO, NULL, NULL);
}

int radio_net_request_city(const char *city, const char *display)
{
    if (city == NULL || city[0] == '\0') {
        return -1;
    }
    return radio_net_submit(RADIO_REQ_CITY, city, (display != NULL) ? display : city);
}

void radio_net_cancel(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_generation++;
    s_req.kind = RADIO_REQ_NONE;
    xSemaphoreGive(s_lock);
}

bool radio_net_busy(void)
{
    bool busy;

    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    busy = s_busy || (s_req.kind != RADIO_REQ_NONE);
    xSemaphoreGive(s_lock);
    return busy;
}
