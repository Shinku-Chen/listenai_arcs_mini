/*
 * 模拟器核心分发层。
 *
 * 目前只有 GB/GBC（Peanut-GB）后端。NES（fceumm）后端接入时，
 * 在这里按扩展名分发，上层 arcade_app.c 不需要改动。
 */
#include <ctype.h>
#include <string.h>

#include "lisa_log.h"

#include "arcade_core.h"
#include "arcade_gb.h"

#define TAG "arcade-core"

static arcade_sys_t s_sys = ARCADE_SYS_GB;

static bool has_suffix(const char *path, const char *suffix)
{
    size_t plen;
    size_t slen;

    if (path == NULL || suffix == NULL) {
        return false;
    }

    plen = strlen(path);
    slen = strlen(suffix);
    if (plen < slen) {
        return false;
    }

    for (size_t i = 0; i < slen; i++) {
        if (tolower((unsigned char)path[plen - slen + i]) !=
            tolower((unsigned char)suffix[i])) {
            return false;
        }
    }

    return true;
}

arcade_sys_t arcade_core_guess_sys(const char *path)
{
    if (has_suffix(path, ".nes")) {
        return ARCADE_SYS_NES;
    }

    return ARCADE_SYS_GB;
}

int arcade_core_start(const char *path)
{
    if (path == NULL) {
        return -1;
    }

    s_sys = arcade_core_guess_sys(path);

    switch (s_sys) {
    case ARCADE_SYS_GB:
        return arcade_gb_start(path);
    case ARCADE_SYS_NES:
        /* NES 后端尚未接入（下一阶段用 libretro-fceumm） */
        LISA_LOGE(TAG, "NES core not implemented yet: %s", path);
        return -1;
    default:
        return -1;
    }
}

void arcade_core_stop(void)
{
    arcade_gb_stop();
}

bool arcade_core_running(void)
{
    return arcade_gb_running();
}

void arcade_core_run_frame(void)
{
    switch (s_sys) {
    case ARCADE_SYS_GB:
        arcade_gb_run_frame();
        break;
    default:
        break;
    }
}

void arcade_core_set_input(uint32_t mask)
{
    switch (s_sys) {
    case ARCADE_SYS_GB:
        arcade_gb_set_input(mask);
        break;
    default:
        break;
    }
}

const char *arcade_core_sys_name(void)
{
    switch (s_sys) {
    case ARCADE_SYS_GB:
        return "GB/GBC";
    case ARCADE_SYS_NES:
        return "NES";
    default:
        return "?";
    }
}
