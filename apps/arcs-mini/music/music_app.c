/*
 * 网易云音乐应用层实现（里程碑 2），见 music_app.h 的说明。
 *
 * 线程模型：
 *  - music.app 工作线程：所有网络请求 + 播放控制 + 歌词解析 + 扫码轮询；
 *  - LVGL 线程：只通过 music_app_get_model() 读快照，不直接碰本模块状态；
 *  - shell 线程：提交请求 + 等 done 信号 + 逐行打印（任务栈只有 1 KB）。
 *
 * 内存：状态、歌单、歌词文本全部放 .psram.bss（SRAM 只剩十几 KB）；
 * 封面 JPEG 直接复用 music_http 的响应缓冲（PSRAM 堆），换歌时释放上一张。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define TAG "music.app"
#include "lisa_log.h"

#include "app_player.h"
#include "esp_heap_caps.h"
#include "lisa_kv.h"
#include "lisa_ui_invoke.h"
#include "service_volume.h"
#include "shell.h"
#include "sys_network_manager.h"
#include "sys_wifi.h"
#include "sysutils.h"
#include "voice_player_comm.h"

#include "music_app.h"
#include "music_http.h"
#include "music_netease.h"
#include "music_ui.h"
#include "voice_msg.h"

#ifdef CONFIG_NETEASE_MUSIC_ONLY
/* 单应用固件：关机 / 蓝牙配网都自己接管（没有 radio 模块帮忙） */
#include "app_ble_common.h"
#include "power_manager.h"
#endif

/* ==================== 配置 ==================== */

/* 工作线程栈：EAPI 一个请求帧里要放 body[1024] + headers[cookie] + json，
 * 给到 8 KB（多 2 KB 换掉栈溢出风险，SRAM 由堆里出）。 */
#define MUSIC_TASK_STACK 8192
#define MUSIC_TASK_PRIO  4
/* 状态机轮询周期（进度 / 歌词游标 / 扫码轮询 / 开机进页） */
#define MUSIC_TICK_MS 200
/* shell 等结果的最长时间：一次请求里可能串行 3 个 HTTP（直链 + 歌词 + 封面） */
#define MUSIC_CMD_WAIT_MS 60000

#define MUSIC_SONGS_MAX MUSIC_SONG_MAX
/* 封面请求尺寸：服务端按 ?param=WxH 缩放，100x100 正好是播放页的显示区 */
#define MUSIC_COVER_PARAM   "?param=100y100"
/* 解不出 SOF 尺寸时的兜底（正常就是 param 请求的 100） */
#define COVER_FALLBACK_W    100u
#define COVER_FALLBACK_H    100u
/* 歌词原文缓冲（LRC 一般 3~8 KB） */
#define MUSIC_LRC_CAP       (12 * 1024)
#define MUSIC_LYRIC_MAX     64
/* 扫码轮询节奏 / 二维码有效期 */
#define MUSIC_QR_POLL_MS    2000
#define MUSIC_QR_TIMEOUT_MS (3u * 60u * 1000u)

#define MUSIC_VOLUME_STEP 5

#ifdef CONFIG_NETEASE_MUSIC_ONLY
/* 单应用固件里没有 radio 模块，开机按键保护要自己管：
 * 用户是「长按功能键 3 s」开机的，松手那一下会被按键驱动识别成 CLICK / LONG_HOLD，
 * 不忽略就会「刚开机就切歌」甚至「刚开机又关机」。 */
#define MUSIC_BOOT_KEY_GUARD_MS 6000

static void music_netcfg_enter(void);
#endif

#define MUSIC_KV_KEY_COOKIE "music.cookie"

/* shell 结果行：SHELL_PRINT_BUFFER 是 128，留一点余量 */
#define MUSIC_LOG_LINES 20
#define MUSIC_LOG_LINE  120

/* 歌单二级菜单：我的歌单之后保留 3 个推荐榜单。 */
static const struct {
    const char *name;
    int64_t id;
} s_playlists[] = {
    {"热歌榜", 3778678},
    {"新歌榜", 3779629},
    {"飙升榜", 19723756},
};
#define MUSIC_PLAYLIST_COUNT ((uint8_t)(sizeof(s_playlists) / sizeof(s_playlists[0])))

/*
 * 主菜单固定四项；歌单二级菜单按登录结果动态加入「我的歌单」。
 * 设置二级菜单通过现有音量服务调节并保存音量。
 */
typedef enum {
    MUSIC_ITEM_MY = 0, /* 登录用户自己的歌单（index 指向 s_state.my） */
    MUSIC_ITEM_CHART,  /* 官方榜单（index 指向 s_playlists） */
    MUSIC_ITEM_LYRIC,
    MUSIC_ITEM_LOGIN,
    MUSIC_ITEM_PLAYLISTS,
    MUSIC_ITEM_SETTINGS,
    MUSIC_ITEM_VOLUME_UP,
    MUSIC_ITEM_VOLUME_DOWN,
} music_item_kind_t;

typedef struct {
    uint8_t kind;  /* music_item_kind_t */
    uint8_t index;
} music_item_t;

/* ==================== 请求 ==================== */

typedef enum {
    MUSIC_REQ_NONE = 0,
    MUSIC_REQ_PLAYLIST,  /* 加载歌单（arg = 导航页索引） */
    MUSIC_REQ_PLAY,      /* 播放歌曲（arg = 歌曲下标） */
    MUSIC_REQ_NEXT,      /* 下一首（arg 忽略） */
    MUSIC_REQ_RETRY,     /* 上一首/重试（arg = 下标） */
    MUSIC_REQ_LYRIC,     /* 只重拉歌词 */
    MUSIC_REQ_QR_START,  /* 申请二维码 */
    MUSIC_REQ_QR_POLL,   /* 手动查询一次登录状态 */
    MUSIC_REQ_ACCOUNT,   /* 登录后取账号信息 + 我的歌单 */
    MUSIC_REQ_SEARCH,    /* shell：搜索 */
    MUSIC_REQ_CHAIN,     /* shell：全链路 */
} music_req_kind_t;

/* ==================== 状态 ==================== */

typedef struct {
    bool inited;
    bool started;      /* 开机自动进音乐页只做一次 */
    uint8_t page;      /* music_page_t */
    uint8_t menu_index;
    uint8_t menu_level; /* music_menu_level_t */
    uint8_t menu_selection[MUSIC_MENU_LEVEL_COUNT];
    uint8_t list_index;

    /* 当前歌单 */
    char list_title[MUSIC_TITLE_MAX];
    int64_t playlist_id;
    uint8_t count;
    uint8_t current;

    /* 播放 */
    bool user_paused;
    volatile bool playing;
    uint32_t pos_ms;
    uint32_t dur_ms;
    char url[MUSIC_URL_MAX];

    /* 歌词 */
    uint8_t lyric_count;
    int8_t lyric_index;
    uint32_t lyric_time[MUSIC_LYRIC_MAX];

    /* 封面：art.body 就是 JPEG 原件（PSRAM 堆，换歌时释放） */
    uint32_t art_gen;
    uint16_t art_w;
    uint16_t art_h;
    music_http_resp_t art;

    /* 登录 */
    uint16_t qr_state;
    uint32_t qr_gen;
    uint32_t qr_at_ms;
    uint32_t qr_start_ms;
    char unikey[64];
    char cookie[MUSIC_HTTP_JAR_MAX];
    char qr_url[MUSIC_QR_URL_MAX];

    /* 登录账号 + 「我的歌单」（扫码成功后才会有内容） */
    int64_t uid;
    bool account_loaded;
    bool account_pending; /* 开机恢复过 cookie，联网后要补拉账号/我的歌单 */
    char nickname[MUSIC_NAME_MAX];
    music_user_playlist_list_t my;

    /* 杂项 */
    volatile bool net_ok;
    bool busy;
    uint32_t boot_ms;
    char detail[MUSIC_SUB_MAX];
    uint32_t detail_ms; /* detail 的设置时刻（成功类提示几秒后自动消失） */
    bool detail_transient;
} music_state_t;

static music_state_t s_state __psram_bss__;
static music_song_t s_songs[MUSIC_SONGS_MAX] __psram_bss__;
/* 详情接口的输出缓冲：10 首 * ~370 字节，不能放 6 KB 的工作线程栈里 */
static music_song_list_t s_song_buf __psram_bss__;
/* 接口输出缓冲：登录账号 / 我的歌单（12 个 × 76 字节，不能放 8 KB 工作线程栈） */
static music_user_playlist_list_t s_my_buf __psram_bss__;
static char s_lyric_text[MUSIC_LYRIC_MAX][MUSIC_LYRIC_LINE] __psram_bss__;
static char s_lrc_raw[MUSIC_LRC_CAP] __psram_bss__;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_signal;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;

static struct {
    volatile music_req_kind_t kind;
    uint8_t arg;
    int64_t playlist_id; /* MUSIC_REQ_PLAYLIST：要加载的歌单 id */
    volatile bool busy;
    volatile bool next_pending;
    int err;
    char keyword[MUSIC_NAME_MAX];
    char title[MUSIC_TITLE_MAX]; /* MUSIC_REQ_PLAYLIST：列表标题 */
    int line_count;
    char lines[MUSIC_LOG_LINES][MUSIC_LOG_LINE];
} s_req __psram_bss__;

/* ==================== 小工具 ==================== */

