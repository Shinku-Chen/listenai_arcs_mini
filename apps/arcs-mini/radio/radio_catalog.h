/*
 * 网络电台的台单模型、频率解析与在线目录（移植自 leo-radio 的 radio_catalog.cc）
 *
 * 纯数据层，不依赖 LVGL / 播放器，方便单独测试：
 *  - 内置台单（leo-radio 的 kFallbackStations 原样搬过来）；
 *  - 城市表（自动定位 + 7 个城市，与 leo-radio 的 kCityChoices 一致）；
 *  - 电台名里的 FM 频率解析（87.0–108.0 MHz，单位 0.1MHz）；
 *  - radio-browser.info 目录查询 URL 构造与响应解析；
 *  - ip-api.com 定位响应解析。
 */
#ifndef __RADIO_CATALOG_H__
#define __RADIO_CATALOG_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define RADIO_MAX_STATIONS 10
#define RADIO_NAME_MAX     48
#define RADIO_FULL_MAX     72
#define RADIO_DESC_MAX     64
#define RADIO_URL_MAX      256
#define RADIO_LANG_MAX     12

/* 城市名 / 地区名 / 国家码的最大长度（含结尾 NUL） */
#define RADIO_CITY_MAX    40
#define RADIO_REGION_MAX  40
#define RADIO_COUNTRY_MAX 4

/* 一个电台。freq_decihz 为 0 表示目录里没有可用频率，
 * 界面上按序号把指针铺在刻度上，而不是回到 0。
 *
 * name —— 屏上短名（目录名太长时提炼成“城市 FM94.7”），用于台单与 KV 记忆
 * full —— 目录原名（可能在长屏上滚播），比短名信息更多
 * desc —— 副标题：流派标签（翻中文）+ 编码 + 码率
 * lang —— 语言（翻中文，认不出就留空） */
typedef struct {
    char name[RADIO_NAME_MAX];
    char full[RADIO_FULL_MAX];
    char desc[RADIO_DESC_MAX];
    char lang[RADIO_LANG_MAX];
    char url[RADIO_URL_MAX];
    uint16_t freq_decihz;
} radio_station_t;

typedef struct {
    char city[RADIO_CITY_MAX];
    char region[RADIO_REGION_MAX];
    char country[RADIO_COUNTRY_MAX];
} radio_location_t;

typedef struct {
    const char *query;   /* 目录查询用的城市名；NULL 表示自动定位 */
    const char *display; /* 界面显示名 */
} radio_city_t;

/* ==================== 频率频段（0.1MHz 单位） ==================== */

/* 中国调频广播占 87.0–108.0 MHz，刻度盘按这区间铺 */
#define RADIO_BAND_LOW_DECIHZ  870
#define RADIO_BAND_HIGH_DECIHZ 1080

/* ==================== 城市表 ==================== */

size_t radio_catalog_city_count(void);
const radio_city_t *radio_catalog_city(size_t index);

/* ==================== 内置台单 ==================== */

/* 把内置台单写进 out，返回写入条数。 */
size_t radio_catalog_builtin(radio_station_t *out, size_t capacity);
const char *radio_catalog_builtin_source(void);

/* ==================== 频率 ==================== */

/* 从电台名里解析真实频率；解析不出返回 0。 */
uint16_t radio_catalog_parse_frequency(const char *name);

/* 把 0.1MHz 单位的频率格式化成 "94.7"；无效频率写空串。 */
void radio_catalog_format_frequency(uint16_t decihz, char *out, size_t capacity);

/* ==================== 目录 URL ==================== */

/* country 传 NULL/空串时按 "CN" 处理。返回值 0 成功，-1 参数或长度错误。 */
int radio_catalog_build_name_query(const char *country, const char *name,
                                   char *out, size_t capacity);
int radio_catalog_build_region_query(const char *country, const char *region,
                                     char *out, size_t capacity);

/* ip-api 的英文城市名换成中文；非 CN 或不在表里返回 NULL。 */
const char *radio_catalog_translate_city(const char *country, const char *english_city);

/* ==================== 响应解析 ==================== */

/* 解析 ip-api.com 的定位响应（调用方持有 root 的所有权）；成功返回 0 并填充 out。 */
int radio_catalog_parse_location(const cJSON *root, radio_location_t *out);

/* 解析 radio-browser.info 的 /stations/search 响应（调用方持有 root 的所有权）。
 * city_display 只用于给没有可读名字的台生成 "城市 FM xx.x" 这样的显示名。
 * 返回解析出来的可用电台数量（已去重、已过滤 HLS/宕机/非 MP3）。 */
size_t radio_catalog_parse_stations(const cJSON *root,
                                    const radio_location_t *location,
                                    const char *city_display,
                                    radio_station_t *out, size_t capacity);

#endif /* __RADIO_CATALOG_H__ */
