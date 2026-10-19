/*
 * Peanut-GB 胶水层。
 *
 * peanut_gb.h 是单头文件实现，只在这一个编译单元里 include，
 * 避免重复定义 gb_init()/gb_run_frame()。
 *
 * ROM 载入策略（按优先级）：
 *   1. 整份读进 PSRAM —— 默认路径。PSRAM 够的时候永远选它：模拟过程中
 *      不再碰文件系统和 TF 卡，gb_rom_read() 就是一次数组访问，帧率稳定。
 *   2. PSRAM 放不下时退回「按 16KB 银行从 TF 卡读 + LRU 缓存」。RAM 占用
 *      固定在几十 KB，卡上 8MB 以内的 ROM 也能跑（但银行切换会读卡，慢）。
 *
 * 注意：本平台的 exram_malloc() 一旦失败会走 sysheap.c 里的
 * CRASH_ON_ALLOC_FAIL 路径直接 trap（整机死机，屏幕冻结、adb shell 也挂住），
 * 所以每次分配前都必须先查 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)。
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_heap_caps.h"
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

/* GB/GBC 卡带按 16KB 分银行，Peanut-GB 的 gb_rom_read() 收到的是绝对偏移 */
#define ROM_BANK_BYTES        (16U * 1024U)
#define ROM_CACHE_SLOTS_MAX  8
#define ROM_SIZE_LIMIT       (8U * 1024U * 1024U)
#define ROM_BANK_INVALID     0xFFFFFFFFU

#define ARCADE_CART_RAM_DEFAULT (32U * 1024U)

/* PSRAM 分配失败会被系统直接 trap，所以每次分配前先确认余量：
 * 宁可让模拟器优雅报错，也不能把整机打死。 */
#define PSRAM_ALLOC_MARGIN (96U * 1024U)

struct rom_cache {
    uint8_t *data;
    int slots;
    uint32_t bank[ROM_CACHE_SLOTS_MAX];
    uint32_t stamp[ROM_CACHE_SLOTS_MAX];
    uint32_t tick;
};

struct arcade_gb_priv {
    /* 卡上的 ROM：文件一直开着，整份载入失败时按银行读 */
    uint32_t rom_file_size;
    /* ROM 大小向上取整到 2 的幂，越界读的掩码回卷才安全 */
    uint32_t rom_size;
    /* ROM 在 PSRAM 里的副本：整份载入成功时非 NULL，也用于内置演示 ROM */
    uint8_t *rom_mem;
    struct rom_cache cache;
    uint8_t *cart_ram;
    uint32_t cart_ram_size;
    bool read_err_logged;
    bool first_frame_logged;
    struct gb_s gb;
};

/*
 * FATFS/VFS 的结构体不放 PSRAM（本平台上 lsfs 的元数据必须落在内部 SRAM），
 * 而且同一时刻只会开一个 ROM，所以文件句柄用静态变量。
 */
static struct lsfs_file_t s_rom_file;
static bool s_rom_file_open;

static struct arcade_gb_priv *s_priv;
static bool s_running;
static bool s_error_logged;
/* 模拟器报错计数：CPU 跑飞（非法指令 / 非法读写）时会累加 */
static uint32_t s_err_count;
static int s_err_last = -1;
static uint16_t s_err_addr;

/* ---------------- PSRAM 分配（带余量检查） ---------------- */