static uint32_t music_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void music_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void music_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static void music_set_detail(const char *text)
{
    snprintf(s_state.detail, sizeof(s_state.detail), "%s", (text != NULL) ? text : "");
    s_state.detail_ms = music_now_ms();
    s_state.detail_transient = false;
}

static void music_set_detailf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_state.detail, sizeof(s_state.detail), fmt, args);
    va_end(args);
    s_state.detail_ms = music_now_ms();
    s_state.detail_transient = false;
}

/* 一次性提示（例如“登录成功”）：几秒后自己消失，不长期占着屏幕 */
static void music_set_detail_info(const char *text)
{
    music_set_detail(text);
    s_state.detail_transient = true;
}

static const music_song_t *music_song_locked(uint8_t index)
{
    if (index >= s_state.count) {
        return NULL;
    }
    return &s_songs[index];
}

/* ==================== 导航页条目 ==================== */

/* 当前层级条目总数（锁内调用）。 */
static uint8_t music_menu_count_locked(void)
{
    switch ((music_menu_level_t)s_state.menu_level) {
    case MUSIC_MENU_PLAYLISTS:
        return (uint8_t)(s_state.my.count + MUSIC_PLAYLIST_COUNT);
    case MUSIC_MENU_SETTINGS:
        return 2u;
    case MUSIC_MENU_ROOT:
    default:
        return 4u;
    }
}

static void music_menu_normalize_locked(void)
{
    if (s_state.menu_level >= MUSIC_MENU_LEVEL_COUNT) {
        s_state.menu_level = MUSIC_MENU_ROOT;
    }
    if (s_state.menu_index >= music_menu_count_locked()) {
        s_state.menu_index = 0;
    }
    s_state.menu_selection[s_state.menu_level] = s_state.menu_index;
}

static void music_menu_open_locked(music_menu_level_t level)
{
    music_menu_normalize_locked();
    s_state.menu_level = (uint8_t)level;
    s_state.menu_index = s_state.menu_selection[level];
    music_menu_normalize_locked();
    s_state.page = MUSIC_PAGE_MENU;
}

/* 把菜单下标翻译成具体条目（锁内调用） */
static bool music_menu_item_locked(uint8_t index, music_item_t *out)
{
    uint8_t rest;

    if (out == NULL) {
        return false;
    }
    if (s_state.menu_level == MUSIC_MENU_ROOT) {
        static const music_item_kind_t root_items[] = {
            MUSIC_ITEM_PLAYLISTS, MUSIC_ITEM_LYRIC, MUSIC_ITEM_LOGIN, MUSIC_ITEM_SETTINGS,
        };
        if (index >= sizeof(root_items) / sizeof(root_items[0])) {
            return false;
        }
        out->kind = (uint8_t)root_items[index];
        out->index = 0;
        return true;
    }
    if (s_state.menu_level == MUSIC_MENU_SETTINGS) {
        if (index >= 2u) {
            return false;
        }
        out->kind = (index == 0) ? MUSIC_ITEM_VOLUME_UP : MUSIC_ITEM_VOLUME_DOWN;
        out->index = 0;
        return true;
    }
    if (s_state.menu_level != MUSIC_MENU_PLAYLISTS) {
        return false;
    }
    if (index < s_state.my.count) {
        out->kind = MUSIC_ITEM_MY;
        out->index = index;
        return true;
    }
    rest = (uint8_t)(index - s_state.my.count);
    if (rest < MUSIC_PLAYLIST_COUNT) {
        out->kind = MUSIC_ITEM_CHART;
        out->index = rest;
        return true;
    }
    return false;
}

/* ==================== shell 结果行 ==================== */

static void log_reset(void)
{
    s_req.line_count = 0;
}

static void log_line(const char *fmt, ...)
{
    va_list args;

    if (s_req.line_count >= MUSIC_LOG_LINES) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(s_req.lines[s_req.line_count], MUSIC_LOG_LINE, fmt, args);
    va_end(args);
    s_req.line_count++;
}

/* ==================== 歌词 ==================== */

static void music_lyric_reset(void)
{
    s_state.lyric_count = 0;
    s_state.lyric_index = -1;
    s_lyric_text[0][0] = '\0';
}

/* 把 "mm:ss.xx" / "mm:ss:xx" 解析成毫秒；失败返回 -1 */
static int32_t music_parse_stamp(const char *text, const char **end)
{
    int mm = 0;
    int ss = 0;
    int cc = 0;
    const char *p = text;

    while (*p >= '0' && *p <= '9') {
        mm = mm * 10 + (*p - '0');
        p++;
    }
    if (*p != ':' || mm > 999) {
        return -1;
    }
    p++;
    if (!(*p >= '0' && *p <= '9')) {
        return -1;
    }
    while (*p >= '0' && *p <= '9') {
        ss = ss * 10 + (*p - '0');
        p++;
    }
    if (*p == '.' || *p == ':') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            cc = cc * 10 + (*p - '0');
            p++;
            digits++;
        }
        while (digits < 3) { /* .5 表示 500ms */
            cc *= 10;
            digits++;
        }
    }
    if (end != NULL) {
        *end = p;
    }
    return mm * 60 * 1000 + ss * 1000 + cc;
}

/*
 * LRC 头几行常常是制作人名单（网易云也带时间戳："[00:00.000] 作词 : xxx"）。
 * 认得出来的就丢掉，否则播放页的「当前歌词」会一直停在作词人上。
 * 判定条件：以名单词开头，且 12 字节内出现冒号（避免误杀普通歌词句子）。
 */
static bool lrc_is_credit(const char *text, size_t len)
{
    static const char *const words[] = {"作词", "作曲", "编曲", "制作", "混音", "母带",
                                        "录音", "监制", "出品", "发行", "统筹", "吉他",
                                        "贝斯", "键盘", "和声", "弦乐", "人声", "词曲",
                                        "OP", "SP"};

    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        const size_t n = strlen(words[w]);

        if (len <= n || strncmp(text, words[w], n) != 0) {
            continue;
        }
        for (size_t i = n; i < len && i < n + 12; i++) {
            if (text[i] == ':') {
                return true;
            }
        }
    }
    return false;
}

/*
 * 解析 LRC。网易云的 LRC 每行形如：
 *   [00:12.340]作词 : 某某
 *   [00:12.340][01:20.500]重复的副歌
 * 这里一行只取第一个时间戳（重复行会自然丢一半，播放时视觉上没问题），
 * 非时间戳行（[ti:] / [ar:] / 空行）与制作人名单直接跳过。
 */
static void music_parse_lrc(const char *lrc)
{
    const char *p = lrc;
    uint8_t count = 0;

    music_lyric_reset();
    if (lrc == NULL) {
        return;
    }

    while (*p != '\0' && count < MUSIC_LYRIC_MAX) {
        const char *line_end = strchr(p, '\n');
        const char *next = (line_end != NULL) ? line_end + 1 : p + strlen(p);
        const int32_t stamp = (*p == '[') ? music_parse_stamp(p + 1, NULL) : -1;

        if (stamp >= 0) {
            const char *close = strchr(p, ']');
            if (close != NULL && close < next) {
                const char *text = close + 1;
                size_t len = (size_t)(next - text);

                while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == ' ')) {
                    len--;
                }
                if (len > 0 && !lrc_is_credit(text, len) && count < MUSIC_LYRIC_MAX) {
                    if (len >= MUSIC_LYRIC_LINE) {
                        len = MUSIC_LYRIC_LINE - 1;
                        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) {
                            len--;
                        }
                    }
                    memcpy(s_lyric_text[count], text, len);
                    s_lyric_text[count][len] = '\0';
                    s_state.lyric_time[count] = (uint32_t)stamp;
                    count++;
                }
            }
        }
        p = next;
    }

    s_state.lyric_count = count;
    s_state.lyric_index = (count > 0) ? 0 : -1;
    LOGI("lyric parsed: %u lines", (unsigned)count);
}

/* 按播放进度定位当前行 */
static void music_lyric_seek(uint32_t pos_ms)
{
    int8_t found = -1;

    for (uint8_t i = 0; i < s_state.lyric_count; i++) {
        if (s_state.lyric_time[i] <= pos_ms) {
            found = (int8_t)i;
        } else {
            break;
        }
    }
    s_state.lyric_index = found;
}

/* ==================== 封面 ==================== */

static void music_art_clear_locked(void)
{
    if (s_state.art.body != NULL) {
        music_http_resp_free(&s_state.art);
    }
    s_state.art_gen++;
}

/*
 * 从 JPEG 头里取宽高（只扫到 SOFn 段）。
 * LVGL 的 RAW/JPEG 解码按 dsc.header.w/h 逐行取数据，尺寸对不上会花屏，
 * 所以不能直接假定服务端一定按 ?param= 给了 100x100。
 */
static bool music_jpeg_size(const uint8_t *data, int len, uint16_t *w, uint16_t *h)
{
    int i = 2;

    while (i + 9 < len) {
        if (data[i] != 0xFF) {
            i++;
            continue;
        }
        const uint8_t marker = data[i + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        const int seg = ((int)data[i + 2] << 8) | data[i + 3];
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC) {
            *h = (uint16_t)(((uint16_t)data[i + 5] << 8) | data[i + 6]);
            *w = (uint16_t)(((uint16_t)data[i + 7] << 8) | data[i + 8]);
            return (*w > 0 && *h > 0);
        }
        if (seg <= 0) {
            return false;
        }
        i += 2 + seg;
    }
    return false;
}

