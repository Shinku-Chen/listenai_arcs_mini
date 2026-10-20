/*
 * binjgb 胶水层（GB + GBC 完整支持）。
 *
 * 为什么换内核：Peanut-GB 完全没有 CGB 支持（SVBK / VBK / HDMA 一处都没实现），
 * Pokemon 水晶这类 GBC 游戏片头能动、一进正式流程就卡死在 pokecrystal 的
 * DelayFrame() 里。binjgb 是纯 C99 + MIT，CGB 的 VRAM/WRAM 分 bank、CGB 调色板、
 * GDMA/HDMA、双速模式都有，ROM 大小不再需要我们自己管银行切换。
 *
 * 和 arcade_gb.c（Peanut-GB 版）的关系：
 *   - 两个文件实现同一套 arcade_gb_* 接口，由 CONFIG_ARCADE_GB_CORE 二选一编译；
 *   - binjgb 版默认给 GBC 用，DMG 游戏也能跑（DMG 模式下把 4 档调色板换成
 *     掌机绿）。
 *
 * 内存：Emulator 结构体近 300KB、ROM 2MB 起，默认 malloc 走内部 SRAM 必然失败，
 * 所以下面用 arcade_bin_* 把 binjgb 的所有堆分配重定向到 PSRAM（memory.h 里
 * 已经把 xmalloc/xfree 换成了这三个函数）。分配前必须先查余量：本平台
 * exram_malloc() 失败会走 sysheap.c 的 CRASH_ON_ALLOC_FAIL 直接 trap，整机死机。
 */
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "esp_heap_caps.h"
#include "sysheap.h"
#include "lisa_log.h"
#include "lsfs.h"

#include "arcade_core.h"
#include "arcade_audio.h"
#include "arcade_demo_rom.h"
#include "arcade_gb.h"
#include "arcade_save.h"
#include "arcade_ui.h"
#include "arcade_video.h"

#include "emulator.h"

#define TAG "arcade-gbc"

/* 送屏模块和 binjgb 必须对帧缓冲像素宽度达成一致（都看 arcade_fb.h） */
_Static_assert(sizeof(FBPixel) == sizeof(arcade_fbpixel_t),
               "frame buffer pixel type mismatch");

#define ROM_SIZE_LIMIT          (8U * 1024U * 1024U)
#define ROM_BANK_BYTES          (32U * 1024U)   /* binjgb 要求 ROM 是 32KB 的整数倍 */
#define PSRAM_ALLOC_MARGIN      (96U * 1024U)

/* 音频：binjgb 的 APU 按 ARCADE_AUDIO_RATE 重采样输出（u8 立体声），
 * 每帧由 arcade_audio_push() 推进喇叭链路。缓冲区要装得下一帧的量
 * （48kHz / 59.7fps ≈ 803 样本），留一点余量避免中途 AUDIO_BUFFER_FULL
 * 导致 emulator_run_until() 把没被取走的采样直接丢掉。 */
#define ARCADE_AUDIO_FREQUENCY  ARCADE_AUDIO_RATE
#define ARCADE_AUDIO_FRAMES     1280    /* 26ms */

#define ARCADE_BIN_MAGIC        0x424A4742U     /* "BJGB" */

#define PC_HIST_SLOTS           32

struct arcade_bin_hdr {
    uint32_t magic;
    uint32_t size;
};

