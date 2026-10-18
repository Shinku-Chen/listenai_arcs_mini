listenai_library_named(app-radio)

listenai_library_sources(
    radio_catalog.c
    radio_net.c
    radio_player.c
    radio_ui.c
    radio_app.c
)

# 界面/字体/服务头文件的路径：上游 master 的 apps-ui 只把 PUBLIC 头路径传给
# 直接链接 lisa_ui_app 的 target，这里显式补齐（与 intercom 的做法一致）。
listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_TOP_DIR}/apps-ui
    ${PROJECT_TOP_DIR}/apps-ui/assets/fonts
    ${PROJECT_TOP_DIR}/apps/arcs-mini/services
    ${PROJECT_TOP_DIR}/src/system
    ${PROJECT_TOP_DIR}/src/middleware/system
)