/*
 * 拉封面。服务端返回的就是 JPEG，这里只做「是不是 JPEG」的粗校验，
 * 真正的解码交给 LVGL 的 split-jpeg 解码器（LV_USE_SJPG 已打开，
 * 它内置 TJpgDec，能吃内存里的裸 JPEG）。
 */
static void music_load_cover(const music_song_t *song)
{
    char url[MUSIC_COVER_MAX + 24];
    music_http_resp_t resp;
    const bool has_query = (strchr(song->cover, '?') != NULL);

    if (song->cover[0] == '\0') {
        music_lock();
        music_art_clear_locked();
        music_unlock();
        return;
    }

    snprintf(url, sizeof(url), "%s%s", song->cover, has_query ? "" : MUSIC_COVER_PARAM);
    memset(&resp, 0, sizeof(resp));
    if (music_http_request(url, false, NULL, 0, NULL, &resp) != 0 || resp.status != 200 ||
        resp.body == NULL || resp.length < 4 ||
        (uint8_t)resp.body[0] != 0xFF || (uint8_t)resp.body[1] != 0xD8) {
        LOGW("cover failed: status=%d len=%d", resp.status, resp.length);
        music_http_resp_free(&resp);
        music_lock();
        music_art_clear_locked();
        music_unlock();
        return;
    }

    music_lock();
    if (s_state.art.body != NULL) {
        music_http_resp_free(&s_state.art);
    }
    s_state.art = resp; /* 所有权转移，别在下面 free */
    s_state.art_w = COVER_FALLBACK_W;
    s_state.art_h = COVER_FALLBACK_H;
    {
        uint16_t w = 0;
        uint16_t h = 0;

        if (music_jpeg_size((const uint8_t *)resp.body, resp.length, &w, &h)) {
            s_state.art_w = w;
            s_state.art_h = h;
        }
    }
    s_state.art_gen++;
    music_unlock();
    LOGI("cover ok: %d bytes %ux%u", resp.length, (unsigned)s_state.art_w,
         (unsigned)s_state.art_h);
}

/* ==================== 播放 ==================== */

static int music_start_url(const char *url)
{
    if (music_player == NULL) {
        LOGE("music player instance is NULL");
        return -1;
    }
    if (url == NULL || url[0] == '\0') {
        return -1;
    }
    (void)app_player_stop(music_player);
    const int ret = app_player_play(music_player, url);

    LOGI("play(%d): %s", ret, url);
    return ret;
}

/* 工作线程：播放第 index 首（取直链 → 起播 → 歌词 → 封面） */
static int music_play_index(uint8_t index, bool quiet)
{
    music_song_t song;
    music_track_url_t track;
    int err;

    music_lock();
    const music_song_t *item = music_song_locked(index);
    if (item == NULL) {
        music_set_detail("列表还没有加载");
        music_unlock();
        return MUSIC_ERR_PARAM;
    }
    song = *item;
    s_state.current = index;
    music_unlock();

    err = music_netease_track_url(song.id, &track);
    if (err != MUSIC_OK) {
        LOGW("track url failed: %d(%s) %s", err, music_err_text(err), song.name);
        music_lock();
        if (err == MUSIC_ERR_UNAVAILABLE) {
            music_set_detailf("%s 不可播放", song.name);
        } else {
            music_set_detailf("取播放地址失败 %d", err);
        }
        music_unlock();
        return err;
    }

    music_lock();
    snprintf(s_state.url, sizeof(s_state.url), "%s", track.url);
    s_state.dur_ms = (song.duration_ms > 0) ? (uint32_t)song.duration_ms : 0u;
    s_state.pos_ms = 0;
    s_state.user_paused = false;
    music_set_detail(NULL);
    music_lyric_reset();
    music_unlock();

    if (music_start_url(track.url) != 0) {
        music_lock();
        music_set_detail("播放器起播失败");
        music_unlock();
        return MUSIC_ERR_NET;
    }

    /* 歌词 */
    if (music_netease_lyric(song.id, s_lrc_raw, sizeof(s_lrc_raw)) == MUSIC_OK) {
        music_lock();
        music_parse_lrc(s_lrc_raw);
        music_unlock();
    } else {
        music_lock();
        music_lyric_reset();
        music_unlock();
    }

    /* 封面 */
    music_load_cover(&song);

    if (!quiet) {
        music_lock();
        log_line("play %u. %s / %s", (unsigned)index + 1u, song.name, song.artist);
        log_line(" url=%s", track.url);
        log_line(" lyric=%u lines art=%d bytes", (unsigned)s_state.lyric_count,
                 s_state.art.length);
        music_unlock();
    }
    return MUSIC_OK;
}

/* 下一首；auto_advance = 播完自动切（两种情况下都跳过不可播的曲目） */
static void music_play_next(bool auto_advance)
{
    uint8_t count;
    uint8_t start;

    (void)auto_advance;

    music_lock();
    count = s_state.count;
    start = s_state.current;
    music_unlock();

    if (count == 0) {
        music_lock();
        music_set_detail("列表还没有加载");
        music_unlock();
        return;
    }

    for (uint8_t step = 1; step <= count; step++) {
        const uint8_t index = (uint8_t)((start + step) % count);
        if (music_play_index(index, true) == MUSIC_OK) {
            return;
        }
    }
    music_lock();
    music_set_detail("没有可播放的曲目");
    music_unlock();
}

static void music_play_prev(void)
{
    uint8_t count;
    uint8_t start;

    music_lock();
    count = s_state.count;
    start = s_state.current;
    music_unlock();

    if (count == 0) {
        return;
    }
    for (uint8_t step = 1; step <= count; step++) {
        const uint8_t index = (uint8_t)((start + count - step) % count);
        if (music_play_index(index, true) == MUSIC_OK) {
            return;
        }
    }
}

/* 工作线程：加载歌单（详情拿 id 列表 → 详情接口拿歌名/歌手/封面） */
static void music_load_playlist(int64_t playlist_id, const char *title)
{
    music_playlist_t playlist;
    int err;

    if (playlist_id <= 0) {
        s_req.err = MUSIC_ERR_PARAM;
        return;
    }

    music_lock();
    s_state.busy = true;
    music_set_detail("正在加载歌单 …");
    music_unlock();

    err = music_netease_playlist(playlist_id, &playlist);
    if (err != MUSIC_OK) {
        LOGW("playlist failed: %d(%s)", err, music_err_text(err));
        music_lock();
        s_state.busy = false;
        music_set_detailf("歌单加载失败 %d", err);
        music_unlock();
        s_req.err = err;
        return;
    }

    memset(&s_song_buf, 0, sizeof(s_song_buf));
    if (playlist.id_count > 0) {
        err = music_netease_song_detail(playlist.ids, playlist.id_count, &s_song_buf);
    } else {
        err = MUSIC_ERR_JSON;
    }

    music_lock();
    s_state.busy = false;
    if (err != MUSIC_OK || s_song_buf.count == 0) {
        music_set_detailf("歌曲列表加载失败 %d", err);
        music_unlock();
        s_req.err = err;
        return;
    }
    memset(s_songs, 0, sizeof(s_songs));
    memcpy(s_songs, s_song_buf.items, (size_t)s_song_buf.count * sizeof(music_song_t));
    s_state.count = (uint8_t)s_song_buf.count;
    s_state.list_index = 0;
    s_state.playlist_id = playlist_id;
    snprintf(s_state.list_title, sizeof(s_state.list_title), "%s",
             (title != NULL && title[0] != '\0') ? title : playlist.name);
    music_set_detail(NULL);
    music_unlock();

    LOGI("playlist %s(%lld) loaded: %u songs", playlist.name, (long long)playlist_id,
         (unsigned)s_song_buf.count);
}

/* ==================== 扫码登录 ==================== */

static void music_fetch_account(void);

static void music_qr_start(void)
{
    char key[64];

    music_lock();
    s_state.qr_state = 0;
    music_set_detail("正在申请二维码 …");
    music_unlock();

    const int err = music_netease_qr_start(key, sizeof(key));
    music_lock();
    if (err != MUSIC_OK) {
        music_set_detailf("二维码申请失败 %d", err);
        music_unlock();
        return;
    }
    snprintf(s_state.unikey, sizeof(s_state.unikey), "%s", key);
    snprintf(s_state.qr_url, sizeof(s_state.qr_url), "https://music.163.com/login?codekey=%s",
             key);
    s_state.qr_state = MUSIC_QR_WAIT_SCAN;
    s_state.qr_gen++;
    s_state.qr_at_ms = music_now_ms();
    s_state.qr_start_ms = s_state.qr_at_ms;
    music_set_detail(NULL);
    music_unlock();
    LOGI("qr started: key=%s", key);
}

