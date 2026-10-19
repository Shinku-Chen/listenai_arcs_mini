/*
 * TF 卡 ROM 目录扫描。
 *
 * 挂载由 platform.c 在启动时完成；如果启动时没插卡，这里会重新探测并挂载。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "sysheap.h"
#include "lisa_log.h"
#include "lisa_device.h"
#include "lisa_sdmmc.h"
#include "lsfs.h"

#include "arcade_rom.h"
#include "arcade_demo_rom.h"

#define TAG "arcade-rom"

#define ROM_DIR        CONFIG_ARCADE_ROM_DIR
#define ROM_MAX        CONFIG_ARCADE_ROM_MAX
#define ROM_NAME_LEN   64
#define ROM_PATH_LEN   128

/* 只展开一层子目录（ROM 目录下的第一级子目录），再多扫卡时间不可控 */
#define ROM_SUBDIR_DEPTH 1
#define ROM_SUBDIR_MAX   8

/* 内置演示 ROM 的虚拟路径，arcade_gb.c 看到它就改用内嵌数据 */
#define ROM_DEMO_PATH  "@demo"

struct arcade_rom_entry {
    char name[ROM_NAME_LEN];
    char path[ROM_PATH_LEN];
    uint32_t size;
};

extern struct lsfs_mount_t *platform_sd_mount_get(void);

static struct arcade_rom_entry *s_roms;
static int s_count = -1; /* -1 表示还没扫描过 */
static bool s_dir_missing_logged;

static void sdmmc_reset(void)
{
    lisa_device_t *dev = lisa_device_get("sdmmc0");

    if (dev == NULL || dev->priv_data == NULL) {
        return;
    }

    /* lisa_sdmmc_probe() 有 initialized 快速路径，卡拔出重插时必须清零才能重新初始化 */
    struct sdmmc_priv_layout {
        void *mutex;
        bool initialized;
    };
    struct sdmmc_priv_layout *priv = (struct sdmmc_priv_layout *)dev->priv_data;
    priv->initialized = false;
}

static bool rom_dir_accessible(void)
{
    struct lsfs_dir_t dir;

    lsfs_dir_t_init(&dir);
    if (lsfs_opendir(&dir, ROM_DIR) != 0) {
        return false;
    }

    lsfs_closedir(&dir);
    return true;
}

static bool rom_dir_mount(void)
{
    lisa_device_t *dev;
    struct lsfs_mount_t *mp;

    if (rom_dir_accessible()) {
        return true;
    }

    sdmmc_reset();

    dev = lisa_device_get("sdmmc0");
    if (dev == NULL) {
        LISA_LOGE(TAG, "sdmmc0 device not found");
        return false;
    }

    if (lisa_sdmmc_probe(dev) != 0) {
        LISA_LOGW(TAG, "sdmmc probe failed, TF card missing?");
        return false;
    }

    mp = platform_sd_mount_get();
    if (mp == NULL) {
        LISA_LOGE(TAG, "no SD mount point");
        return false;
    }

    if (lsfs_mount(mp) != 0) {
        LISA_LOGW(TAG, "mount %s failed", mp->mnt_point);
        return false;
    }

    return rom_dir_accessible();
}

static bool rom_extension_supported(const char *name)
{
    const char *dot = strrchr(name, '.');

    if (dot == NULL) {
        return false;
    }

    if (strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".gbc") == 0) {
        return true;
    }

    return false;
}

static void rom_entries_free(void)
{
    if (s_roms != NULL) {
        exram_free(s_roms);
        s_roms = NULL;
    }
    s_count = -1;
}