static size_t psram_free_bytes(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static void *psram_alloc(size_t size, const char *what)
{
    size_t free_bytes = psram_free_bytes();
    void *ptr;

    if (free_bytes < size + PSRAM_ALLOC_MARGIN) {
        LISA_LOGE(TAG, "%s: need %u B, psram free only %u B",
                  what, (unsigned)size, (unsigned)free_bytes);
        return NULL;
    }

    ptr = exram_malloc(4, size);

    if (ptr == NULL) {
        LISA_LOGE(TAG, "%s: alloc %u B failed", what, (unsigned)size);
    }

    return ptr;
}

/* ---------------- ROM 银行缓存 ---------------- */

static void rom_cache_reset(struct rom_cache *c)
{
    for (int i = 0; i < ROM_CACHE_SLOTS_MAX; i++) {
        c->bank[i] = ROM_BANK_INVALID;
        c->stamp[i] = 0;
    }
    c->tick = 0;
}

static bool rom_cache_init(struct rom_cache *c)
{
    static const int k_slots[] = {ROM_CACHE_SLOTS_MAX, 4, 2, 1};

    c->data = NULL;
    c->slots = 0;
    rom_cache_reset(c);

    for (size_t i = 0; i < sizeof(k_slots) / sizeof(k_slots[0]); i++) {
        size_t size = (size_t)k_slots[i] * ROM_BANK_BYTES;
        uint8_t *buf = (uint8_t *)psram_alloc(size, "rom bank cache");

        if (buf != NULL) {
            c->data = buf;
            c->slots = k_slots[i];
            LISA_LOGI(TAG, "rom bank cache: %d x %u B", c->slots, (unsigned)ROM_BANK_BYTES);
            return true;
        }
    }

    LISA_LOGE(TAG, "no psram left for rom bank cache");
    return false;
}

static void rom_bank_fill(struct arcade_gb_priv *p, uint8_t *dst, uint32_t bank_index)
{
    uint32_t off = bank_index * ROM_BANK_BYTES;
    uint32_t len;
    uint32_t done = 0;

    /* 读不到的部分保持 0xFF，和真机未接线的表现接近，不打断模拟器 */
    memset(dst, 0xFF, ROM_BANK_BYTES);

    if (!s_rom_file_open || off >= p->rom_file_size) {
        return;
    }

    len = p->rom_file_size - off;
    if (len > ROM_BANK_BYTES) {
        len = ROM_BANK_BYTES;
    }

    if (lsfs_seek(&s_rom_file, (off_t)off, LSFS_SEEK_SET) != 0) {
        if (!p->read_err_logged) {
            LISA_LOGW(TAG, "seek %u failed, ROM reads return 0xFF", (unsigned)off);
            p->read_err_logged = true;
        }
        return;
    }

    while (done < len) {
        ssize_t got = lsfs_read(&s_rom_file, dst + done, len - done);

        if (got <= 0) {
            if (!p->read_err_logged) {
                LISA_LOGW(TAG, "short ROM read at %u (%u/%u), rest filled with 0xFF",
                          (unsigned)off, (unsigned)done, (unsigned)len);
                p->read_err_logged = true;
            }
            break;
        }

        done += (uint32_t)got;
    }
}

/* 取 bank_index 对应的 16KB 缓冲（未命中就从卡上读进来） */
static uint8_t *rom_cache_bank(struct arcade_gb_priv *p, uint32_t bank_index)
{
    struct rom_cache *c = &p->cache;
    int slot = -1;
    uint8_t *buf;

    for (int i = 0; i < c->slots; i++) {
        if (c->bank[i] == bank_index) {
            c->stamp[i] = ++c->tick;
            return c->data + (size_t)i * ROM_BANK_BYTES;
        }
    }

    /* 先找空槽，没有就按 LRU 淘汰 */
    for (int i = 0; i < c->slots; i++) {
        if (c->bank[i] == ROM_BANK_INVALID) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < c->slots; i++) {
            if (c->stamp[i] < c->stamp[slot]) {
                slot = i;
            }
        }
    }

    buf = c->data + (size_t)slot * ROM_BANK_BYTES;
    rom_bank_fill(p, buf, bank_index);
    c->bank[slot] = bank_index;
    c->stamp[slot] = ++c->tick;

    return buf;
}

/* ---------------- Peanut-GB 前端回调 ---------------- */

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    struct arcade_gb_priv *p = (struct arcade_gb_priv *)gb->direct.priv;
    uint32_t a = (uint32_t)addr;
    const uint8_t *bank;

    if (p == NULL || p->rom_size == 0) {
        return 0xFF;
    }

    /* 越界读按 ROM 大小回卷，和真机未接线的数据线表现接近 */
    if (a >= p->rom_size) {
        a &= (p->rom_size - 1);
    }

    if (p->rom_mem != NULL) {
        return p->rom_mem[a];
    }

    bank = rom_cache_bank(p, a / ROM_BANK_BYTES);
    if (bank == NULL) {
        return 0xFF;
    }

    return bank[a & (ROM_BANK_BYTES - 1)];
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

    s_err_count++;
    s_err_last = (int)err;
    s_err_addr = addr;

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

/* ---------------- ROM 打开 / 关闭 ---------------- */

static void rom_close(void)
{
    if (s_rom_file_open) {
        lsfs_close(&s_rom_file);
        lsfs_file_t_init(&s_rom_file);
        s_rom_file_open = false;
    }
}