/* 查询一次登录状态（工作线程/轮询都走它） */
static void music_qr_poll_once(void)
{
    char key[64];
    bool got_login = false;
    int state = MUSIC_QR_WAIT_SCAN;

    music_lock();
    snprintf(key, sizeof(key), "%s", s_state.unikey);
    /* 已经登录成功就不再轮询：unikey 一次性作废，再问只会得到误导的 800 */
    const bool logged = (s_state.qr_state == MUSIC_QR_OK);
    music_unlock();

    if (key[0] == '\0' || logged) {
        return;
    }

    const int err = music_netease_qr_poll(key, &state, &got_login);

    music_lock();
    s_state.qr_at_ms = music_now_ms();
    if (err != MUSIC_OK) {
        music_unlock();
        return;
    }
    s_state.qr_state = (uint16_t)state;

    if (state == MUSIC_QR_OK) {
        (void)music_netease_cookie_copy(s_state.cookie, sizeof(s_state.cookie));
        if (got_login) {
            music_netease_set_cookie(s_state.cookie);
            music_set_detail_info("登录成功");
            /*
             * 扫码成功就离开二维码页，回主菜单的「歌单」入口。
             */
            music_menu_open_locked(MUSIC_MENU_ROOT);
            s_state.menu_index = 0;
            music_unlock();
            /* KV 写 flash 不能在持锁时做（要几十毫秒，会拖住渲染抽帧） */
            (void)lisa_kv_set_string(MUSIC_KV_KEY_COOKIE, s_state.cookie);
            LOGI("qr login ok: cookie %u bytes music_u=%u", (unsigned)strlen(s_state.cookie),
                 (unsigned)music_netease_login_token_len());
            /* 登录态生效后立刻取账号，个人歌单出现在歌单二级菜单。 */
            music_fetch_account();
        } else {
            /* 803 但没下发 MUSIC_U：别假装登录成功 */
            s_state.cookie[0] = '\0';
            music_set_detail("登录成功但没拿到 cookie");
            music_unlock();
            LOGW("qr login 803 but no MUSIC_U in cookie jar");
        }
        return;
    }
    if (state == MUSIC_QR_EXPIRED) {
        music_set_detail("二维码已过期 双击刷新");
        music_unlock();
        return;
    }
    if (state == MUSIC_QR_WAIT_CONFIRM) {
        music_set_detail("已扫码 请在手机上确认");
    }
    music_unlock();
}

/*
 * 工作线程：取登录账号 + 我的歌单。
 * 调用时机：扫码 803 成功后、开机恢复 cookie 后（网络已就绪）。
 * cookie 失效时服务端回 code=200 但 account/profile 为 null，按“未登录”处理。
 */
static void music_fetch_account(void)
{
    int64_t uid = 0;
    char nickname[MUSIC_NAME_MAX];
    int err;

    nickname[0] = '\0';
    err = music_netease_account(&uid, nickname, sizeof(nickname));
    if (err != MUSIC_OK) {
        music_lock();
        s_state.uid = 0;
        s_state.my.count = 0;
        s_state.account_loaded = false;
        s_state.menu_index = 0;
        /* 有 cookie 才叫“失效”，从未登录就别给用户看红字 */
        music_set_detail((s_state.cookie[0] != '\0') ? "登录态已失效，可重新扫码" : NULL);
        music_unlock();
        LOGW("account fetch failed: %d(%s)", err, music_err_text(err));
        return;
    }

    music_lock();
    s_state.uid = uid;
    snprintf(s_state.nickname, sizeof(s_state.nickname), "%s", nickname);
    music_unlock();

    memset(&s_my_buf, 0, sizeof(s_my_buf));
    err = music_netease_user_playlists(uid, &s_my_buf);

    music_lock();
    memset(&s_state.my, 0, sizeof(s_state.my));
    if (err == MUSIC_OK) {
        memcpy(&s_state.my, &s_my_buf, sizeof(s_state.my));
    }
    s_state.account_loaded = true;
    if (s_state.menu_index >= music_menu_count_locked()) {
        s_state.menu_index = 0;
    }
    music_set_detail(NULL);
    music_unlock();

    LOGI("account %s uid=%lld playlists=%d (err=%d)", nickname, (long long)uid,
         (int)s_state.my.count, err);
    log_line("account %s uid=%lld playlists=%d", nickname, (long long)uid,
             (int)s_state.my.count);
}

/* ==================== 工作线程 ==================== */

static void music_execute(void)
{
    switch (s_req.kind) {
    case MUSIC_REQ_PLAYLIST:
        music_load_playlist(s_req.playlist_id, s_req.title);
        break;
    case MUSIC_REQ_ACCOUNT:
        music_fetch_account();
        break;
    case MUSIC_REQ_PLAY:
        s_req.err = music_play_index(s_req.arg, false);
        break;
    case MUSIC_REQ_NEXT:
        music_play_next(false);
        break;
    case MUSIC_REQ_RETRY:
        music_play_prev();
        break;
    case MUSIC_REQ_LYRIC: {
        music_song_t song;
        bool has = false;

        music_lock();
        const music_song_t *item = music_song_locked(s_state.current);
        if (item != NULL) {
            song = *item;
            has = true;
        }
        music_unlock();
        if (has) {
            const int err = music_netease_lyric(song.id, s_lrc_raw, sizeof(s_lrc_raw));
            music_lock();
            if (err == MUSIC_OK) {
                music_parse_lrc(s_lrc_raw);
            } else {
                music_lyric_reset();
            }
            log_line("lyric %lld -> %d(%s) lines=%u", (long long)song.id, err,
                     music_err_text(err), (unsigned)s_state.lyric_count);
            music_unlock();
        }
        break;
    }
    case MUSIC_REQ_QR_START:
        music_qr_start();
        break;
    case MUSIC_REQ_QR_POLL:
        music_qr_poll_once();
        music_lock();
        log_line("qr state=%u", (unsigned)s_state.qr_state);
        music_unlock();
        break;
    case MUSIC_REQ_SEARCH: {
        const int err = music_netease_search(s_req.keyword, 6, &s_song_buf);

        s_req.err = err;
        log_line("search '%s' -> err=%d(%s) n=%d", s_req.keyword, err, music_err_text(err),
                 s_song_buf.count);
        for (int i = 0; i < s_song_buf.count; i++) {
            log_line(" %d. %s / %s fee=%d", i + 1, s_song_buf.items[i].name,
                     s_song_buf.items[i].artist, s_song_buf.items[i].fee);
        }
        break;
    }
    case MUSIC_REQ_CHAIN: {
        music_track_url_t track;
        int err = music_netease_search(s_req.keyword, 6, &s_song_buf);

        log_line("1) search '%s' err=%d(%s) n=%d", s_req.keyword, err, music_err_text(err),
                 s_song_buf.count);
        if (err == MUSIC_OK) {
            for (int i = 0; i < s_song_buf.count; i++) {
                log_line("2) %s / %s fee=%d", s_song_buf.items[i].name,
                         s_song_buf.items[i].artist, s_song_buf.items[i].fee);
                err = music_netease_track_url(s_song_buf.items[i].id, &track);
                if (err == MUSIC_OK) {
                    log_line("3) %s", track.url);
                    log_line("4) play=%d", music_start_url(track.url));
                    err = MUSIC_OK;
                    break;
                }
                log_line("   直链失败: %d(%s)", err, music_err_text(err));
            }
        }
        s_req.err = err;
        break;
    }
    default:
        break;
    }
}

/* 播放器事件（player 线程）：只置标志，绝不在这里碰网络/UI */
static void music_player_event_cb(app_player_t *player, app_player_event_t event, void *user_data)
{
    (void)player;
    (void)user_data;

    switch (event) {
    case APP_PLAYER_EVENT_PLAYING:
    case APP_PLAYER_EVENT_PREPARED:
        s_state.playing = true;
        music_lock();
        music_set_detail(NULL);
        music_unlock();
        break;
    case APP_PLAYER_EVENT_PAUSED:
    case APP_PLAYER_EVENT_STOPPED:
        s_state.playing = false;
        break;
    case APP_PLAYER_EVENT_COMPLETED:
        s_state.playing = false;
        s_req.next_pending = true;
        if (s_signal != NULL) {
            (void)xSemaphoreGive(s_signal);
        }
        break;
    case APP_PLAYER_EVENT_ERROR:
        s_state.playing = false;
        music_lock();
        music_set_detail("播放出错");
        music_unlock();
        break;
    default:
        break;
    }
}