struct arcade_gbc_priv {
    Emulator *emu;
    /* ROM 的只读视图：emulator 接管所有权，但地址不变，调试命令还在用 */
    const uint8_t *rom_view;
    uint32_t rom_size;        /* 卡上的真实大小 */
    uint32_t rom_alloc_size;  /* 向上对齐后的大小（binjgb 读 ROM 用） */
    /* 卡带存档区：binjgb 自己那份在 Emulator 里，这份是给 arcade_save 落盘用 */
    uint8_t *cart_ram;
    uint32_t cart_ram_size;
    bool is_cgb;
    bool first_frame_logged;
    uint32_t frame_n;
    /* 帧边界 PC 直方图：集中=死循环，分散=正常跑 */
    uint16_t hist_pc[PC_HIST_SLOTS];
    uint16_t hist_sp[PC_HIST_SLOTS];
    uint32_t hist_n[PC_HIST_SLOTS];
    /* 帧耗时拆分（cycle 累计）：emu=模拟器本体，blit=帧缓冲放大 */
    uint64_t perf_emu_cycles;
    uint32_t perf_frames;
};

static struct arcade_gbc_priv *s_priv;
static bool s_error_logged;

/* ---------------- PSRAM 分配包装（binjgb 的 xmalloc/xfree） ---------------- */

static size_t psram_free_bytes(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

void *arcade_bin_alloc(size_t size)
{
    size_t payload;
    size_t total;
    uint8_t *base;
    struct arcade_bin_hdr *hdr;

    if (size == 0) {
        return NULL;
    }

    payload = (size + 7U) & ~(size_t)7U;
    total = payload + sizeof(struct arcade_bin_hdr);

    if (psram_free_bytes() < total + PSRAM_ALLOC_MARGIN) {
        LISA_LOGE(TAG, "psram: need %u B, free only %u B",
                  (unsigned)total, (unsigned)psram_free_bytes());
        return NULL;
    }

    base = (uint8_t *)exram_malloc(8, total);
    if (base == NULL) {
        LISA_LOGE(TAG, "psram: alloc %u B failed", (unsigned)total);
        return NULL;
    }

    hdr = (struct arcade_bin_hdr *)base;
    hdr->magic = ARCADE_BIN_MAGIC;
    hdr->size = (uint32_t)payload;

    return base + sizeof(struct arcade_bin_hdr);
}

void arcade_bin_free(void *ptr)
{
    uint8_t *base;

    if (ptr == NULL) {
        return;
    }

    base = (uint8_t *)ptr - sizeof(struct arcade_bin_hdr);
    if (((struct arcade_bin_hdr *)base)->magic != ARCADE_BIN_MAGIC) {
        /* 不是我们发的指针：宁可不释放也不能让堆崩掉 */
        LISA_LOGE(TAG, "psram: free of foreign pointer %p", ptr);
        return;
    }

    ((struct arcade_bin_hdr *)base)->magic = 0;
    exram_free(base);
}

void *arcade_bin_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr;

    if (count != 0 && total / count != size) {
        return NULL; /* 溢出 */
    }

    ptr = arcade_bin_alloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }

    return ptr;
}

void *arcade_bin_realloc(void *ptr, size_t size)
{
    uint8_t *base;
    uint32_t old_size;
    void *new_ptr;

    if (ptr == NULL) {
        return arcade_bin_alloc(size);
    }

    if (size == 0) {
        arcade_bin_free(ptr);
        return NULL;
    }

    base = (uint8_t *)ptr - sizeof(struct arcade_bin_hdr);
    if (((struct arcade_bin_hdr *)base)->magic != ARCADE_BIN_MAGIC) {
        LISA_LOGE(TAG, "psram: realloc of foreign pointer %p", ptr);
        return NULL;
    }

    old_size = ((struct arcade_bin_hdr *)base)->size;
    new_ptr = arcade_bin_alloc(size);
    if (new_ptr == NULL) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    arcade_bin_free(ptr);

    return new_ptr;
}

/* ---------------- ROM 载入 ---------------- */

static uint32_t rom_padded_size(uint32_t size)
{
    uint32_t padded = ROM_BANK_BYTES;

    while (padded < size && padded < ROM_SIZE_LIMIT) {
        padded <<= 1;
    }

    return padded;
}