/* ROM 大小向上取整到 2 的幂（最小 32KB），gb_rom_read() 的掩码回卷才安全 */
static uint32_t rom_padded_size(uint32_t size)
{
    uint32_t padded = 32U * 1024U;

    while (padded < size && padded < ROM_SIZE_LIMIT) {
        padded <<= 1;
    }

    return padded;
}

static bool rom_open_demo(struct arcade_gb_priv *p)
{
    uint32_t padded;
    uint8_t *buf;

    if (arcade_demo_rom_size == 0) {
        LISA_LOGE(TAG, "built-in demo ROM is empty");
        return false;
    }

    padded = rom_padded_size(arcade_demo_rom_size);
    buf = (uint8_t *)psram_alloc(padded, "demo rom");
    if (buf == NULL) {
        return false;
    }

    memset(buf, 0xFF, padded);
    memcpy(buf, arcade_demo_rom, arcade_demo_rom_size);

    p->rom_mem = buf;
    p->rom_size = padded;
    return true;
}

static bool rom_open_file(struct arcade_gb_priv *p, const char *path)
{
    off_t size;

    lsfs_file_t_init(&s_rom_file);

    if (lsfs_open(&s_rom_file, path, LSFS_O_READ) != 0) {
        LISA_LOGE(TAG, "open %s failed", path);
        return false;
    }

    s_rom_file_open = true;

    size = lsfs_lsize(&s_rom_file);
    if (size <= 0 || (uint32_t)size > ROM_SIZE_LIMIT) {
        LISA_LOGE(TAG, "%s size %ld out of range (max %u)", path, (long)size,
                  (unsigned)ROM_SIZE_LIMIT);
        rom_close();
        return false;
    }

    p->rom_file_size = (uint32_t)size;
    p->rom_size = rom_padded_size(p->rom_file_size);
    return true;
}

/*
 * 整份读进 PSRAM。成功返回 true，失败（内存不够 / 读文件出错）返回 false，
 * 调用方再退回银行缓存。读不满的部分保持 0xFF，和真机未接线的表现接近。
 */
static bool rom_load_image(struct arcade_gb_priv *p, const char *path)
{
    uint8_t *buf;
    uint32_t len = p->rom_size;
    uint32_t done = 0;

    if (path == NULL) {
        return false;
    }

    buf = (uint8_t *)psram_alloc(len, "rom image");
    if (buf == NULL) {
        LISA_LOGW(TAG, "rom image %u B does not fit, falling back to bank cache",
                  (unsigned)len);
        return false;
    }

    memset(buf, 0xFF, len);

    if (lsfs_seek(&s_rom_file, 0, LSFS_SEEK_SET) != 0) {
        LISA_LOGW(TAG, "seek 0 failed, ROM reads would return 0xFF");
        exram_free(buf);
        return false;
    }

    while (done < p->rom_file_size) {
        ssize_t got = lsfs_read(&s_rom_file, buf + done, p->rom_file_size - done);

        if (got <= 0) {
            LISA_LOGW(TAG, "short ROM read at %u/%u, rest filled with 0xFF",
                      (unsigned)done, (unsigned)p->rom_file_size);
            break;
        }

        done += (uint32_t)got;
    }

    if (done == 0) {
        LISA_LOGE(TAG, "read 0 bytes from %s", path);
        exram_free(buf);
        return false;
    }

    p->rom_mem = buf;
    LISA_LOGI(TAG, "rom image in psram: %u B (read %u B), psram free %u KB",
              (unsigned)len, (unsigned)done,
              (unsigned)(psram_free_bytes() / 1024));
    return true;
}

/* ---------------- 对外接口 ---------------- */

