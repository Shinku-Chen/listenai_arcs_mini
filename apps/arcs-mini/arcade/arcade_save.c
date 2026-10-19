/*
 * 卡带存档（cartridge RAM）落盘。
 *
 * 只在模拟器任务里读写，不做额外加锁；shell 命令通过 arcade_app.c 的请求队列
 * 转交给同一个任务，避免和帧循环同时动 cart_ram。
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_log.h"
#include "lsfs.h"

#include "arcade_save.h"

#define TAG "arcade-save"

#define SAVE_PATH_LEN      128
#define SAVE_AUTO_FLUSH_MS 30000

static char s_save_path[SAVE_PATH_LEN];
static char s_tmp_path[SAVE_PATH_LEN];

static uint8_t *s_cart_ram;
static uint32_t s_cart_ram_size;
static bool s_card_backed;
static volatile bool s_dirty;
static TickType_t s_last_flush;

/*
 * 由 ROM 路径推出 .sav / .sav.tmp 路径。
 * 内置演示 ROM 的路径是 "@demo"，不是绝对路径，直接判为不可存档。
 */
static bool save_path_build(const char *rom_path)
{
    const char *slash;
    const char *dot;
    size_t stem_len;
    int n;

    if (rom_path == NULL || rom_path[0] != '/') {
        return false;
    }

    slash = strrchr(rom_path, '/');
    dot = strrchr(rom_path, '.');

    /* 只去掉最后一个扩展名；目录名里的点不算 */
    if (dot != NULL && slash != NULL && dot > slash) {
        stem_len = (size_t)(dot - rom_path);
    } else {
        stem_len = strlen(rom_path);
    }

    n = snprintf(s_save_path, sizeof(s_save_path), "%.*s.sav", (int)stem_len, rom_path);
    if (n < 0 || (size_t)n >= sizeof(s_save_path)) {
        return false;
    }

    n = snprintf(s_tmp_path, sizeof(s_tmp_path), "%.*s.sav.tmp", (int)stem_len, rom_path);
    if (n < 0 || (size_t)n >= sizeof(s_tmp_path)) {
        return false;
    }

    return true;
}

static void save_load(void)
{
    struct lsfs_file_t file;
    off_t size;
    ssize_t got;
    int rc;

    lsfs_file_t_init(&file);
    rc = lsfs_open(&file, s_save_path, LSFS_O_READ);
    if (rc != 0) {
        LISA_LOGI(TAG, "no save file yet: %s", s_save_path);
        return;
    }

    size = lsfs_lsize(&file);
    if (size <= 0) {
        LISA_LOGW(TAG, "save file %s is empty, ignored", s_save_path);
        lsfs_close(&file);
        return;
    }

    if ((uint32_t)size > s_cart_ram_size) {
        LISA_LOGW(TAG, "save %s is %ld bytes > cart RAM %u, truncated",
                  s_save_path, (long)size, (unsigned)s_cart_ram_size);
    }

    got = lsfs_read(&file, s_cart_ram, s_cart_ram_size);
    lsfs_close(&file);

    if (got <= 0) {
        LISA_LOGW(TAG, "read %s failed (%d), start with empty cart RAM",
                  s_save_path, (int)got);
        return;
    }

    if ((uint32_t)got < s_cart_ram_size) {
        LISA_LOGI(TAG, "save %s is %d bytes, rest of cart RAM stays zeroed",
                  s_save_path, (int)got);
    }

    LISA_LOGI(TAG, "loaded save %s (%d bytes)", s_save_path, (int)got);
}

/** @return 写入字节数，<0 失败 */
static int save_write(void)
{
    struct lsfs_file_t file;
    ssize_t wrote;
    int rc;

    lsfs_file_t_init(&file);
    rc = lsfs_open(&file, s_tmp_path, LSFS_O_WRITE | LSFS_O_CREATE | LSFS_O_TRUNC);
    if (rc != 0) {
        LISA_LOGW(TAG, "open %s failed (%d), save skipped", s_tmp_path, rc);
        return rc;
    }

    wrote = lsfs_write(&file, s_cart_ram, s_cart_ram_size);
    if (wrote != (ssize_t)s_cart_ram_size) {
        LISA_LOGW(TAG, "short write %d/%u on %s", (int)wrote,
                  (unsigned)s_cart_ram_size, s_tmp_path);
        lsfs_close(&file);
        (void)lsfs_unlink(s_tmp_path);
        return -1;
    }

    if (lsfs_sync(&file) != 0) {
        /* 只是让数据尽早落到介质上；失败不致命，close 时还会再刷一次 */
        LISA_LOGW(TAG, "sync %s failed", s_tmp_path);
    }
    lsfs_close(&file);

    /*
     * FatFs 的 f_rename 在目标已存在时返回 FR_EXIST，所以先删旧文件。
     * 这一步掉电只会丢「上一份存档」，刚写的 .tmp 仍完整躺在卡上。
     */
    (void)lsfs_unlink(s_save_path);

    rc = lsfs_rename(s_tmp_path, s_save_path);
    if (rc != 0) {
        LISA_LOGW(TAG, "rename %s -> %s failed (%d)", s_tmp_path, s_save_path, rc);
        return -1;
    }

    return (int)s_cart_ram_size;
}

bool arcade_save_attach(const char *rom_path, uint8_t *cart_ram, uint32_t cart_ram_size)
{
    arcade_save_detach();

    if (cart_ram == NULL || cart_ram_size == 0) {
        return false;
    }

    s_cart_ram = cart_ram;
    s_cart_ram_size = cart_ram_size;
    s_dirty = false;
    s_last_flush = xTaskGetTickCount();

    s_card_backed = save_path_build(rom_path);
    if (!s_card_backed) {
        LISA_LOGI(TAG, "%s is not on the card, saving disabled",
                  rom_path != NULL ? rom_path : "?");
        return false;
    }

    save_load();
    return true;
}

void arcade_save_detach(void)
{
    s_cart_ram = NULL;
    s_cart_ram_size = 0;
    s_card_backed = false;
    s_dirty = false;
    s_save_path[0] = '\0';
    s_tmp_path[0] = '\0';
}

void arcade_save_mark_dirty(void)
{
    s_dirty = true;
}

int arcade_save_flush(void)
{
    int written;

    if (!s_dirty) {
        return 0;
    }

    if (!s_card_backed || s_cart_ram == NULL) {
        s_dirty = false;
        return 0;
    }

    written = save_write();
    if (written >= 0) {
        s_dirty = false;
        s_last_flush = xTaskGetTickCount();
        if (written > 0) {
            LISA_LOGI(TAG, "saved %d bytes to %s", written, s_save_path);
        }
    }

    return written;
}

void arcade_save_tick(void)
{
    if (!s_dirty || !s_card_backed) {
        return;
    }

    TickType_t now = xTaskGetTickCount();

    if ((now - s_last_flush) < pdMS_TO_TICKS(SAVE_AUTO_FLUSH_MS)) {
        return;
    }

    s_last_flush = now;
    (void)arcade_save_flush();
}

const char *arcade_save_path(void)
{
    return s_card_backed ? s_save_path : NULL;
}

bool arcade_save_pending(void)
{
    return s_dirty;
}
