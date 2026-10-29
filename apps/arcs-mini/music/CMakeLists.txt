listenai_library_named(app-music)

listenai_library_sources(
    music_http.c
    music_netease.c
    music_app.c
)

# 播放器实例（music_player）来自 src/category/comm/voice_player_comm.c；
# shell.h 来自 letter-shell；include 路径与 radio 保持一致的写法。
listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_TOP_DIR}/src/category/comm
    ${PROJECT_TOP_DIR}/src/system
    ${PROJECT_TOP_DIR}/src/middleware/system
)
