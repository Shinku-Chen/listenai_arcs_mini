# =============================================================================
# Voice Assistant Application Build Configuration
# =============================================================================

listenai_library_named(app)

# 让 "utils/xxx.h" 这种 src 根相对 include 能解析（rcmd_router 等在用）
listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Core application source files
add_subdirectory(category)
add_subdirectory(middleware)
add_subdirectory(server)
add_subdirectory(shell)
add_subdirectory(utils)
add_subdirectory(system)
add_subdirectory(framework)