static uint8_t *rom_load_demo(uint32_t *out_size, uint32_t *out_alloc)
{
    uint32_t padded;
    uint8_t *buf;

    if (arcade_demo_rom_size == 0) {
        LISA_LOGE(TAG, "built-in demo ROM is empty");
        return NULL;
    }

    padded = rom_padded_size(arcade_demo_rom_size);
    buf = (uint8_t *)arcade_bin_alloc(padded);
    if (buf == NULL) {
        return NULL;
    }

    memset(buf, 0xFF, padded);
    memcpy(buf, arcade_demo_rom, arcade_demo_rom_size);

    *out_size = arcade_demo_rom_size;
    *out_alloc = padded;
    return buf;
}

static uint8_t *rom_load_file(const char *path, uint32_t *out_size, uint32_t *out_alloc)
{
    static struct lsfs_file_t file;
    off_t size;
    uint32_t padded;
    uint32_t done = 0;
    uint8_t *buf;

    lsfs_file_t_init(&file);
    if (lsfs_open(&file, path, LSFS_O_READ) != 0) {
        LISA_LOGE(TAG, "open %s failed", path);
        return NULL;
    }

    size = lsfs_lsize(&file);
    if (size <= 0 || (uint32_t)size > ROM_SIZE_LIMIT) {
        LISA_LOGE(TAG, "%s size %ld out of range (max %u)", path, (long)size,
                  (unsigned)ROM_SIZE_LIMIT);
        lsfs_close(&file);
        return NULL;
    }

    padded = rom_padded_size((uint32_t)size);
    buf = (uint8_t *)arcade_bin_alloc(padded);
    if (buf == NULL) {
        LISA_LOGE(TAG, "%s needs %u B in PSRAM", path, (unsigned)padded);
        lsfs_close(&file);
        return NULL;
    }

    memset(buf, 0xFF, padded);

    while (done < (uint32_t)size) {
        ssize_t got = lsfs_read(&file, buf + done, (uint32_t)size - done);

        if (got <= 0) {
            LISA_LOGW(TAG, "short ROM read at %u/%u, rest filled with 0xFF",
                      (unsigned)done, (unsigned)size);
            break;
        }
        done += (uint32_t)got;
    }

    lsfs_close(&file);

    if (done == 0) {
        LISA_LOGE(TAG, "read 0 bytes from %s", path);
        arcade_bin_free(buf);
        return NULL;
    }

    *out_size = (uint32_t)size;
    *out_alloc = padded;
    return buf;
}

/* ---------------- 存档区同步 ---------------- */

static void cart_ram_sync_from_emu(struct arcade_gbc_priv *p)
{
    FileData fd;

    if (p->cart_ram == NULL || p->cart_ram_size == 0) {
        return;
    }

    fd.data = p->cart_ram;
    fd.size = p->cart_ram_size;
    (void)emulator_write_ext_ram(p->emu, &fd);
    arcade_save_mark_dirty();
}

static void cart_ram_setup(struct arcade_gbc_priv *p, const char *path)
{
    FileData probe;
    FileData in;
    uint8_t *buf;

    /* ext RAM 的大小只有卡带头说了算，先用它的接口问一下（会临时分配一次） */
    ZERO_MEMORY(probe);
    emulator_init_ext_ram_file_data(p->emu, &probe);
    if (probe.size == 0) {
        LISA_LOGI(TAG, "cart has no battery-backed RAM, saving disabled");
        arcade_bin_free(probe.data);
        return;
    }

    buf = (uint8_t *)arcade_bin_alloc(probe.size);
    if (buf == NULL) {
        LISA_LOGW(TAG, "no psram for %u B cart RAM, saving disabled",
                  (unsigned)probe.size);
        arcade_bin_free(probe.data);
        return;
    }

    memset(buf, 0, probe.size);
    p->cart_ram = buf;
    p->cart_ram_size = (uint32_t)probe.size;

    /* 先把卡上的 .sav 读进来，再灌给模拟器，这样开机就能读到旧存档 */
    arcade_save_attach(path, buf, p->cart_ram_size);

    in.data = buf;
    in.size = probe.size;
    if (emulator_read_ext_ram(p->emu, &in) != OK) {
        LISA_LOGW(TAG, "existing save file rejected (size mismatch?)");
    }

    arcade_bin_free(probe.data);

    LISA_LOGI(TAG, "cart RAM %u B, save path %s", (unsigned)p->cart_ram_size,
              arcade_save_path() != NULL ? arcade_save_path() : "(none)");
}

