if(CONFIG_LISA_JPEG)

listenai_library_named(lisa_jpeg)

listenai_library_sources(
    lisa_jpeg.c
    lisa_jpeg_validate.c
    ../../soc/arcs/hal/src/arcs/video/middlewares/jpeg/jpeg_enc_golden/jpeg_enc_htable_golden.c
    ../../soc/arcs/hal/src/arcs/video/middlewares/jpeg/jpeg_enc_golden/jpeg_enc_qtable_golden.c
)

listenai_include_directories(
    ./
)

endif() # CONFIG_LISA_JPEG
