/*
 * WiFi 驱动被裁掉时的 sys_wifi 空实现。
 *
 * 纯模拟器固件（arcade.conf + arcade-sram.conf）把 WiFi 驱动整个从编译里
 * 去掉，内部 SRAM 让给 binjgb 的 VRAM/WRAM。但上层还有几个模块
 * （sys_network_manager / lschat_server / voice_player_network_tone）
 * 无条件引用了 sys_wifi_* 接口，所以这里补一份「永远未联网」的空实现：
 * 链接能过，运行期语义也正确 —— 这个固件本来就不会联网。
 *
 * 由 src/system/CMakeLists.txt 在 CONFIG_LISA_WIFI=n 时编入。
 */

#include <stdbool.h>
#include <stddef.h>

#include "sys_wifi.h"

int sys_wifi_init(void)
{
    return -1;
}

int sys_wifi_start(bool autoconnect)
{
    (void)autoconnect;
    return -1;
}

int sys_wifi_stop(void)
{
    return -1;
}

int sys_wifi_connect(const char *ssid, const char *pwd, const char *bssid)
{
    (void)ssid;
    (void)pwd;
    (void)bssid;
    return -1;
}

sys_wifi_connect_result_t sys_wifi_get_connect_result(void)
{
    return SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP;
}

int sys_wifi_save_ap(const char *ssid, const char *pwd, const char *bssid)
{
    (void)ssid;
    (void)pwd;
    (void)bssid;
    return -1;
}

int sys_wifi_clear_saved_aps(void)
{
    return -1;
}

bool sys_wifi_get_signal_quality(int *rssi)
{
    if (rssi != NULL) {
        *rssi = 0;
    }
    return false;
}

bool sys_wifi_is_ready(void)
{
    return false;
}

bool sys_wifi_is_started(void)
{
    return false;
}

bool sys_wifi_is_connected(void)
{
    return false;
}

bool sys_wifi_has_ap(void)
{
    return false;
}

int sys_wifi_set_standby_power_save(bool enable)
{
    (void)enable;
    return -1;
}

/* src/server/lschat_server/voice_cloud.c 用到的 DNS 轮换接口 */
void sys_wifi_refresh_dnsserver(const char *const dns_srv)
{
    (void)dns_srv;
}
