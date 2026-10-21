/*
 * TF 卡 ROM 列表。
 *
 * 注意：本平台 VFS 的目录遍历（lsfs_opendir + lsfs_readdir）在卡根目录上
 * 并不可靠（会卡死在 readdir 上，整机冻结），所以这里不做目录遍历，改成
 * 「按固定候选路径逐个 fopen 探测」：只要文件放在下面任一候选目录、且文件名
 * 在候选名单里，就能出现在列表里。
 *
 *   目录：/SD:/roms/ 、/SD:/
 *   名单：pock.gbc / pock.gb / pokemon.gbc / pokemon.gb
 *         game.gbc / game.gb / rom.gbc / rom.gb
 *
 * 探测只用 lsfs_open + lsfs_lsize（这条链路已被 `arcade fs` 自检证明稳定）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sysheap.h"
#include "lisa_log.h"
#include "lsfs.h"

#include "arcade_rom.h"
#include "arcade_demo_rom.h"

#define TAG "arcade-rom"

#define ROM_DIR        CONFIG_ARCADE_ROM_DIR
#define ROM_MAX        CONFIG_ARCADE_ROM_MAX
#define ROM_NAME_LEN   64
#define ROM_PATH_LEN   128

/* GB/GBC 卡带最大 8MB，超过这个尺寸肯定不是 ROM，当作探测失败 */
#define ROM_SIZE_LIMIT (8U * 1024U * 1024U)

/* 内置演示 ROM 的虚拟路径，arcade_gb.c 看到它就改用内嵌数据 */
#define ROM_DEMO_PATH  "@demo"

/*
 * 列表最前面固定放一个「内置小游戏」：它是固件原生代码（arcade_jump.c），
 * 不读卡、不占 ROM 名额，所以即使没插 TF 卡、甚至一张 ROM 都没有，
 * 列表也不会是空的。小恐龙（arcade_dino.c）仍然编在固件里，但不占列表
 * 名额，用 shell 命令 `arcade dino` 直接进。
 */
#define ROM_BUILTIN_COUNT  1

static const struct {
    const char *name;
    const char *path;
} k_builtins[ROM_BUILTIN_COUNT] = {
    [ARCADE_BUILTIN_JUMP] = {"跳一跳（内置）", "@jump"},
};

struct arcade_rom_entry {
    char name[ROM_NAME_LEN];
    char path[ROM_PATH_LEN];
    uint32_t size;
};

static struct arcade_rom_entry *s_roms;
static int s_count = -1; /* -1 表示还没扫描过 */

/* 候选目录（顺序即列表顺序） */
static const char *const k_rom_dirs[] = {
    "/SD:/roms/",
    "/SD:/",
};

/* 候选文件名 */
static const char *const k_rom_names[] = {
    "pock.gbc",
    "pock.gb",
    "pokemon.gbc",
    "pokemon.gb",
    "game.gbc",
    "game.gb",
    "rom.gbc",
    "rom.gb",
};

static bool rom_probe_path(const char *path, uint32_t *size_out)
{
    struct lsfs_file_t file;
    off_t size;

    lsfs_file_t_init(&file);
    if (lsfs_open(&file, path, LSFS_O_READ) != 0) {
        return false;
    }

    size = lsfs_lsize(&file);
    lsfs_close(&file);

    if (size <= 0 || (uint32_t)size > ROM_SIZE_LIMIT) {
        return false;
    }

    *size_out = (uint32_t)size;
    return true;
}

static bool rom_collect(void)
{
    int count = 0;

    for (size_t d = 0; d < sizeof(k_rom_dirs) / sizeof(k_rom_dirs[0]); d++) {
        for (size_t n = 0; n < sizeof(k_rom_names) / sizeof(k_rom_names[0]); n++) {
            char path[ROM_PATH_LEN];
            uint32_t size = 0;

            if (count >= ROM_MAX) {
                break;
            }

            snprintf(path, sizeof(path), "%s%s", k_rom_dirs[d], k_rom_names[n]);

            if (!rom_probe_path(path, &size)) {
                continue;
            }

            struct arcade_rom_entry *slot = &s_roms[count];

            memset(slot, 0, sizeof(*slot));
            snprintf(slot->name, ROM_NAME_LEN, "%s", k_rom_names[n]);
            snprintf(slot->path, ROM_PATH_LEN, "%s", path);
            slot->size = size;
            count++;
        }
    }

    s_count = count;
    return true;
}