/* 每 200 ms：网络状态 / 进度 / 歌词游标 / 扫码轮询 / 开机进页 */
static void music_tick(void)
{
    sys_network_status_t status;
    bool net_ok = false;
    bool net_up_event = false;

    if (sys_network_get_status(&status) == 0) {
        net_ok = status.connected;
    }
    net_up_event = (net_ok && !s_state.net_ok);
    s_state.net_ok = net_ok;

    /* 开机进页只做一次 */
    if (!s_state.started) {
        s_state.started = true;
#ifdef CONFIG_NETEASE_MUSIC_ONLY
        /* 单应用固件：音乐页是唯一页面。没配过网就直接停在配网页并开始广播，
         * 配网成功后（下面 net_up_event）自动回播放页。 */
        if (!sys_wifi_has_ap()) {
            LOGI("no saved wifi, boot into netcfg page");
            music_netcfg_enter();
        }
#else
        /* 全功能固件：只有配过网才抢默认页，否则让 radio 的配网二维码留在前台 */
        if (sys_wifi_has_ap()) {
            LISA_UI_INVOKE_UI_ARG_NONE({ music_ui_show(); });
            LOGI("music page is the front page now (net=%d)", (int)net_ok);
        }
#endif
    }

#ifdef CONFIG_NETEASE_MUSIC_ONLY
    /* 配网成功：离开配网页回播放页 */
    if (net_up_event && (music_page_t)s_state.page == MUSIC_PAGE_NETCFG) {
        music_lock();
        s_state.page = MUSIC_PAGE_PLAYER;
        music_set_detail(NULL);
        music_unlock();
        LOGI("provisioned, back to player page");
    }
#endif

    /* 开机恢复了 cookie：等网络就绪后补拉一次账号 + 我的歌单 */
    if (s_state.account_pending && net_ok) {
        s_state.account_pending = false;
        music_fetch_account();
    }

    const app_player_state_t player_state = music_player != NULL
        ? app_player_get_state(music_player) : APP_PLAYER_STATE_IDLE;
    s_state.playing = (player_state == APP_PLAYER_STATE_PLAYING);

    /* 只在播放或暂停时查询进度，避免无 demux 时反复打印错误。 */
    if (player_state == APP_PLAYER_STATE_PLAYING || player_state == APP_PLAYER_STATE_PAUSED) {
        uint32_t pos = 0;
        uint32_t dur = 0;

        if (app_player_get_position(music_player, &pos) == 0) {
            s_state.pos_ms = pos;
        }
        if (app_player_get_duration(music_player, &dur) == 0 && dur > 0) {
            s_state.dur_ms = dur;
        }
    }

    music_lock();
    if (s_state.lyric_count > 0) {
        music_lyric_seek(s_state.pos_ms);
    }

    /* 一次性提示（登录成功等）3 秒后自动消失 */
    if (s_state.detail_transient && s_state.detail[0] != '\0' &&
        (uint32_t)(music_now_ms() - s_state.detail_ms) > 3000u) {
        music_set_detail(NULL);
    }

    /* 扫码轮询：只在登录页、且状态是「待扫码/待确认」时跑 */
    if (s_state.page == MUSIC_PAGE_LOGIN &&
        (s_state.qr_state == MUSIC_QR_WAIT_SCAN || s_state.qr_state == MUSIC_QR_WAIT_CONFIRM)) {
        const uint32_t now = music_now_ms();
        const bool due = (uint32_t)(now - s_state.qr_at_ms) >= MUSIC_QR_POLL_MS;
        const bool timeout = (uint32_t)(now - s_state.qr_start_ms) >= MUSIC_QR_TIMEOUT_MS;

        if (timeout) {
            s_state.qr_state = MUSIC_QR_EXPIRED;
            music_set_detail("二维码已过期 双击刷新");
        } else if (due) {
            s_state.qr_at_ms = now; /* 先在锁内置位，避免重入 */
            music_unlock();
            music_qr_poll_once();
            music_lock();
        }
    }
    music_unlock();
}

static void music_task(void *argument)
{
    (void)argument;

    for (;;) {
        if (xSemaphoreTake(s_signal, pdMS_TO_TICKS(MUSIC_TICK_MS)) == pdTRUE) {
            while (s_req.kind != MUSIC_REQ_NONE) {
                s_req.busy = true;
                music_execute();
                s_req.kind = MUSIC_REQ_NONE;
                s_req.busy = false;
                (void)xSemaphoreGive(s_done);
            }
        }
        if (s_req.next_pending) {
            s_req.next_pending = false;
            music_play_next(true);
        }
        music_tick();
    }
}

/* ==================== 渲染侧取数 ==================== */

bool music_app_get_model(music_ui_model_t *out)
{
    if (out == NULL || !s_state.inited || s_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    music_menu_normalize_locked();
    memset(out, 0, sizeof(*out));
    out->page = s_state.page;
    out->menu_level = s_state.menu_level;
    out->menu_index = s_state.menu_index;
    out->list_index = s_state.list_index;
    out->net_ok = s_state.net_ok ? 1u : 0u;
    out->busy = s_state.busy ? 1u : 0u;
    out->playing = s_state.playing ? 1u : 0u;
    out->paused = (s_state.user_paused && !s_state.playing) ? 1u : 0u;
    out->song_count = s_state.count;
    out->song_index = s_state.current + ((s_state.count > 0) ? 1u : 0u);
    out->qr_state = s_state.qr_state;
    out->logged_in = (s_state.cookie[0] != '\0') ? 1u : 0u;
    out->lyric_valid = (s_state.lyric_count > 0) ? 1u : 0u;
    out->pos_ms = s_state.pos_ms;
    out->dur_ms = s_state.dur_ms;
    out->art_gen = s_state.art_gen;
    out->art_len = (s_state.art.body != NULL) ? (uint32_t)s_state.art.length : 0u;
    out->art_w = s_state.art_w;
    out->art_h = s_state.art_h;
    out->qr_gen = s_state.qr_gen;
    snprintf(out->detail, sizeof(out->detail), "%s", s_state.detail);
    snprintf(out->list_title, sizeof(out->list_title), "%s", s_state.list_title);
    snprintf(out->qr_url, sizeof(out->qr_url), "%s", s_state.qr_url);
    snprintf(out->logon, sizeof(out->logon), "%s",
             (s_state.cookie[0] == '\0')
                 ? "未登录"
                 : (s_state.nickname[0] != '\0' ? s_state.nickname : "已登录"));

    const music_song_t *song = music_song_locked(s_state.current);
    if (song != NULL) {
        snprintf(out->title, sizeof(out->title), "%s", song->name);
        snprintf(out->artist, sizeof(out->artist), "%s", song->artist);
        snprintf(out->album, sizeof(out->album), "%s", song->album);
    } else {
        snprintf(out->title, sizeof(out->title), "网易云音乐");
        snprintf(out->artist, sizeof(out->artist), "%s",
                 s_state.net_ok ? "三击菜单选歌单" : "未连接网络");
        snprintf(out->album, sizeof(out->album), "热歌榜 · 新歌榜 · 飙升榜");
    }

    snprintf(out->menu_title, sizeof(out->menu_title), "%s",
             s_state.menu_level == MUSIC_MENU_PLAYLISTS ? "歌单" : "网易云音乐");
    snprintf(out->menu_hint, sizeof(out->menu_hint), "%s",
             s_state.menu_level == MUSIC_MENU_SETTINGS
                 ? "单击选择 双击调节 三击返回" : "单击选择 双击确认 三击返回");

    /* 所有层级复用一屏五行，歌单二级菜单超过五项时滚动。 */
    const uint8_t menu_count = music_menu_count_locked();
    uint8_t menu_window = (uint8_t)(s_state.menu_index / MUSIC_UI_ROWS * MUSIC_UI_ROWS);

    if (menu_count > MUSIC_UI_ROWS && menu_window + MUSIC_UI_ROWS > menu_count) {
        menu_window = (uint8_t)(menu_count - MUSIC_UI_ROWS);
    }
    out->menu_count = menu_count;
    out->menu_window = menu_window;
    for (uint8_t i = 0; i < MUSIC_UI_ROWS; i++) {
        const uint8_t index = (uint8_t)(menu_window + i);
        music_item_t item;

        if (index >= menu_count || !music_menu_item_locked(index, &item)) {
            continue;
        }
        switch (item.kind) {
        case MUSIC_ITEM_PLAYLISTS:
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "歌单");
            snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%u 个",
                     (unsigned)(s_state.my.count + MUSIC_PLAYLIST_COUNT));
            break;
        case MUSIC_ITEM_SETTINGS:
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "设置");
            snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "音量");
            break;
        case MUSIC_ITEM_VOLUME_UP:
        case MUSIC_ITEM_VOLUME_DOWN:
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "%s",
                     item.kind == MUSIC_ITEM_VOLUME_UP ? "音量增加" : "音量降低");
            snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%c%u",
                     item.kind == MUSIC_ITEM_VOLUME_UP ? '+' : '-', (unsigned)MUSIC_VOLUME_STEP);
            break;
        case MUSIC_ITEM_MY: {
            const music_user_playlist_t *my = &s_state.my.items[item.index];

            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "%s", my->name);
            if (s_state.playlist_id == my->id && s_state.count > 0) {
                snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%u 首",
                         (unsigned)s_state.count);
            } else if (my->track_count > 0) {
                snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%d 首",
                         my->track_count);
            }
            break;
        }
        case MUSIC_ITEM_CHART: {
            const uint8_t chart = item.index;

            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "%s", s_playlists[chart].name);
            if (s_state.playlist_id == s_playlists[chart].id && s_state.count > 0) {
                snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%u 首",
                         (unsigned)s_state.count);
            }
            break;
        }
        case MUSIC_ITEM_LYRIC:
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "歌词");
            snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%s",
                     (s_state.lyric_count > 0) ? "有" : "无");
            break;
        case MUSIC_ITEM_LOGIN:
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "扫码登录");
            snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%s",
                     (s_state.cookie[0] != '\0')
                         ? (s_state.nickname[0] != '\0' ? s_state.nickname : "已登录")
                         : "未登录");
            break;
        default:
            break;
        }
    }

    /* 歌曲列表窗口 */
    if (s_state.count > 0) {
        uint8_t window = (uint8_t)(s_state.list_index / MUSIC_UI_ROWS * MUSIC_UI_ROWS);
        if (window + MUSIC_UI_ROWS > s_state.count && s_state.count > MUSIC_UI_ROWS) {
            window = (uint8_t)(s_state.count - MUSIC_UI_ROWS);
        }
        out->list_window = window;
        for (uint8_t i = 0; i < MUSIC_UI_ROWS; i++) {
            const uint8_t index = (uint8_t)(window + i);
            if (index >= s_state.count) {
                break;
            }
            snprintf(out->row_title[i], sizeof(out->row_title[i]), "%02u  %s",
                     (unsigned)index + 1u, s_songs[index].name);
            snprintf(out->row_sub[i], sizeof(out->row_sub[i]), "%s · %s", s_songs[index].artist,
                     s_songs[index].album);
            out->row_count = (uint8_t)(i + 1u);
        }
    }

    /* 歌词窗口：当前行放在中间 */
    out->lyric_center = -1;
    if (s_state.lyric_count > 0 && s_state.lyric_index >= 0) {
        const int center_row = MUSIC_UI_ROWS / 2;
        const int first = (int)s_state.lyric_index - center_row;

        out->lyric_center = (int8_t)center_row;
        for (int i = 0; i < MUSIC_UI_ROWS; i++) {
            const int index = first + i;
            if (index < 0 || index >= (int)s_state.lyric_count) {
                continue;
            }
            snprintf(out->lyric[i], sizeof(out->lyric[i]), "%s", s_lyric_text[index]);
        }
    }

    switch ((int)s_state.qr_state) {
    case MUSIC_QR_WAIT_SCAN:
        snprintf(out->qr_hint, sizeof(out->qr_hint), "网易云 App 扫码登录");
        break;
    case MUSIC_QR_WAIT_CONFIRM:
        snprintf(out->qr_hint, sizeof(out->qr_hint), "已扫码 请在手机上确认");
        break;
    case MUSIC_QR_OK:
        snprintf(out->qr_hint, sizeof(out->qr_hint), "登录成功");
        break;
    case MUSIC_QR_EXPIRED:
        snprintf(out->qr_hint, sizeof(out->qr_hint), "已过期 双击刷新");
        break;
    default:
        snprintf(out->qr_hint, sizeof(out->qr_hint), "双击生成二维码");
        break;
    }

    xSemaphoreGive(s_lock);

    const int volume = service_volume_get();
    out->volume = (volume < 0) ? 0u : (uint8_t)volume;
    if (out->menu_level == MUSIC_MENU_SETTINGS) {
        snprintf(out->menu_title, sizeof(out->menu_title), "设置 · 音量%u",
                 (unsigned)out->volume);
    }
    return true;
}

