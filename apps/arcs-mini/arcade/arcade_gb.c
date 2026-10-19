/*
 * Peanut-GB 胶水层。
 *
 * peanut_gb.h 是单头文件实现，只在这一个编译单元里 include，
 * 避免重复定义 gb_init()/gb_run_frame()。
 */
#include <string.h>
#include <stdlib.h>

#include "sysheap.h"
#include "lisa_log.h"
#include "lsfs.h"

#include "arcade_core.h"
#include "arcade_demo_rom.h"
#include "arcade_gb.h"
#include "arcade_save.h"
#include "arcade_video.h"

#include "peanut_gb.h"

#define TAG "arcade-gb"

/* GB ROM 上限：PSRAM 足够容纳常见商业 ROM，超过就拒绝加载 */
#define ARCADE_ROM_MAX_BYTES (8U * 1024U * 1024U)
#define ARCADE_CART_RAM_DEFAULT (32U * 1024U)

struct arcade_gb_priv {
    uint8_t *rom;
    uint32_t rom_size;
    uint8_t *cart_ram;
    uint32_t cart_ram_size;
    struct gb_s gb;
};

static struct arcade_gb_priv *s_priv;
static bool s_running;
static bool s_error_logged;

/* ---------------- Peanut-GB 前端回调 ---------------- */

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    const struct arcade_gb_priv *p = (const struct arcade_gb_priv *)gb->direct.priv;

    if (p->rom == NULL || p->rom_size == 0) {
        return 0xFF;
    }

    if (addr < p->rom_size) {
        return p->rom[addr];
    }

    /* 越界读按 16KB 银行回卷，和真机未接线的数据线表现接近 */
    return p->rom[addr & (p->rom_size - 1)];
}

static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    const struct arcade_gb_priv *p = (const struct arcade_gb_priv *)gb->direct.priv;

    if (p->cart_ram == NULL || addr >= p->cart_ram_size) {
        return 0xFF;
    }

    return p->cart_ram[addr];
}

static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    struct arcade_gb_priv *p = (struct arcade_gb_priv *)gb->direct.priv;

    if (p->cart_ram == NULL || addr >= p->cart_ram_size) {
        return;
    }

    p->cart_ram[addr] = val;

    /* 游戏真写了卡带存档区才标记，避免每帧都去动文件系统 */
    arcade_save_mark_dirty();
}

static void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    (void)gb;

    if (!s_error_logged) {
        LISA_LOGW(TAG, "GB error %d at 0x%04X, emulation keeps running", (int)err, (unsigned)addr);
        s_error_logged = true;
    }
}

static void gb_lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    (void)gb;
    arcade_video_draw_line((int)line, pixels);
}

/* ---------------- 对外接口 ---------------- */

static uint8_t *rom_load(const char *path, uint32_t *size_out)
{
    struct lsfs_file_t file;
    uint8_t *buf = NULL;
    off_t size;

    if (strcmp(path, "@demo") == 0) {
        /* 内置演示 ROM：直接从 flash 拷贝到 PSRAM */
        uint32_t padded = 32U * 1024U;

        if (arcade_demo_rom_size == 0) {
            LISA_LOGE(TAG, "built-in demo ROM is empty");
            return NULL;
        }

        while (padded < arcade_demo_rom_size && padded < ARCADE_ROM_MAX_BYTES) {
            padded <<= 1;
        }

        buf = (uint8_t *)exram_malloc(4, padded);
        if (buf == NULL) {
            LISA_LOGE(TAG, "demo ROM alloc %u failed", (unsigned)padded);
            return NULL;
        }

        memset(buf, 0xFF, padded);
        memcpy(buf, arcade_demo_rom, arcade_demo_rom_size);
        *size_out = padded;
        return buf;
    }

    lsfs_file_t_init(&file);
    if (lsfs_open(&file, path, LSFS_O_READ) != 0) {
        LISA_LOGE(TAG, "open %s failed", path);
        return NULL;
    }

    size = lsfs_lsize(&file);
    if (size <= 0 || (uint32_t)size > ARCADE_ROM_MAX_BYTES) {
        LISA_LOGE(TAG, "%s size %ld out of range", path, (long)size);
        lsfs_close(&file);
        return NULL;
    }

    /* ROM 大小取到下一个 2 的幂，gb_rom_read() 的掩码回卷才安全 */
    uint32_t padded = 32U * 1024U;
    while (padded < (uint32_t)size && padded < ARCADE_ROM_MAX_BYTES) {
        padded <<= 1;
    }

    buf = (uint8_t *)exram_malloc(4, padded);
    if (buf == NULL) {
        LISA_LOGE(TAG, "ROM alloc %u failed", (unsigned)padded);
        lsfs_close(&file);
        return NULL;
    }
    memset(buf, 0xFF, padded);

    ssize_t got = lsfs_read(&file, buf, (size_t)size);
    lsfs_close(&file);

    if (got != (ssize_t)size) {
        LISA_LOGE(TAG, "short read %d/%u", (int)got, (unsigned)size);
        exram_free(buf);
        return NULL;
    }

    *size_out = padded;
    return buf;
}

