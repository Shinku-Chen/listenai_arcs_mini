
set(SYSTEM_SOURCES
    powerup.c
    platform.c
    sys_network_manager.c
    remote_logger.c
    sys_network_probe.c
)

# WiFi 驱动被裁掉的固件（如纯模拟器 arcade.conf）不能编译 sys_wifi.c：
# lisa_wifi.h 在没有任何 WiFi 模式宏时会直接 #error，而且它的依赖
# （lisa_wifi / wifi_manager / net_al）此时都不存在。
# 但上层还有几个模块无条件引用 sys_wifi_* 接口，所以换上一份空实现。
if(CONFIG_LISA_WIFI)
    list(APPEND SYSTEM_SOURCES sys_wifi.c)
else()
    list(APPEND SYSTEM_SOURCES sys_wifi_stub.c)
endif()

listenai_library_sources(
    ${SYSTEM_SOURCES}
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)
