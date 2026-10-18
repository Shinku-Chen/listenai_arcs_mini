listenai_library_named(aiui_alarm)

listenai_library_sources(
    alarm.c
    alarm_ring.c
    alarm_nvs.c
    alarm_next.c
    alarm_constructor.c
    alarm_time_utils.c
    # 纯电台固件：alarm_api.c（云端闹钟同步，依赖 lschat 的 lsc.h）不参与构建
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(
    ${LISTENAI_CURRENT_LIBRARY}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/private
)
