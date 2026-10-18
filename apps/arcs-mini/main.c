#include "FreeRTOS.h"
#include "task.h"

#define TAG "main"
#include "lisa_log.h"

#include "lv_i18n.h"
#include "lisa_ui_invoke.h"

#include "service_volume.h"
#include "service_brightness.h"
#include "service_button.h"
#include "battery/battery.h"

/*
 * 纯电台固件（leo-radio）入口。
 *
 * 只起四件事：显示 + 按键 + 音量/亮度 + 电池，然后把整个 UI 交给电台页。
 * 语音助手（唤醒词 / 云端会话 / TTS / MCP / 小程序 / 相机）一律不初始化，
 * 对应目标文件也不会被链接进来。
 *
 * WiFi / BLE 配网不在这里：src/system/platform.c 的 SYS_INIT 早于 main() 就已经
 * 完成 lisa_bluetooth_init() + app_ble_netcfg_init() + 音频平台初始化，
 * 配网链路与原来的助手固件完全一致（手机 App 照常发现设备、下发 SSID/密码）。
 */
int main(int argc, char **argv)
{
    extern int boot_watchdog_feed(void);
    (void)argc;
    (void)argv;

    boot_watchdog_feed();

    service_volume_init();
    service_brightness_init();
    service_button_init();
    battery_init();

    /* LVGL + UI 线程：只做显示，不加载助手页面（lisa_ui_app_init 不调用） */
    {
        extern int lisa_ui_lvgl_init(void);
        extern int lisa_ui_lvgl_run(void);

        lv_i18n_init(lv_i18n_language_pack);
        lisa_ui_invoke_init();
        lisa_ui_lvgl_init();
        lisa_ui_lvgl_run();
    }

    /* 电台把自己注册成导航栈的默认页面（整个 UI 就是它）并开始播放 */
    {
        extern int radio_app_init(void);
        if (radio_app_init() != 0) {
            LOGE("radio init failed");
        }
    }

    LOGI("leo-radio ready");

    while (1) {
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return 0;
}