int arcade_gb_start(const char *path)
{
    enum gb_init_error_e ret;

    arcade_gb_stop();

    s_priv = (struct arcade_gb_priv *)exram_calloc(4, 1, sizeof(struct arcade_gb_priv));
    if (s_priv == NULL) {
        LISA_LOGE(TAG, "priv alloc %u failed", (unsigned)sizeof(struct arcade_gb_priv));
        return -1;
    }

    s_priv->rom = rom_load(path, &s_priv->rom_size);
    if (s_priv->rom == NULL) {
        arcade_gb_stop();
        return -1;
    }

    s_priv->cart_ram_size = CONFIG_ARCADE_CART_RAM_MAX;
    if (s_priv->cart_ram_size < ARCADE_CART_RAM_DEFAULT) {
        s_priv->cart_ram_size = ARCADE_CART_RAM_DEFAULT;
    }
    s_priv->cart_ram = (uint8_t *)exram_calloc(4, 1, s_priv->cart_ram_size);
    if (s_priv->cart_ram == NULL) {
        LISA_LOGE(TAG, "cart ram alloc %u failed", (unsigned)s_priv->cart_ram_size);
        arcade_gb_stop();
        return -1;
    }

    /* 先读回已有存档，再让 gb_init() 把 MBC 指向这块 RAM */
    arcade_save_attach(path, s_priv->cart_ram, s_priv->cart_ram_size);

    s_error_logged = false;

    ret = gb_init(&s_priv->gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write,
                  &gb_error, s_priv);
    if (ret != GB_INIT_NO_ERROR) {
        LISA_LOGE(TAG, "gb_init failed: %d (%s)", (int)ret,
                  ret == GB_INIT_INVALID_CHECKSUM ? "invalid header checksum"
                                                  : "unsupported cartridge");
        arcade_gb_stop();
        return -1;
    }

    gb_init_lcd(&s_priv->gb, &gb_lcd_line);
    s_priv->gb.direct.joypad = 0;

    s_running = true;
    LISA_LOGI(TAG, "GB started: %s, rom %u bytes, mbc %d, save %u bytes",
              path, (unsigned)s_priv->rom_size, (int)s_priv->gb.mbc,
              (unsigned)s_priv->cart_ram_size);

    return 0;
}

void arcade_gb_stop(void)
{
    s_running = false;

    if (s_priv == NULL) {
        return;
    }

    /* 退出、换 ROM、关机前都走这里：有改动才写盘 */
    (void)arcade_save_flush();
    arcade_save_detach();

    if (s_priv->rom != NULL) {
        exram_free(s_priv->rom);
    }
    if (s_priv->cart_ram != NULL) {
        exram_free(s_priv->cart_ram);
    }
    exram_free(s_priv);
    s_priv = NULL;
}

bool arcade_gb_running(void)
{
    return s_running;
}

void arcade_gb_run_frame(void)
{
    if (!s_running || s_priv == NULL) {
        return;
    }

    gb_run_frame(&s_priv->gb);
}

void arcade_gb_set_input(uint32_t mask)
{
    if (s_priv == NULL) {
        return;
    }

    s_priv->gb.direct.joypad = (uint8_t)(mask & 0xFF);
}
