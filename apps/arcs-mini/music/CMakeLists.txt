listenai_library_named(app-music)

listenai_library_sources(
    music_http.c
    music_netease.c
    music_app.c
    music_ui.c
)

# 播放器实例（music_player）来自 src/category/comm/voice_player_comm.c；
# shell.h 来自 letter-shell；apps-ui 的路径给字体/导航/系统时钟用，
# 写法与 radio 保持一致（上游只把 PUBLIC 头路径传给直接链接 lisa_ui_app 的 target）。
listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_TOP_DIR}/src/category/comm
    ${PROJECT_TOP_DIR}/src/system
    ${PROJECT_TOP_DIR}/src/middleware/system
    ${PROJECT_TOP_DIR}/apps-ui
    ${PROJECT_TOP_DIR}/apps-ui/apps/llm
    ${PROJECT_TOP_DIR}/apps-ui/assets/fonts
    ${CMAKE_BINARY_DIR}/generated/assets
)
