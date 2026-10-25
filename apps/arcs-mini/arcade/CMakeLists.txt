listenai_library_named(app-arcade)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- 内置小游戏「今天吃啥」的帧素材 ------------------------------------------
# assets/*.bin 是 tools/prepare_eat_what.py 裁好的索引帧序列（160x160，每帧
# 512B RGB565 调色板 + 25600B 索引，41 帧共约 1.02MB）。这里在配置阶段把它
# 转成一个 C 数组参与编译，固件里就不用再搞一套文件系统读取。
set(ARCADE_EAT_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)
set(ARCADE_EAT_C ${CMAKE_CURRENT_BINARY_DIR}/generated/arcade_eat_assets.c)
set(ARCADE_EAT_H ${CMAKE_CURRENT_BINARY_DIR}/generated/arcade_eat_assets.h)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)

execute_process(
    COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_eat_what_assets.py
            ${ARCADE_EAT_ASSET_DIR}/eat_what_g1.bin
            ${ARCADE_EAT_ASSET_DIR}/eat_what_g2.bin
            ${ARCADE_EAT_C} ${ARCADE_EAT_H}
    RESULT_VARIABLE ARCADE_EAT_GEN_RET
)

if(NOT ARCADE_EAT_GEN_RET EQUAL 0)
    message(FATAL_ERROR "arcade: eat-what assets generation failed")
endif()

listenai_include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)
listenai_library_sources(${ARCADE_EAT_C})

# ============================================================================
# 两个构建形态
#
#   CONFIG_ARCADE_APP_EATONLY=n（默认，全功能）：
#     ROM 列表 + GB/GBC 模拟器 + 小恐龙 + 跳一跳 + 今天吃啥
#   CONFIG_ARCADE_APP_EATONLY=y（单应用）：
#     只有「今天吃啥」，开机直达；模拟器 / ROM 扫描 / 存档 / 其他小游戏
#     的源码**不参与编译**（源码留在仓库里，全功能构建照旧能编）
# ============================================================================
if(CONFIG_ARCADE_APP_EATONLY)
    listenai_library_sources(
        arcade_eat_app.c
        arcade_eat.c
        arcade_gfx.c
        arcade_video.c
    )

    message(STATUS "arcade: single-app build (eat-what only)")
else()
    listenai_library_sources(
        arcade_app.c
        arcade_audio.c
        arcade_core.c
        arcade_dino.c
        arcade_eat.c
        arcade_gfx.c
        arcade_input.c
        arcade_jump.c
        arcade_rom.c
        arcade_save.c
        arcade_video.c
        arcade_ui.c
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

        # VRAM/WRAM 是否搬进内部 SRAM 由 Kconfig（CONFIG_ARCADE_GB_STATE_IN_SRAM）
        # 决定，emulator.c 里直接看 autoconf.h 的宏，不需要额外的编译选项。
    endif()

    # ---- 内置演示 ROM ----------------------------------------------------------
    # third_party/gb-test-roms 是开发期下载的第三方测试 ROM，不进仓库。
    # 存在时把它编进固件，这样没有 TF 卡也能验证「模拟器能跑」。
    set(ARCADE_DEMO_ROM_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gb-test-roms/cpu_instrs/cpu_instrs.gb)
    set(ARCADE_DEMO_ROM_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated/arcade_demo_rom.c)

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
endif()