/* ---------------- 调试统计 ---------------- */

static void hist_sample(struct arcade_gbc_priv *p, uint16_t pc, uint16_t sp)
{
    uint32_t slot = ((uint32_t)pc * 2654435761u) >> 27;

    if (p->hist_n[slot] != 0 && p->hist_pc[slot] == pc) {
        p->hist_n[slot]++;
        p->hist_sp[slot] = sp;
        return;
    }

    p->hist_pc[slot] = pc;
    p->hist_sp[slot] = sp;
    p->hist_n[slot] = 1;
}

/* ---------------- 对外接口 ---------------- */

int arcade_gb_start(const char *path)
{
    EmulatorInit init;
    Emulator *emu;
    struct arcade_gbc_priv *p;
    uint8_t *rom;
    uint32_t rom_size = 0;
    uint32_t rom_alloc = 0;
    size_t free_bytes;

    arcade_gb_stop();

    if (path == NULL) {
        return -1;
    }

    free_bytes = psram_free_bytes();
    LISA_LOGI(TAG, "loading %s, psram free %u KB", path, (unsigned)(free_bytes / 1024));

    p = (struct arcade_gbc_priv *)arcade_bin_alloc(sizeof(*p));
    if (p == NULL) {
        return -1;
    }
    memset(p, 0, sizeof(*p));

    if (strcmp(path, "@demo") == 0) {
        rom = rom_load_demo(&rom_size, &rom_alloc);
    } else {
        rom = rom_load_file(path, &rom_size, &rom_alloc);
    }

    if (rom == NULL) {
        arcade_bin_free(p);
        return -1;
    }

    LISA_LOGI(TAG, "rom loaded: %u B (alloc %u B), psram free %u KB",
              (unsigned)rom_size, (unsigned)rom_alloc,
              (unsigned)(psram_free_bytes() / 1024));

    /*
     * 模拟器自己的状态（含帧缓冲）约 300KB（SGB 边框缓冲已改成按需分配）。
     * 先判断余量：emulator_new() 里一旦分配失败，ROM 的所有权已经在它手上，
     * 我们没法再回收，所以宁愿在进去之前就报错。
     */
    if (psram_free_bytes() < 400U * 1024U) {
        LISA_LOGE(TAG, "not enough psram for emulator state (%u KB free)",
                  (unsigned)(psram_free_bytes() / 1024));
        arcade_bin_free(rom);
        arcade_bin_free(p);
        return -1;
    }

    ZERO_MEMORY(init);
    init.rom.data = rom;
    init.rom.size = rom_alloc;
    init.audio_frequency = ARCADE_AUDIO_FREQUENCY;
    init.audio_frames = ARCADE_AUDIO_FRAMES;
    init.random_seed = 0x5eed1234;
    init.builtin_palette = 0;
    init.force_dmg = FALSE;
    init.cgb_color_curve = CGB_COLOR_CURVE_NONE;

    emu = emulator_new(&init);
    /*
     * 不管成功与否，ROM 缓冲的所有权都已经被 emulator_new() 拿走
     * （set_rom_file_data 只在我们保证「大小是 32KB 整数倍且非空」时才会失败，
     * 而 rom_padded_size() 保证了这一点），失败时它内部已经 xfree 掉。
     */
    if (emu == NULL) {
        LISA_LOGE(TAG, "emulator_new failed (out of psram?)");
        arcade_bin_free(p);
        return -1;
    }

    p->emu = emu;
    p->rom_view = rom;
    p->rom_size = rom_size;
    p->rom_alloc_size = rom_alloc;

    /* 打开 APU 的全部 4 个声道（binjgb 默认是开的，这里显式确认一次） */
    EmulatorConfig config = emulator_get_config(emu);
    for (int i = 0; i < APU_CHANNEL_COUNT; i++) {
        config.disable_sound[i] = FALSE;
    }
    emulator_set_config(emu, &config);

    /* DMG 游戏用掌机绿，CGB 游戏用游戏自己的 15bit 色 */
    {
        const uint8_t (*rgb)[3] = arcade_video_dmg_palette_rgb();
        PaletteRGBA dmg;

        for (int i = 0; i < 4; i++) {
            dmg.color[i] = MAKE_RGBA(rgb[i][0], rgb[i][1], rgb[i][2], 255);
        }
        emulator_set_all_bw_palettes(emu, &dmg);
    }

    cart_ram_setup(p, path);

    EmulatorArcadeSnapshot snap;
    emulator_arcade_snapshot(emu, &snap);
    p->is_cgb = snap.is_cgb;

    s_priv = p;
    s_error_logged = false;

    LISA_LOGI(TAG, "binjgb started: %s, cgb=%d sgb=%d, psram free %u KB",
              path, p->is_cgb ? 1 : 0, snap.is_sgb ? 1 : 0,
              (unsigned)(psram_free_bytes() / 1024));

    return 0;
}