bool music_app_cover_copy(uint8_t *dst, size_t capacity, size_t *length)
{
    bool ok = false;

    if (dst == NULL || length == NULL || s_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (s_state.art.body != NULL && s_state.art.length > 0 &&
        (size_t)s_state.art.length <= capacity) {
        memcpy(dst, s_state.art.body, (size_t)s_state.art.length);
        *length = (size_t)s_state.art.length;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool music_app_is_on_top(void)
{
    return music_ui_top_id() == music_ui_screen_id();
}

void music_app_show(void)
{
    LISA_UI_INVOKE_UI_ARG_NONE({ music_ui_show(); });
}

void music_app_hide(void)
{
    LISA_UI_INVOKE_UI_ARG_NONE({ music_ui_hide(); });
}

/* ==================== 按键 ==================== */

static void music_toggle_play(void)
{
    bool resume = false;
    char url[MUSIC_URL_MAX] = {0};

    music_lock();
    if (s_state.playing && !s_state.user_paused) {
        s_state.user_paused = true;
    } else {
        s_state.user_paused = false;
        resume = true;
        snprintf(url, sizeof(url), "%s", s_state.url);
    }
    music_unlock();

    if (!resume) {
        if (music_player != NULL) {
            (void)app_player_pause(music_player);
        }
        return;
    }
    if (url[0] != '\0' && music_player != NULL) {
        (void)app_player_resume(music_player);
        return;
    }
    /* 还没有起播过：直接播当前选中 */
    s_req.kind = MUSIC_REQ_PLAY;
    s_req.arg = s_state.current;
    (void)xSemaphoreGive(s_signal);
}

static void music_menu_enter(void)
{
    music_item_t item;
    int64_t playlist_id = 0;
    char title[MUSIC_TITLE_MAX] = {0};
    bool ok;

    music_lock();
    music_menu_normalize_locked();
    ok = music_menu_item_locked(s_state.menu_index, &item);
    if (ok) {
        switch (item.kind) {
        case MUSIC_ITEM_PLAYLISTS:
            music_menu_open_locked(MUSIC_MENU_PLAYLISTS);
            break;
        case MUSIC_ITEM_SETTINGS:
            music_menu_open_locked(MUSIC_MENU_SETTINGS);
            break;
        case MUSIC_ITEM_MY:
            playlist_id = s_state.my.items[item.index].id;
            snprintf(title, sizeof(title), "%s", s_state.my.items[item.index].name);
            break;
        case MUSIC_ITEM_CHART:
            playlist_id = s_playlists[item.index].id;
            snprintf(title, sizeof(title), "%s", s_playlists[item.index].name);
            break;
        default:
            break;
        }
    }
    music_unlock();

    if (!ok) {
        return;
    }
    if (item.kind == MUSIC_ITEM_PLAYLISTS || item.kind == MUSIC_ITEM_SETTINGS) {
        return;
    }
    if (item.kind == MUSIC_ITEM_VOLUME_UP || item.kind == MUSIC_ITEM_VOLUME_DOWN) {
        /* 持久化写入放在锁外，避免阻塞渲染侧快照。 */
        service_volume_adjust(item.kind == MUSIC_ITEM_VOLUME_UP
                                  ? MUSIC_VOLUME_STEP : -MUSIC_VOLUME_STEP);
        return;
    }
    if (item.kind == MUSIC_ITEM_MY || item.kind == MUSIC_ITEM_CHART) {
        s_state.page = MUSIC_PAGE_SONGLIST;
        s_req.kind = MUSIC_REQ_PLAYLIST;
        s_req.playlist_id = playlist_id;
        snprintf(s_req.title, sizeof(s_req.title), "%s", title);
        (void)xSemaphoreGive(s_signal);
        return;
    }
    if (item.kind == MUSIC_ITEM_LYRIC) {
        s_state.page = MUSIC_PAGE_LYRIC;
        return;
    }
    s_state.page = MUSIC_PAGE_LOGIN;
    if (s_state.qr_state != MUSIC_QR_WAIT_SCAN && s_state.qr_state != MUSIC_QR_WAIT_CONFIRM) {
        s_req.kind = MUSIC_REQ_QR_START;
        (void)xSemaphoreGive(s_signal);
    }
}

static void music_back(void)
{
    music_lock();
    switch ((music_page_t)s_state.page) {
    case MUSIC_PAGE_SONGLIST:
        music_menu_open_locked(MUSIC_MENU_PLAYLISTS);
        break;
    case MUSIC_PAGE_MENU:
        if (s_state.menu_level != MUSIC_MENU_ROOT) {
            music_menu_open_locked(MUSIC_MENU_ROOT);
        } else {
            s_state.page = MUSIC_PAGE_PLAYER;
        }
        break;
    case MUSIC_PAGE_LYRIC:
    case MUSIC_PAGE_LOGIN:
        music_menu_open_locked(MUSIC_MENU_ROOT);
        break;
    case MUSIC_PAGE_NETCFG:
    case MUSIC_PAGE_PLAYER:
    default:
        s_state.page = MUSIC_PAGE_PLAYER;
        break;
    }
    music_unlock();
}

/*
 * 蓝牙配网页：屏幕换成配网二维码，并开始广播。
 * 单应用固件（没有 radio 模块）由音乐页自己提供这个入口（五/六/七击）。
 */
static void music_netcfg_enter(void)
{
    music_lock();
    s_state.page = MUSIC_PAGE_NETCFG;
    music_set_detail(NULL);
    music_unlock();
    app_ble_netcfg_adv_start_delayed();
    LOGI("ble netcfg requested by button");
}

bool music_app_handle_action(int action)
{
    if (!s_state.inited || !music_app_is_on_top()) {
        return false;
    }

#ifdef CONFIG_NETEASE_MUSIC_ONLY
    /* 开机按键保护：长按开机时松手那一下会被当成 CLICK/LONG_HOLD */
    if ((uint32_t)(music_now_ms() - s_state.boot_ms) < MUSIC_BOOT_KEY_GUARD_MS) {
        LOGI("ignore button %d during boot guard", action);
        return true;
    }

    /* 未配网时锁在配网页：只允许关机（长按）和重新发起配网（5/6/7） */
    if ((music_page_t)s_state.page == MUSIC_PAGE_NETCFG && !sys_wifi_has_ap()) {
        if (action != VOICE_MSG_BUTTON_ACTION_LONG_HOLD &&
            action != VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK &&
            action != VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK &&
            action != VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK) {
            LOGI("netcfg page locked until provisioned (action=%d)", action);
            return true;
        }
    }

    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        /* 长按 = 关机：单应用固件里没有 radio 的系统语义，自己来 */
        LOGI("power button long hold, shutting down");
        power_shutdown();
        return true;
    case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
        music_netcfg_enter();
        return true;
    default:
        break;
    }
#endif

    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
        switch ((music_page_t)s_state.page) {
        case MUSIC_PAGE_MENU: {
            /* 歌单层条目数会随登录状态变化，读写选中项都在锁内。 */
            music_lock();
            music_menu_normalize_locked();
            const uint8_t menu_count = music_menu_count_locked();

            if (menu_count > 0) {
                s_state.menu_index = (uint8_t)((s_state.menu_index + 1u) % menu_count);
            }
            music_unlock();
            break;
        }
        case MUSIC_PAGE_SONGLIST:
            if (s_state.count > 0) {
                s_state.list_index = (uint8_t)((s_state.list_index + 1u) % s_state.count);
            }
            break;
        case MUSIC_PAGE_LOGIN:
            s_req.kind = MUSIC_REQ_QR_START;
            (void)xSemaphoreGive(s_signal);
            break;
        case MUSIC_PAGE_LYRIC:
        case MUSIC_PAGE_PLAYER:
        default:
            s_req.kind = MUSIC_REQ_NEXT;
            (void)xSemaphoreGive(s_signal);
            break;
        }
        return true;

    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
        switch ((music_page_t)s_state.page) {
        case MUSIC_PAGE_MENU:
            music_menu_enter();
            break;
        case MUSIC_PAGE_SONGLIST:
            s_state.current = s_state.list_index;
            s_state.page = MUSIC_PAGE_PLAYER;
            s_req.kind = MUSIC_REQ_PLAY;
            s_req.arg = s_state.list_index;
            (void)xSemaphoreGive(s_signal);
            break;
        case MUSIC_PAGE_LOGIN:
            s_req.kind = MUSIC_REQ_QR_POLL;
            (void)xSemaphoreGive(s_signal);
            break;
        case MUSIC_PAGE_LYRIC:
        case MUSIC_PAGE_PLAYER:
        default:
            music_toggle_play();
            break;
        }
        return true;

    case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
        if ((music_page_t)s_state.page == MUSIC_PAGE_PLAYER) {
            music_lock();
            music_menu_open_locked(MUSIC_MENU_ROOT);
            s_state.menu_index = 0;
            music_unlock();
        } else {
            music_back();
        }
        return true;

    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        /* 长按 = 关机（全功能固件里由 radio 的系统语义执行） */
        return false;

    case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
        /* 配网：单应用固件已在上面的分支处理，全功能固件交给 radio */
        return false;

    default:
        /* 连击 8+ 吞掉，避免误触恢复出厂 */
        return true;
    }
}

/* ==================== shell 命令 ==================== */

static int music_submit(music_req_kind_t kind, uint8_t arg, const char *keyword)
{
    if (!s_state.inited) {
        return -1;
    }
    /*
     * 工作线程正忙时不直接报 busy：串口命令很容易撞上后台的拉歌单/取账号，
     * 最多等 3 秒（工作线程一个请求通常几百毫秒），让用户看到的是结果而不是
     * 一句让人以为出错的 “music: busy”。
     */
    for (int waited = 0; waited < 30 && (s_req.busy || s_req.kind != MUSIC_REQ_NONE); waited++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_req.busy || s_req.kind != MUSIC_REQ_NONE) {
        return -2;
    }

    log_reset();
    s_req.err = MUSIC_OK;
    s_req.keyword[0] = '\0';
    if (keyword != NULL) {
        snprintf(s_req.keyword, sizeof(s_req.keyword), "%s", keyword);
    }
    s_req.arg = arg;
    s_req.busy = true;
    s_req.kind = kind;

    (void)xSemaphoreTake(s_done, 0); /* 排空上一轮的残留信号 */
    (void)xSemaphoreGive(s_signal);

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(MUSIC_CMD_WAIT_MS)) != pdTRUE) {
        s_req.busy = false;
        return -3;
    }
    return 0;
}