static void rom_entries_free(void)
{
    if (s_roms != NULL) {
        exram_free(s_roms);
        s_roms = NULL;
    }
    s_count = -1;
}

static bool rom_ensure_table(void)
{
    if (s_roms != NULL) {
        return true;
    }

    s_roms = (struct arcade_rom_entry *)exram_calloc(4, ROM_MAX, sizeof(struct arcade_rom_entry));
    if (s_roms == NULL) {
        LISA_LOGE(TAG, "rom table alloc failed (%d entries)",
                  ROM_MAX * (int)sizeof(struct arcade_rom_entry));
        return false;
    }

    return true;
}

/**
 * 卡里一个 ROM 都没有时，把固件内置的测试 ROM 放进列表，
 * 这样没插 TF 卡也能验证模拟器能跑。
 */
static void rom_add_demo_if_empty(void)
{
    if (s_count != 0 || arcade_demo_rom_size == 0) {
        return;
    }

    if (!rom_ensure_table()) {
        return;
    }

    struct arcade_rom_entry *slot = &s_roms[0];

    memset(slot, 0, sizeof(*slot));
    strncpy(slot->name, "demo-cpu_instrs.gb", ROM_NAME_LEN - 1);
    strncpy(slot->path, ROM_DEMO_PATH, ROM_PATH_LEN - 1);
    slot->size = arcade_demo_rom_size;
    s_count = 1;

    LISA_LOGI(TAG, "no ROM on card, fall back to built-in demo ROM");
}

int arcade_rom_scan(void)
{
    if (s_count >= 0) {
        return s_count + ROM_BUILTIN_COUNT;
    }

    if (!rom_ensure_table()) {
        s_count = 0;
        return s_count + ROM_BUILTIN_COUNT;
    }

    if (!rom_collect()) {
        s_count = 0;
        rom_add_demo_if_empty();
        return s_count + ROM_BUILTIN_COUNT;
    }

    rom_add_demo_if_empty();

    LISA_LOGI(TAG, "found %d ROM(s) (probe dirs %s, %s)", s_count,
              k_rom_dirs[0], k_rom_dirs[1]);
    for (int i = 0; i < s_count; i++) {
        LISA_LOGI(TAG, "  [%02d] %s (%u bytes)", i, s_roms[i].path, (unsigned)s_roms[i].size);
    }

    return s_count + ROM_BUILTIN_COUNT;
}

int arcade_rom_count(void)
{
    int n = s_count > 0 ? s_count : 0;

    return n + ROM_BUILTIN_COUNT;
}

bool arcade_rom_is_builtin(int index)
{
    return index >= 0 && index < ROM_BUILTIN_COUNT;
}

int arcade_rom_builtin_id(int index)
{
    if (!arcade_rom_is_builtin(index)) {
        return -1;
    }

    return index;
}

const char *arcade_rom_name(int index)
{
    if (index < 0 || index >= arcade_rom_count()) {
        return NULL;
    }

    if (arcade_rom_is_builtin(index)) {
        return k_builtins[index].name;
    }

    return s_roms[index - ROM_BUILTIN_COUNT].name;
}

const char *arcade_rom_path(int index)
{
    if (index < 0 || index >= arcade_rom_count()) {
        return NULL;
    }

    if (arcade_rom_is_builtin(index)) {
        return k_builtins[index].path;
    }

    return s_roms[index - ROM_BUILTIN_COUNT].path;
}

uint32_t arcade_rom_size(int index)
{
    if (index < 0 || index >= arcade_rom_count()) {
        return 0;
    }

    if (arcade_rom_is_builtin(index)) {
        return 0;
    }

    return s_roms[index - ROM_BUILTIN_COUNT].size;
}

bool arcade_rom_scanned(void)
{
    return s_count >= 0;
}

void arcade_rom_invalidate(void)
{
    rom_entries_free();
}