void arcade_gb_stop(void)
{
    struct arcade_gbc_priv *p = s_priv;

    s_priv = NULL;

    if (p == NULL) {
        return;
    }

    if (p->emu != NULL) {
        /* 退出、换 ROM 前先把模拟器里的存档区同步出来再落盘 */
        cart_ram_sync_from_emu(p);
        (void)arcade_save_flush();
        arcade_save_detach();
        /* 这里会把 ROM 缓冲和我们通过 xmalloc 发出去的块一起释放 */
        emulator_delete(p->emu);
        p->emu = NULL;
        p->rom_view = NULL;
    }

    if (p->cart_ram != NULL) {
        arcade_bin_free(p->cart_ram);
        p->cart_ram = NULL;
    }

    arcade_bin_free(p);
}

bool arcade_gb_running(void)
{
    return s_priv != NULL && s_priv->emu != NULL;
}

static void stats_sample(struct arcade_gbc_priv *p)
{
    EmulatorArcadeSnapshot snap;

    /*
     * PC/SP 直方图是调试用的：emulator_arcade_snapshot() 每次都把二十几个字段
     * 从 PSRAM 的 Emulator 里读一遍，而这个 Emulator 每帧都在搅 D-Cache。
     * 每帧都采没有额外信息量，改成每 8 帧一次，把这笔开销从热路径上挪走。
     */
    if ((p->frame_n & 7U) != 0) {
        p->frame_n++;
        return;
    }

    emulator_arcade_snapshot(p->emu, &snap);
    p->frame_n++;
    hist_sample(p, snap.pc, snap.sp);
}