static int music_submit_list(int64_t playlist_id, const char *title)
{
    s_req.playlist_id = playlist_id;
    snprintf(s_req.title, sizeof(s_req.title), "%s", (title != NULL) ? title : "");
    return music_submit(MUSIC_REQ_PLAYLIST, 0, NULL);
}

static void music_print_lines(Shell *shell)
{
    for (int i = 0; i < s_req.line_count; i++) {
        shellPrint(shell, "%s\n", s_req.lines[i]);
    }

    /* JSON 片段按 100 字节分段打印：SHELL_PRINT_BUFFER 只有 128 */
    if (music_netease_last_json_len > 0) {
        const int total = music_netease_last_json_len;
        char chunk[101];

        shellPrint(shell, " json[%d,http=%d]:\n", total, music_netease_last_status);
        for (int offset = 0; offset < total; offset += 100) {
            int n = total - offset;

            if (n > 100) {
                n = 100;
            }
            memcpy(chunk, music_netease_last_json + offset, (size_t)n);
            chunk[n] = '\0';
            shellPrint(shell, "  %s\n", chunk);
        }
    }
}

static int music_cmd_ui(const char *name)
{
    static const struct {
        const char *name;
        uint8_t page;
    } map[] = {
        {"player", MUSIC_PAGE_PLAYER},
        {"menu", MUSIC_PAGE_MENU},
        {"playlist", MUSIC_PAGE_MENU},
        {"settings", MUSIC_PAGE_MENU},
        {"songlist", MUSIC_PAGE_SONGLIST},
        {"lyric", MUSIC_PAGE_LYRIC},
        {"login", MUSIC_PAGE_LOGIN},
        {"netcfg", MUSIC_PAGE_NETCFG},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcmp(name, map[i].name) == 0) {
            music_lock();
            if (map[i].page == MUSIC_PAGE_MENU) {
                music_menu_open_locked(strcmp(name, "playlist") == 0 ? MUSIC_MENU_PLAYLISTS
                                       : strcmp(name, "settings") == 0 ? MUSIC_MENU_SETTINGS
                                                                       : MUSIC_MENU_ROOT);
                s_state.menu_index = 0;
            } else {
                s_state.page = map[i].page;
            }
            music_unlock();
            LISA_UI_INVOKE_UI_ARG_NONE({ music_ui_show(); });
            if (map[i].page == MUSIC_PAGE_LOGIN &&
                s_state.qr_state != MUSIC_QR_WAIT_SCAN) {
                s_req.kind = MUSIC_REQ_QR_START;
                (void)xSemaphoreGive(s_signal);
            }
            return 0;
        }
    }
    LOGE("unknown page: %s", name);
    return -1;
}

