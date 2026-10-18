/*
 * 网络电台应用层：状态、按键语义、台单安装、睡眠定时、KV 持久化。
 *
 * 全局只有一份电台状态（s_state），radio_ui 每 100 ms 通过
 * radio_app_get_model() 取一份快照来渲染，所以这里不需要把任何东西
 * 投递到 LVGL 线程；只有「进/出页」是导航动作，必须走 UI 线程。
 *
 * 入口：四击（电源键）。页内：
 *   单击 播放页=换台 / 列表=下移 / 菜单=下移 / 城市=下移 / 音量=+5
 *   双击 播放页=播放/暂停 / 菜单=确认 / 列表=选台 / 城市=确认 / 音量=返回
 *   三击 返回（播放页直接退出电台）
 *   四击 退出电台
 *   长按 退出电台（并吞掉「长按=关机」，避免在电台页误关机）
 *   五六七击 退出电台并放行给 BLE 配网（无网时重新配网用）
 *   连击 8+ 吞掉（避免误触恢复出厂）
 */
#ifndef __RADIO_APP_H__
#define __RADIO_APP_H__

#include <stdbool.h>
#include <stdint.h>

#include "radio_catalog.h"

#define RADIO_SOURCE_MAX 24
#define RADIO_DETAIL_MAX 40

typedef enum {
    RADIO_PAGE_PLAYER = 0,
    RADIO_PAGE_MENU,
    RADIO_PAGE_LIST,
    RADIO_PAGE_CITY,
    RADIO_PAGE_VOLUME,
    RADIO_PAGE_NETCFG,   /* 蓝牙配网：显示配网二维码 */
} radio_page_t;

typedef enum {
    RADIO_MENU_LIST = 0,
    RADIO_MENU_CITY,
    RADIO_MENU_SLEEP,
    RADIO_MENU_VOLUME,
    RADIO_MENU_NET,     /* 网络：显示连接状态，双击进配网页重新配网 */
    RADIO_MENU_COUNT,
} radio_menu_t;

/* 界面每帧读取的快照 */
typedef struct {
    uint8_t page;        /* radio_page_t */
    uint8_t menu_index;  /* radio_menu_t */
    uint8_t list_index;  /* 台单页选中项 */
    uint8_t city_index;  /* 城市页选中项 */
    uint8_t sleep_choice;
    uint8_t volume;      /* 0-100 */
    uint8_t network_ok;
    uint8_t busy;        /* 正在查目录 */
    uint8_t playback;    /* radio_pb_state_t */
    uint8_t station_index;
    uint8_t station_count;
    uint32_t sleep_left_ms;
    uint16_t freq_decihz;
    char source[RADIO_SOURCE_MAX];
    char detail[RADIO_DETAIL_MAX]; /* 非空时状态行显示它（红色） */
    char station_name[RADIO_NAME_MAX];
    char station_full[RADIO_FULL_MAX];  /* 目录原名（播放页可能滚播） */
    char station_desc[RADIO_DESC_MAX]; /* 副标题：流派 / 编码 / 码率 */
} radio_ui_model_t;

/* 初始化（UI 就绪后调）：注册 nav screen、网络任务、播放器任务，恢复 KV */
int radio_app_init(void);

/* 按键拦截：返回 true 表示这次动作已被电台消费掉 */
bool radio_app_intercept_action(int action);

/* 渲染侧取数；返回 false 表示取不到（持锁超时），调用方跳过本帧 */
bool radio_app_get_model(radio_ui_model_t *out);
bool radio_app_get_station(uint8_t index, radio_station_t *out);

uint8_t radio_app_city_count(void);
const char *radio_app_city_name(uint8_t index);

uint8_t radio_app_sleep_choice_count(void);
const char *radio_app_sleep_text(uint8_t choice);

#endif /* __RADIO_APP_H__ */