void arcade_gb_run_frame(void)
{
    struct arcade_gbc_priv *p = s_priv;
    Ticks until;
    EmulatorEvent ev;
    int guard = 0;
    uint32_t t0;
    uint32_t t1;

    if (p == NULL || p->emu == NULL) {
        return;
    }

    /*
     * 先把这一帧的渲染目标要到手：video 模块的源帧双缓冲让缩放（1.9ms）
     * 和模拟并行，所以 PPU 得直接写进那块缓冲，而不是模拟器自己的。
     * 拿不到就退回模拟器内部缓冲（只少一帧画面，不影响运行）。
     */
    arcade_fbpixel_t *fb = arcade_video_begin_frame();
    /* 拿不到源缓冲时要显式退回模拟器内部缓冲：否则 PPU 可能继续写上一帧
     * 还在被缩放任务读的那块，画面会撕裂 */
    emulator_set_frame_buffer(p->emu, (FBPixel *)fb);

    t0 = arcade_cycles();

    /*
     * 跑到「当前 tick + 一帧」为止。返回 AUDIO_BUFFER_FULL 之类的事件时
     * 继续跑，直到走到目标 tick，保证每次调用一定前进一帧（不会自旋）。
     */
    until = emulator_get_ticks(p->emu) + PPU_FRAME_TICKS;
    do {
        ev = emulator_run_until(p->emu, until);
        guard++;
    } while (!(ev & (EMULATOR_EVENT_UNTIL_TICKS | EMULATOR_EVENT_BREAKPOINT |
                     EMULATOR_EVENT_INVALID_OPCODE)) &&
             guard < 64);

    if (ev & EMULATOR_EVENT_INVALID_OPCODE) {
        if (!s_error_logged) {
            LISA_LOGW(TAG, "invalid opcode, emulation keeps running");
            s_error_logged = true;
        }
    }

    /*
     * 把这一帧的 APU 输出推给喇叭，然后把 binjgb 的写游标复位。
     * 必须每帧取走：AudioBuffer 满了以后，下一次 emulator_run_until()
     * 会直接把 position 拨回 data（那几个采样就丢了）；我们自己复位
     * 才能保证「产出的每个采样都被送到音频链路」。
     */
    {
        AudioBuffer *ab = emulator_get_audio_buffer(p->emu);
        uint32_t aframes = audio_buffer_get_frames(ab);

        if (aframes > 0) {
            arcade_audio_push(ab->data, aframes);
            ab->position = ab->data;
        }
    }

    /* 游戏真写了卡带存档区才同步 + 标脏，避免每帧 memcpy 32KB */
    if (emulator_was_ext_ram_updated(p->emu)) {
        cart_ram_sync_from_emu(p);
    }

    t1 = arcade_cycles();
    /* 缩放 + 送屏交给 video 模块的三级流水，这里只记模拟器本体耗时。
     * 屏上有 LVGL 提示文字时不缩放也不送帧，否则会把提示盖掉。 */
    arcade_video_end_frame(
        fb != NULL && !arcade_ui_player_message_visible());
    p->perf_emu_cycles += (uint32_t)(t1 - t0);
    p->perf_frames++;

    stats_sample(p);

    if (!p->first_frame_logged) {
        p->first_frame_logged = true;
        LISA_LOGI(TAG, "first frame done, psram free %u KB",
                  (unsigned)(psram_free_bytes() / 1024));
    }
}

void arcade_gb_set_input(uint32_t mask)
{
    struct arcade_gbc_priv *p = s_priv;
    JoypadButtons buttons;

    if (p == NULL || p->emu == NULL) {
        return;
    }

    /* binjgb 是「true = 按下」，和 ARCADE_KEY_* 语义一致，不用反相 */
    buttons.A = (mask & ARCADE_KEY_A) != 0;
    buttons.B = (mask & ARCADE_KEY_B) != 0;
    buttons.select = (mask & ARCADE_KEY_SELECT) != 0;
    buttons.start = (mask & ARCADE_KEY_START) != 0;
    buttons.right = (mask & ARCADE_KEY_RIGHT) != 0;
    buttons.left = (mask & ARCADE_KEY_LEFT) != 0;
    buttons.up = (mask & ARCADE_KEY_UP) != 0;
    buttons.down = (mask & ARCADE_KEY_DOWN) != 0;

    emulator_set_joypad_buttons(p->emu, &buttons);
}

uint32_t arcade_gb_rom_checksum(void)
{
    uint32_t hash = 2166136261u;

    if (s_priv == NULL || s_priv->rom_view == NULL || s_priv->rom_size == 0) {
        return 0;
    }

    for (uint32_t addr = 0; addr < s_priv->rom_size; addr++) {
        hash ^= s_priv->rom_view[addr];
        hash *= 16777619u;
    }

    return hash;
}