static int music_cmd(int argc, char *argv[])
{
    Shell *shell = shellGetCurrent();
    const char *sub = (argc >= 2) ? argv[1] : NULL;
    int ret = 0;

    if (shell == NULL) {
        return -1;
    }
    if (!s_state.inited) {
        shellPrint(shell, "music: not initialized\n");
        return -1;
    }

    if (sub == NULL || strcmp(sub, "stat") == 0) {
        shellPrint(shell, "music: page=%u busy=%u busyreq=%u player=%d url=%s\n",
                   (unsigned)s_state.page, (unsigned)s_state.busy, (unsigned)s_req.busy,
                   music_player != NULL ? (int)app_player_get_state(music_player) : -1,
                   s_state.url);
        shellPrint(shell, "  list=%s songs=%u current=%u pos=%u/%u lyric=%u/%d\n",
                   s_state.list_title, (unsigned)s_state.count,
                   (unsigned)s_state.current + 1u, (unsigned)s_state.pos_ms,
                   (unsigned)s_state.dur_ms, (unsigned)s_state.lyric_count,
                   (int)s_state.lyric_index);
        shellPrint(shell, "  qr=%u logon=%s jar=%u art=%d detail=%s\n",
                   (unsigned)s_state.qr_state,
                   s_state.cookie[0] != '\0' ? "yes" : "no",
                   (unsigned)strlen(s_state.cookie), s_state.art.length,
                   s_state.detail[0] != '\0' ? s_state.detail : "-");
        shellPrint(shell, "  uid=%lld nick=%s my=%d menu=%u/%u level=%u volume=%d\n", (long long)s_state.uid,
                   s_state.nickname[0] != '\0' ? s_state.nickname : "-",
                   (int)s_state.my.count, (unsigned)s_state.menu_index,
                   (unsigned)music_menu_count_locked(), (unsigned)s_state.menu_level,
                   service_volume_get());
        shellPrint(shell, "  psram free=%u sram free=%u\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return 0;
    }

    if (strcmp(sub, "stop") == 0) {
        if (music_player != NULL) {
            (void)app_player_stop(music_player);
        }
        s_state.playing = false;
        s_state.user_paused = true;
        shellPrint(shell, "music: stopped\n");
        return 0;
    }

    if (strcmp(sub, "ui") == 0) {
        if (argc < 3) {
            shellPrint(shell, "usage: music ui player|menu|playlist|settings|songlist|lyric|login\n");
            return -1;
        }
        return music_cmd_ui(argv[2]);
    }

    if (strcmp(sub, "api") == 0) {
        const int which = (argc >= 3) ? atoi(argv[2]) : 0;

        switch (which) {
        case 1:
            ret = music_submit(MUSIC_REQ_SEARCH, 0, "周杰伦");
            break;
        case 2:
            ret = music_submit(MUSIC_REQ_PLAYLIST, 0, NULL);
            break;
        case 3:
            ret = music_submit(MUSIC_REQ_LYRIC, 0, NULL);
            break;
        case 4:
            ret = music_submit(MUSIC_REQ_PLAY, 0, NULL);
            break;
        case 5:
            ret = music_submit(MUSIC_REQ_QR_START, 0, NULL);
            break;
        case 6:
            ret = music_submit(MUSIC_REQ_QR_POLL, 0, NULL);
            break;
        case 7:
            ret = music_submit(MUSIC_REQ_ACCOUNT, 0, NULL);
            break;
        default:
            shellPrint(shell, "usage: music api <1..7>\n");
            return -1;
        }
    } else if (strcmp(sub, "search") == 0) {
        ret = music_submit(MUSIC_REQ_SEARCH, 0, (argc >= 3) ? argv[2] : "周杰伦");
    } else if (strcmp(sub, "playlist") == 0) {
        const int index = (argc >= 3) ? atoi(argv[2]) : 0;

        if (index < 0 || index >= (int)MUSIC_PLAYLIST_COUNT) {
            shellPrint(shell, "usage: music playlist <0..%d>\n", (int)MUSIC_PLAYLIST_COUNT - 1);
            return -1;
        }
        ret = music_submit_list(s_playlists[index].id, s_playlists[index].name);
    } else if (strcmp(sub, "my") == 0) {
        /* 登录后自己的歌单（order 与导航页顶部一致） */
        const int index = (argc >= 3) ? atoi(argv[2]) : 0;

        if (index < 0 || index >= (int)s_state.my.count) {
            shellPrint(shell, "music: my playlists=%d (login first?)\n", (int)s_state.my.count);
            return -1;
        }
        ret = music_submit_list(s_state.my.items[index].id, s_state.my.items[index].name);
    } else if (strcmp(sub, "account") == 0) {
        ret = music_submit(MUSIC_REQ_ACCOUNT, 0, NULL);
#ifdef CONFIG_NETEASE_MUSIC_ONLY
    } else if (strcmp(sub, "key") == 0) {
        /* 单应用固件里按键全归音乐页，这里直接注入一个动作（真机验收用） */
        static const struct {
            const char *name;
            int action;
        } keys[] = {
            {"click", VOICE_MSG_BUTTON_ACTION_CLICK},
            {"double", VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK},
            {"triple", VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK},
            {"quad", VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK},
            {"five", VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK},
            {"six", VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK},
            {"seven", VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK},
            {"long", VOICE_MSG_BUTTON_ACTION_LONG_HOLD},
        };
        const char *name = (argc >= 3) ? argv[2] : "click";

        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (strcmp(name, keys[i].name) == 0) {
                shellPrint(shell, "music: key %s -> %d, page=%u\n", name, keys[i].action,
                           (unsigned)s_state.page);
                (void)music_app_handle_action(keys[i].action);
                shellPrint(shell, "  page=%u now\n", (unsigned)s_state.page);
                return 0;
            }
        }
        shellPrint(shell, "usage: music key click|double|triple|quad|five|six|seven|long\n");
        return -1;
#endif
    } else if (strcmp(sub, "play") == 0) {
        const int index = (argc >= 3) ? atoi(argv[2]) : 0;

        if (index < 0 || index >= (int)MUSIC_SONGS_MAX) {
            shellPrint(shell, "usage: music play <0..%d>\n", (int)MUSIC_SONGS_MAX - 1);
            return -1;
        }
        ret = music_submit(MUSIC_REQ_PLAY, (uint8_t)index, NULL);
    } else if (strcmp(sub, "next") == 0) {
        ret = music_submit(MUSIC_REQ_NEXT, 0, NULL);
    } else if (strcmp(sub, "prev") == 0) {
        ret = music_submit(MUSIC_REQ_RETRY, 0, NULL);
    } else if (strcmp(sub, "lyrics") == 0) {
        ret = music_submit(MUSIC_REQ_LYRIC, 0, NULL);
    } else if (strcmp(sub, "login") == 0) {
        const char *what = (argc >= 3) ? argv[2] : "start";

        if (strcmp(what, "poll") == 0) {
            ret = music_submit(MUSIC_REQ_QR_POLL, 0, NULL);
        } else if (strcmp(what, "start") == 0) {
            ret = music_submit(MUSIC_REQ_QR_START, 0, NULL);
        } else if (strcmp(what, "cookie") == 0) {
            shellPrint(shell, "jar(%u) music_u=%u: %.160s\n", (unsigned)strlen(s_state.cookie),
                       (unsigned)music_netease_login_token_len(), s_state.cookie);
            return 0;
        } else if (strcmp(what, "logout") == 0) {
            s_state.cookie[0] = '\0';
            s_state.uid = 0;
            s_state.nickname[0] = '\0';
            s_state.my.count = 0;
            s_state.account_loaded = false;
            s_state.menu_index = 0;
            music_netease_set_cookie(NULL);
            (void)lisa_kv_set_string(MUSIC_KV_KEY_COOKIE, "");
            shellPrint(shell, "music: logged out\n");
            return 0;
        } else {
            shellPrint(shell, "usage: music login start|poll|cookie|logout\n");
            return -1;
        }
    } else if (strcmp(sub, "chain") == 0) {
        ret = music_submit(MUSIC_REQ_CHAIN, 0, (argc >= 3) ? argv[2] : "周杰伦");
    } else if (strcmp(sub, "song") == 0) {
        /* 打印当前列表（真机验收用） */
        for (uint8_t i = 0; i < s_state.count; i++) {
            shellPrint(shell, "[%u] %s | %s | %s\n", (unsigned)i + 1u, s_songs[i].name,
                       s_songs[i].artist, s_songs[i].album);
        }
        return 0;
    } else {
        shellPrint(shell, "usage: music [stat|stop|ui <page>|api <n>|search|playlist|my|account|"
                          "key|play|next|prev|lyrics|login start|poll|chain|song]\n");
        return -1;
    }

    if (ret == -2) {
        shellPrint(shell, "music: busy\n");
        return -1;
    }
    if (ret == -3) {
        shellPrint(shell, "music: timeout\n");
        return -1;
    }
    if (ret != 0) {
        shellPrint(shell, "music: submit failed %d\n", ret);
        return -1;
    }
    music_print_lines(shell);
    return (s_req.err == MUSIC_OK) ? 0 : -1;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 music, music_cmd,
                 music [stat|stop|ui|api|search|playlist|my|account|key|play|next|prev|lyrics|login|chain|song]);

/* ==================== 初始化 ==================== */

static void music_restore_kv(void)
{
    char *value = NULL;

    if (lisa_kv_get_string(MUSIC_KV_KEY_COOKIE, &value) == 0 && value != NULL) {
        snprintf(s_state.cookie, sizeof(s_state.cookie), "%s", value);
        lisa_kv_free(value);
        music_netease_set_cookie(s_state.cookie);
        if (!music_netease_has_login()) {
            /* 只有 __csrf 没有 MUSIC_U 的残渣不算登录态 */
            s_state.cookie[0] = '\0';
            music_netease_set_cookie(NULL);
            LOGW("restored cookie has no MUSIC_U, treated as logged out");
            return;
        }
        /* 登录态是持久的：等网络就绪后补拉账号 + 我的歌单 */
        s_state.account_pending = true;
        LOGI("restored login cookie (%u bytes)", (unsigned)strlen(s_state.cookie));
    }
}

#ifdef CONFIG_NETEASE_MUSIC_ONLY
/* 按键消息回调（订阅 VOICE_MSG_BUTTON_CHANGE；全功能固件里这条路由 radio 转发） */
static void music_on_button(void *unused, uint32_t msg_id, void *data, uint32_t len,
                           void *user_data)
{
    (void)unused;
    (void)msg_id;
    (void)user_data;

    const voice_msg_button_evt_t *evt = (const voice_msg_button_evt_t *)data;
    if (evt == NULL || len < sizeof(*evt) || evt->button_id != 0) {
        return;
    }
    (void)music_app_handle_action((int)evt->action);
}
#endif

int music_app_init(void)
{
    if (s_state.inited) {
        return 0;
    }

    memset(&s_state, 0, sizeof(s_state));
    memset(s_songs, 0, sizeof(s_songs));
    memset(&s_req, 0, sizeof(s_req));
    s_state.page = MUSIC_PAGE_PLAYER;
    s_state.lyric_index = -1;

    s_lock = xSemaphoreCreateMutex();
    s_signal = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_signal == NULL || s_done == NULL) {
        LOGE("failed to create sync objects");
        return -1;
    }
    /* 开机每一步都打一行 INFO：以后“起不来”能直接看出停在哪个阶段，
     * 不用再靠猜（本 SDK 内存分配失败会 __builtin_trap，静默停机）。 */
    LOGI("music init: sync objects ok, psram free %u sram free %u",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* 音乐页是这套固件的主界面：让 radio 让出音频（它开机自己拉过流）。
     * 未编译电台（CONFIG_RADIO=n）时没有这个符号，也就不需要让位。 */
#ifdef CONFIG_RADIO
    {
        extern void radio_app_audio_yield(void);
        radio_app_audio_yield();
    }
#endif

    music_restore_kv();
    LOGI("music init: kv restored (logon=%d)", (int)music_netease_has_login());

    if (music_player != NULL) {
        (void)app_player_register_callback(music_player, music_player_event_cb, NULL);
    } else {
        LOGW("music player instance is NULL");
    }

    s_state.inited = true;
    s_state.boot_ms = music_now_ms();

#ifdef CONFIG_NETEASE_MUSIC_ONLY
    /* 单应用固件：按键由音乐页自己订阅（全功能固件里由 radio 转发过来） */
    (void)voice_msg_sub(VOICE_MSG_BUTTON_CHANGE, music_on_button, NULL);
#endif

    if (music_ui_init() != 0) {
        LOGE("music_ui_init failed");
        s_state.inited = false;
        return -1;
    }
    LOGI("music init: ui ok, psram free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (xTaskCreate(music_task, "music.app", MUSIC_TASK_STACK, NULL, MUSIC_TASK_PRIO, &s_task) !=
        pdPASS) {
        LOGE("failed to create music task");
        s_task = NULL;
        s_state.inited = false;
        return -1;
    }
    LOGI("music init: task started");

    LOGI("netease music ready (milestone 2: player/playlist/lyric/login ui)");
    return 0;
}