int arcade_gb_start(const char *path)
{
    enum gb_init_error_e ret;
    size_t free_bytes;
    bool whole_rom;

    arcade_gb_stop();

    if (path == NULL) {
        return -1;
    }

    free_bytes = psram_free_bytes();
    LISA_LOGI(TAG, "loading %s, psram free %u KB", path, (unsigned)(free_bytes / 1024));

    s_priv = (struct arcade_gb_priv *)psram_alloc(sizeof(struct arcade_gb_priv), "gb priv");
    if (s_priv == NULL) {
        return -1;
    }
    memset(s_priv, 0, sizeof(*s_priv));

    if (strcmp(path, "@demo") == 0) {
        if (!rom_open_demo(s_priv)) {
            arcade_gb_stop();
            return -1;
        }
        whole_rom = true;
    } else {
        if (!rom_open_file(s_priv, path)) {
            arcade_gb_stop();
            return -1;
        }

        whole_rom = rom_load_image(s_priv, path);
        if (whole_rom) {
            /* 整份 ROM 已在 PSRAM 里，文件句柄不用再占着，
             * 避免模拟过程中一直持有一个 FATFS 句柄 */
            rom_close();
        } else if (!rom_cache_init(&s_priv->cache)) {
            arcade_gb_stop();
            return -1;
        }
    }

    s_priv->cart_ram_size = CONFIG_ARCADE_CART_RAM_MAX;
    if (s_priv->cart_ram_size < ARCADE_CART_RAM_DEFAULT) {
        s_priv->cart_ram_size = ARCADE_CART_RAM_DEFAULT;
    }
    s_priv->cart_ram = (uint8_t *)psram_alloc(s_priv->cart_ram_size, "cart ram");
    if (s_priv->cart_ram == NULL) {
        arcade_gb_stop();
        return -1;
    }
    memset(s_priv->cart_ram, 0, s_priv->cart_ram_size);

    /* 先读回已有存档，再让 gb_init() 把 MBC 指向这块 RAM */
    arcade_save_attach(path, s_priv->cart_ram, s_priv->cart_ram_size);

    s_error_logged = false;
    s_err_count = 0;
    s_err_last = -1;
    s_err_addr = 0;

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
    /* direct.joypad 是低电平有效：1=松开、0=按下。复位后必须全部松开，
     * 否则游戏一开机就看到八个键全被按住（Pokemon 会卡在初始化）。 */
    s_priv->gb.direct.joypad = 0xFF;

    s_running = true;
    LISA_LOGI(TAG, "GB started: %s, rom %u bytes (%u B on card), rom_in_ram=%d, mbc %d, save %u bytes",
              path, (unsigned)s_priv->rom_size,
              (unsigned)(s_priv->rom_mem != NULL ? 0 : s_priv->rom_file_size),
              s_priv->rom_mem != NULL ? 1 : 0,
              (int)s_priv->gb.mbc, (unsigned)s_priv->cart_ram_size);
    LISA_LOGI(TAG, "psram free after load: %u KB", (unsigned)(psram_free_bytes() / 1024));

    return 0;
}

