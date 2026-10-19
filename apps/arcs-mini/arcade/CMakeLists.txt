listenai_library_named(app-arcade)

listenai_library_sources(
    arcade_app.c
    arcade_audio.c
    arcade_core.c
    arcade_input.c
    arcade_rom.c
    arcade_save.c
    arcade_video.c
    arcade_ui.c
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- GB/GBC 内核 ----------------------------------------------------------
# CONFIG_ARCADE_GB_CORE_PEANUT=y 时用 Peanut-GB（只有 DMG，体积小）；
# 默认用 binjgb（纯 C99 + MIT，CGB 完整：VRAM/WRAM 分 bank、CGB 调色板、
# GDMA/HDMA、双速模式），GBC 游戏才能真正跑起来。
#
# 两个后端实现同一套 arcade_gb.h 接口，上层 arcade_app.c 不需要区分。
if(CONFIG_ARCADE_GB_CORE_PEANUT)
    listenai_library_sources(arcade_gb.c)
    listenai_include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Peanut-GB
    )
else()
    listenai_library_sources(
        arcade_gbc.c
        third_party/binjgb/emulator.c
        third_party/binjgb/common.c
        third_party/binjgb/memory.c
    )
    listenai_include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/binjgb
    )
    # third_party 是 vendored 原版代码，不参与本分支的告警策略。
    # 编译选项：
    #   -O3            全局是 -Os，binjgb 是纯解释器，内联程度直接决定帧率
    #   -DNDEBUG       去掉所有 assert（binjgb 热路径上也有 assert）
    #   -fomit-frame-pointer  全局是 -fno-omit-frame-pointer，解释器少一个寄存器差别很大
    set_source_files_properties(
        third_party/binjgb/emulator.c
        third_party/binjgb/common.c
        third_party/binjgb/memory.c
        PROPERTIES COMPILE_OPTIONS "-w;-O3;-DNDEBUG;-fomit-frame-pointer"
    )
endif()

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
