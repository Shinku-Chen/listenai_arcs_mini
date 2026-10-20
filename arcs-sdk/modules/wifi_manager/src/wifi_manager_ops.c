/**
 * Copyright (c) 2025, LISTENAI
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_manager/wifi_manager.h"
#include "wifi_manager/wifi_manager_ops.h"

#if (CONFIG_WIFI_MANAGER_MEM_HEAP_CAPS)
#include "wifi_manager_mem_ops_heap_caps.h"
#endif

#if (CONFIG_WIFI_MANAGER_OS_LISA)
#include "wifi_manager_os_ops_lisa.h"
#endif

#if (CONFIG_WIFI_MANAGER_WIFI_DEV_ARCS) && !defined(WIFI_MANAGER_WIFI_DEV_NONE)
#include "wifi_arcs.h"
#endif

static wifi_mgr_ops_t init_param;

static wifi_manager_mem_ops_t *wifi_mgr_init_mem_ops(void)
{
#if (CONFIG_WIFI_MANAGER_MEM_HEAP_CAPS)
    return wifi_manager_heap_caps_mem_ops_get();
#else
#error "CONFIG_WIFI_MANAGER_MEM_HEAP_CAPS is not defined"
#endif
}

static wifi_manager_os_ops_t *wifi_mgr_init_os_ops(void)
{
#if (CONFIG_WIFI_MANAGER_OS_LISA)
    return wifi_manager_lisa_os_ops_get();
#else
#error "CONFIG_WIFI_MANAGER_OS_LISA is not defined"
#endif
}

static wifi_manager_wifi_ops_t wifi_mgr_no_dev_ops;

static wifi_manager_wifi_ops_t *wifi_mgr_init_wifi_ops(void)
{
#ifdef WIFI_MANAGER_WIFI_DEV_NONE
    /*
     * WiFi 驱动没编进固件（CONFIG_WIFI=n，如纯模拟器固件）：没有设备实现。
     * 返回一份全空的 ops，让上层的 WIFI_MGR_CHECK(ops->wifi_ops->xxx != NULL)
     * 优雅地被 -EINVAL 拦下，而不是空指针崩掉。这种固件本来就不会调用
     * wifi_manager_init()。
     */
    return &wifi_mgr_no_dev_ops;
#elif (CONFIG_WIFI_MANAGER_WIFI_DEV_ARCS)
    return wifi_manager_arcs_wifi_ops_get();
#else
    #error "Other wifi module is not supported"
    return NULL;
#endif
}

wifi_mgr_ops_t* wifi_mgr_ops_get(void)
{
    wifi_manager_mem_ops_t *mem_ops = wifi_mgr_init_mem_ops();
    wifi_manager_os_ops_t *os_ops = wifi_mgr_init_os_ops();
    wifi_manager_wifi_ops_t *wifi_ops = wifi_mgr_init_wifi_ops();

    init_param.mem_ops = mem_ops;
    init_param.os_ops = os_ops;
    init_param.wifi_ops = wifi_ops;

    return &init_param;
}