void arcade_gb_stop(void)
{
    s_running = false;

    if (s_priv == NULL) {
        rom_close();
        return;
    }

    /* 退出、换 ROM、关机前都走这里：有改动才写盘 */
    (void)arcade_save_flush();
    arcade_save_detach();

    rom_close();

    if (s_priv->cache.data != NULL) {
        exram_free(s_priv->cache.data);
        s_priv->cache.data = NULL;
    }
    if (s_priv->rom_mem != NULL) {
        exram_free(s_priv->rom_mem);
        s_priv->rom_mem = NULL;
    }
    if (s_priv->cart_ram != NULL) {
        exram_free(s_priv->cart_ram);
        s_priv->cart_ram = NULL;
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

    if (!s_priv->first_frame_logged) {
        s_priv->first_frame_logged = true;
        LISA_LOGI(TAG, "first frame done, psram free %u KB",
                  (unsigned)(psram_free_bytes() / 1024));
    }
}

void arcade_gb_set_input(uint32_t mask)
{
    if (s_priv == NULL) {
        return;
    }

    /*
     * ARCADE_KEY_* / JOYPAD_* 位为 1 表示「按下」，但 Peanut-GB 的
     * direct.joypad 是低电平有效（和 JOYP 寄存器一样：1=松开、0=按下），
     * 所以这里要反过来写。官方 sdl2 示例也是 released 时 |= 、pressed 时 &= ~。
     */
    s_priv->gb.direct.joypad = (uint8_t)(~(mask & 0xFF));
}

uint32_t arcade_gb_rom_checksum(void)
{
    uint32_t hash = 2166136261u;

    if (s_priv == NULL || s_priv->rom_size == 0) {
        return 0;
    }

    for (uint32_t addr = 0; addr < s_priv->rom_size; addr++) {
        hash ^= gb_rom_read(&s_priv->gb, addr);
        hash *= 16777619u;
    }

    return hash;
}

int arcade_gb_rom_header(char *out, uint32_t out_len)
{
    char title[17];
    int n;

    if (s_priv == NULL || out == NULL || out_len < 32) {
        return -1;
    }

    for (int i = 0; i < 16; i++) {
        uint8_t c = gb_rom_read(&s_priv->gb, 0x0134u + (uint16_t)i);

        title[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    title[16] = '\0';

    n = snprintf(out, out_len, "title=[%s] cart=0x%02X rom_size=0x%02X",
                 title,
                 (unsigned)gb_rom_read(&s_priv->gb, 0x0147u),
                 (unsigned)gb_rom_read(&s_priv->gb, 0x0148u));

    return n;
}

/*
 * CPU / LCD 状态快照。
 *
 * 全白屏的黑是最多的：ROM 读错、CPU 跑飞、LCDC 没开都会表现为全白。
 * 连续调两次对比 PC / LY，就能区分「CPU 在跑」和「CPU 卡死」，再结合
 * LCDC / BGP / vram 就知道屏上的白是大是小。
 */
int arcade_gb_machine_report(char *out, uint32_t out_len)
{
    static const uint8_t kNintendoLogo[48] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
        0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
        0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
        0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
    };
    struct gb_s *gb;
    uint32_t vram_sum = 0;
    int nonzero = 0;
    int logo_ok = 1;
    int n;

    if (s_priv == NULL || out == NULL || out_len < 128) {
        return -1;
    }

    gb = &s_priv->gb;

    for (int i = 0; i < VRAM_SIZE; i++) {
        vram_sum += gb->vram[i];
        if (gb->vram[i] != 0) {
            nonzero++;
        }
    }

    /* 任天堂 logo 是卡带上固定的 48 字节，对不上就说明 ROM 读取/载入坏了 */
    for (int i = 0; i < 48; i++) {
        if (gb_rom_read(gb, 0x0104u + (uint16_t)i) != kNintendoLogo[i]) {
            logo_ok = 0;
            break;
        }
    }

    n = snprintf(out, out_len, "pc=%04X sp=%04X af=%02X%02X bc=%04X de=%04X hl=%04X\n",
                 (unsigned)gb->cpu_reg.pc.reg, (unsigned)gb->cpu_reg.sp.reg,
                 (unsigned)gb->cpu_reg.a, (unsigned)gb->cpu_reg.f.reg,
                 (unsigned)gb->cpu_reg.bc.reg, (unsigned)gb->cpu_reg.de.reg,
                 (unsigned)gb->cpu_reg.hl.reg);

    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "lcdc=%02X ly=%02X bgp=%u,%u,%u,%u halt=%d ime=%d blank=%d\n",
                  (unsigned)gb->hram_io[IO_LCDC], (unsigned)gb->hram_io[IO_LY],
                  (unsigned)gb->display.bg_palette[0],
                  (unsigned)gb->display.bg_palette[1],
                  (unsigned)gb->display.bg_palette[2],
                  (unsigned)gb->display.bg_palette[3],
                  gb->gb_halt ? 1 : 0, gb->gb_ime ? 1 : 0,
                  gb->lcd_blank ? 1 : 0);

    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "vram=%d/%d sum=%u mbc=%d bank=%u rambank=%u ramblk=%u mbc3O=%d\n",
                  nonzero, (int)VRAM_SIZE, (unsigned)vram_sum,
                  (int)gb->mbc, (unsigned)gb->selected_rom_bank,
                  (unsigned)gb->cart_ram_bank, (unsigned)gb->num_ram_banks,
                  gb->cart_is_mbc3O ? 1 : 0);

    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "logo=%s ramcode=%02X err=%u last=%d@%04X",
                  logo_ok ? "ok" : "BAD",
                  (unsigned)gb_rom_read(gb, 0x0149u),
                  (unsigned)s_err_count, s_err_last, (unsigned)s_err_addr);

    return n;
}

int arcade_gb_peek(char *out, uint32_t out_len, uint32_t addr, uint32_t len)
{
    struct gb_s *gb;
    uint32_t off = 0;

    if (s_priv == NULL || out == NULL || out_len < 16) {
        return -1;
    }

    if (len == 0 || len > 256) {
        len = 64;
    }

    gb = &s_priv->gb;

    for (uint32_t i = 0; i < len && off + 4 < out_len; i++) {
        off += (uint32_t)snprintf(out + off, out_len - off, "%02X ",
                                  (unsigned)__gb_read(gb, (uint16_t)((addr + i) & 0xFFFF)));
        if (((addr + i) & 0x0F) == 0x0F && off + 12 < out_len) {
            off += (uint32_t)snprintf(out + off, out_len - off, "\n");
        }
    }

    return (int)off;
}
