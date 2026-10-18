listenai_include_directories(./)
listenai_library_sources(
    app_datas.c
    voice_player_comm.c
)
# 纯电台固件：voice_music_list.c 是云端歌单（依赖 lschat 的 lsc.h），不参与构建

# 纯电台固件：msgs/ 是云端会话/唤醒事件的接线（依赖 voice_cloud/lschat），不参与构建
