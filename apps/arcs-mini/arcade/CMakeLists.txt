listenai_library_named(app-arcade)

listenai_library_sources(
    arcade_app.c
    arcade_core.c
    arcade_gb.c
    arcade_input.c
    arcade_rom.c
    arcade_video.c
    arcade_ui.c
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Peanut-GB
)

# ---- 内置演示 ROM ----------------------------------------------------------
# third_party/gb-test-roms 是开发期下载的第三方测试 ROM，不进仓库。
# 存在时把它编进固件，这样没有 TF 卡也能验证「模拟器能跑」。
set(ARCADE_DEMO_ROM_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gb-test-roms/cpu_instrs/cpu_instrs.gb)
set(ARCADE_DEMO_ROM_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated/arcade_demo_rom.c)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(NOT EXISTS ${ARCADE_DEMO_ROM_SRC})
    set(ARCADE_DEMO_ROM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/arcade_demo_rom.h)
endif()

execute_process(
    COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_demo_rom.py
            ${ARCADE_DEMO_ROM_SRC} ${ARCADE_DEMO_ROM_OUT}
    RESULT_VARIABLE ARCADE_DEMO_GEN_RET
)

if(NOT ARCADE_DEMO_GEN_RET EQUAL 0)
    message(WARNING "arcade: demo ROM generation failed, continuing without it")
endif()

listenai_library_sources(${ARCADE_DEMO_ROM_OUT})
