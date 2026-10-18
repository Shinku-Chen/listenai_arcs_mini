/*
 * 电台目录的网络层：异步任务 + lisa_http + cJSON。
 *
 * 设计要点：
 *  - 只有一条「当前请求」，新的请求直接覆盖旧请求（用 generation 丢弃过期结果），
 *    因为界面上同时只会有一个城市在查；
 *  - 全部阻塞操作（DNS / TCP / TLS / 读 body / 解析 JSON）都在本任务里做，
 *    回调也在本任务上下文里执行，调用方不要在回调里做阻塞动作；
 *  - lisa_http_perform() 不暴露 HTTP 状态码，失败只能靠「解析不出东西」判断，
 *    因此解析结果为空也算失败，交给上层回退到内置台单。
 */
#ifndef __RADIO_NET_H__
#define __RADIO_NET_H__

#include <stdbool.h>
#include <stdint.h>

#include "radio_catalog.h"

typedef enum {
    RADIO_NET_EVENT_STATIONS = 0, /* 查到台单（stations/count/source 有效） */
    RADIO_NET_EVENT_FAILED,       /* 没查到，detail 给原因 */
} radio_net_event_t;

typedef void (*radio_net_cb_t)(radio_net_event_t event,
                               const radio_station_t *stations, uint8_t count,
                               const radio_location_t *location,
                               const char *source, const char *detail);

/* 启动网络任务并注册回调（回调只在任务上下文里被调用一次）。 */
int radio_net_init(radio_net_cb_t cb);

/* 自动定位：ip-api 定城市 → 按城市查目录；城市查不到再按省/州查一次。 */
int radio_net_request_auto(void);

/* 手选城市：直接用中文城市名查目录，不做定位。 */
int radio_net_request_city(const char *city, const char *display);

/* 丢弃当前请求的结果（请求本身跑完就结束）。 */
void radio_net_cancel(void);

/* 是否有请求在跑（含排队）。 */
bool radio_net_busy(void);

#endif /* __RADIO_NET_H__ */
