listenai_library_named(app_dis)

listenai_library_sources(
    app_display.c
    lv_fs_net.c
    lv_img_net_loader.c
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

listenai_library_sources_ifdef(CONFIG_LISA_JPEG lv_img_hw_jpeg.c)