int arcade_gb_rom_header(char *out, uint32_t out_len)
{
    char title[17];
    const uint8_t *rom;

    if (s_priv == NULL || s_priv->rom_view == NULL || out == NULL || out_len < 48) {
        return -1;
    }

    rom = s_priv->rom_view;

    for (int i = 0; i < 16; i++) {
        uint8_t c = rom[0x0134 + i];

        title[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    title[16] = '\0';

    return snprintf(out, out_len, "title=[%s] cart=0x%02X rom_size=0x%02X cgb=0x%02X",
                    title, (unsigned)rom[0x0147], (unsigned)rom[0x0148],
                    (unsigned)rom[0x0143]);
}

int arcade_gb_machine_report(char *out, uint32_t out_len)
{
    EmulatorArcadeSnapshot s;
    int n;

    if (s_priv == NULL || s_priv->emu == NULL || out == NULL || out_len < 128) {
        return -1;
    }

    emulator_arcade_snapshot(s_priv->emu, &s);

    n = snprintf(out, out_len,
                 "pc=%04X sp=%04X af=%04X bc=%04X de=%04X hl=%04X\n",
                 (unsigned)s.pc, (unsigned)s.sp, (unsigned)s.af,
                 (unsigned)s.bc, (unsigned)s.de, (unsigned)s.hl);

    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "lcdc=%02X stat=%02X ly=%02X scx=%02X scy=%02X bgp=%02X\n",
                  (unsigned)s.lcdc, (unsigned)s.stat, (unsigned)s.ly,
                  (unsigned)s.scx, (unsigned)s.scy, (unsigned)s.bgp);

    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    n += snprintf(out + n, out_len - (uint32_t)n,
                  "cgb=%d sgb=%d vrambank=%u wrambank=%u ie=%02X if=%02X joy=%02X\n",
                  s.is_cgb ? 1 : 0, s.is_sgb ? 1 : 0,
                  (unsigned)s.vram_bank, (unsigned)s.wram_bank,
                  (unsigned)s.ie, (unsigned)s.iflag, (unsigned)s.joyp);

    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    /* binjgb 是 tick 驱动的，PC 分布 + ppu_frame 增长就能判断「在跑 / 卡死」 */
    n += snprintf(out + n, out_len - (uint32_t)n,
                  "frames=%u ppuframe=%u ticks=%u save=%s\n",
                  (unsigned)s_priv->frame_n, (unsigned)s.ppu_frame,
                  (unsigned)(s.ticks >> 10),
                  arcade_save_pending() ? "pending" : "clean");

    if (n < 0 || (uint32_t)n >= out_len) {
        return n;
    }

    /* 帧耗时：emu 是模拟器本体（video 模块自己的 blit/送屏计时单独报） */
    {
        uint32_t f = s_priv->perf_frames ? s_priv->perf_frames : 1;
        uint32_t emu_us = arcade_cyc_to_us(s_priv->perf_emu_cycles / f);
        char video_perf[192];

        arcade_video_perf(video_perf, sizeof(video_perf));

        n += snprintf(out + n, out_len - (uint32_t)n,
                      "perf emu=%u.%ums of %u frames\n%s",
                      (unsigned)(emu_us / 1000), (unsigned)((emu_us / 100) % 10),
                      (unsigned)f, video_perf);
    }

    return n;
}

int arcade_gb_hist_report(char *out, uint32_t out_len)
{
    uint32_t order[PC_HIST_SLOTS];
    uint32_t total = 0;
    int used = 0;
    int off = 0;

    if (s_priv == NULL || out == NULL || out_len < 64) {
        return -1;
    }

    for (int i = 0; i < PC_HIST_SLOTS; i++) {
        if (s_priv->hist_n[i] == 0) {
            continue;
        }
        order[used++] = (uint32_t)i;
        total += s_priv->hist_n[i];
    }

    for (int i = 1; i < used; i++) {
        uint32_t key = order[i];
        int j = i - 1;

        while (j >= 0 && s_priv->hist_n[order[j]] < s_priv->hist_n[key]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    off += snprintf(out + off, out_len - (uint32_t)off,
                    "frames=%u slots=%d total=%u\n",
                    (unsigned)s_priv->frame_n, used, (unsigned)total);

    for (int i = 0; i < used && i < 16 && off + 40 < (int)out_len; i++) {
        uint32_t k = order[i];

        off += snprintf(out + off, out_len - (uint32_t)off,
                        "pc=%04X sp=%04X n=%-6u %u%%\n",
                        (unsigned)s_priv->hist_pc[k], (unsigned)s_priv->hist_sp[k],
                        (unsigned)s_priv->hist_n[k],
                        (unsigned)(total ? (s_priv->hist_n[k] * 100u) / total : 0));
    }

    return off;
}

/* 清零帧耗时统计（`arcade perf reset` / apu / bg 切换前后对比用） */
void arcade_gb_reset_perf(void)
{
    if (s_priv == NULL) {
        return;
    }

    s_priv->perf_emu_cycles = 0;
    s_priv->perf_frames = 0;
}

int arcade_gb_peek(char *out, uint32_t out_len, uint32_t addr, uint32_t len)
{    uint32_t off = 0;

    if (s_priv == NULL || s_priv->emu == NULL || out == NULL || out_len < 16) {
        return -1;
    }

    if (len == 0 || len > 256) {
        len = 64;
    }

    for (uint32_t i = 0; i < len && off + 4 < out_len; i++) {
        off += (uint32_t)snprintf(out + off, out_len - off, "%02X ",
                                  (unsigned)emulator_arcade_read(s_priv->emu,
                                                                 (uint16_t)((addr + i) & 0xFFFF)));
        if (((addr + i) & 0x0F) == 0x0F && off + 12 < out_len) {
            off += (uint32_t)snprintf(out + off, out_len - off, "\n");
        }
    }

    return (int)off;
}

/*
 * 调试：开关 APU 的 4 个声道。
 *
 * 用来量「声音占了 simulate 里多少时间」：关掉 APU 后 emu 耗时掉多少，
 * 就是 APU 的成本。binjgb 的 APU 是按输出采样率逐个采样算的，48kHz 下
 * 每帧 800 个采样 × 4 声道，是候选热点之一。
 */
int arcade_gb_set_audio_enabled(bool on)
{
    EmulatorConfig config;

    if (s_priv == NULL || s_priv->emu == NULL) {
        return -1;
    }

    config = emulator_get_config(s_priv->emu);
    for (int i = 0; i < APU_CHANNEL_COUNT; i++) {
        config.disable_sound[i] = on ? FALSE : TRUE;
    }
    emulator_set_config(s_priv->emu, &config);

    return 0;
}

/*
 * 调试：只关掉 PIXEL 渲染（BG / window / OBJ），模拟照旧推进。
 *
 * `arcade bg 0` 之后 emu 耗时掉了多少，就是 PPU 画像素的成本；
 * 剩下的是 CPU 解释器 + 每 tick 的状态机。两者分开才能知道该优化哪边。
 */
int arcade_gb_set_render_enabled(bool on)
{
    EmulatorConfig config;

    if (s_priv == NULL || s_priv->emu == NULL) {
        return -1;
    }

    config = emulator_get_config(s_priv->emu);
    config.disable_bg = on ? FALSE : TRUE;
    config.disable_window = on ? FALSE : TRUE;
    config.disable_obj = on ? FALSE : TRUE;
    emulator_set_config(s_priv->emu, &config);

    return 0;
}