static bool rom_collect_dir(const char *dir_path, const char *prefix, int *count, int depth)
{
    struct lsfs_dir_t dir;
    struct lsfs_dirent *entry;
    char subdirs[ROM_SUBDIR_MAX][ROM_NAME_LEN];
    int subdir_count = 0;

    entry = (struct lsfs_dirent *)exram_malloc(4, sizeof(struct lsfs_dirent));
    if (entry == NULL) {
        LISA_LOGE(TAG, "dirent alloc failed");
        return false;
    }

    lsfs_dir_t_init(&dir);
    if (lsfs_opendir(&dir, dir_path) != 0) {
        exram_free(entry);
        return false;
    }

    while (*count < ROM_MAX && lsfs_readdir(&dir, entry) == 0) {
        if (entry->name[0] == '\0' || entry->name[0] == (char)0xE5) {
            break;
        }

        /* 跳过 FAT 里的隐藏/系统项和 macOS 留下的 ._xxx */
        if (entry->name[0] == '.') {
            continue;
        }

        if (entry->type == LSFS_DIR_ENTRY_DIR) {
            if (depth < ROM_SUBDIR_DEPTH && subdir_count < ROM_SUBDIR_MAX) {
                memset(subdirs[subdir_count], 0, ROM_NAME_LEN);
                strncpy(subdirs[subdir_count], entry->name, ROM_NAME_LEN - 1);
                subdir_count++;
            }
            continue;
        }

        if (!rom_extension_supported(entry->name)) {
            continue;
        }

        struct arcade_rom_entry *slot = &s_roms[*count];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->name, ROM_NAME_LEN, "%s%s", prefix, entry->name);
        snprintf(slot->path, ROM_PATH_LEN, "%s/%s", dir_path, entry->name);
        slot->size = (uint32_t)entry->size;
        (*count)++;
    }

    lsfs_closedir(&dir);
    exram_free(entry);

    /* 子目录放到父目录的文件之后，保持「先根目录、后子目录」的直观顺序 */
    for (int i = 0; i < subdir_count && *count < ROM_MAX; i++) {
        char sub_path[ROM_PATH_LEN];
        char sub_prefix[ROM_NAME_LEN];

        snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, subdirs[i]);
        snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, subdirs[i]);

        (void)rom_collect_dir(sub_path, sub_prefix, count, depth + 1);
    }

    return true;
}

static bool rom_collect(void)
{
    int count = 0;

    if (!rom_collect_dir(ROM_DIR, "", &count, 0)) {
        return false;
    }

    /* 按文件名做一次插入排序，卡里的顺序通常不可控 */
    for (int i = 1; i < count; i++) {
        struct arcade_rom_entry key = s_roms[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(s_roms[j].name, key.name) > 0) {
            s_roms[j + 1] = s_roms[j];
            j--;
        }
        s_roms[j + 1] = key;
    }

    s_count = count;
    return true;
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
        return s_count;
    }

    if (!rom_dir_mount()) {
        if (!s_dir_missing_logged) {
            LISA_LOGW(TAG, "%s not accessible (no TF card or empty card)", ROM_DIR);
            s_dir_missing_logged = true;
        }
        s_count = 0;
        rom_add_demo_if_empty();
        return s_count;
    }

    if (!rom_collect()) {
        s_count = 0;
        rom_add_demo_if_empty();
        return s_count;
    }

    rom_add_demo_if_empty();

    LISA_LOGI(TAG, "found %d ROM(s) in %s", s_count, ROM_DIR);
    for (int i = 0; i < s_count; i++) {
        LISA_LOGI(TAG, "  [%02d] %s (%u bytes)", i, s_roms[i].name, (unsigned)s_roms[i].size);
    }

    return s_count;
}

int arcade_rom_count(void)
{
    return s_count > 0 ? s_count : 0;
}

const char *arcade_rom_name(int index)
{
    if (index < 0 || index >= arcade_rom_count()) {
        return NULL;
    }

    return s_roms[index].name;
}

const char *arcade_rom_path(int index)
{
    if (index < 0 || index >= arcade_rom_count()) {
        return NULL;
    }

    return s_roms[index].path;
}

uint32_t arcade_rom_size(int index)
{
    if (index < 0 || index >= arcade_rom_count()) {
        return 0;
    }

    return s_roms[index].size;
}

bool arcade_rom_scanned(void)
{
    return s_count >= 0;
}

void arcade_rom_invalidate(void)
{
    rom_entries_free();
    s_dir_missing_logged = false;
}
