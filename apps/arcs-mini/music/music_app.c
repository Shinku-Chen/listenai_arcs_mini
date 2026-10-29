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

#define MUSIC_KV_KEY_COOKIE "music.cookie"

/* shell 结果行：SHELL_PRINT_BUFFER 是 128，留一点余量 */
#define MUSIC_LOG_LINES 20
#define MUSIC_LOG_LINE  120

/* 导航页：3 个推荐歌单 + 歌词 + 扫码登录 */
static const struct {
    const char *name;
    int64_t id;
} s_playlists[] = {
    {"热歌榜", 3778678},
    {"新歌榜", 3779629},
    {"飙升榜", 19723756},
};
#define MUSIC_PLAYLIST_COUNT ((uint8_t)(sizeof(s_playlists) / sizeof(s_playlists[0])))
#define MUSIC_MENU_LYRIC     MUSIC_PLAYLIST_COUNT
#define MUSIC_MENU_LOGIN     (MUSIC_PLAYLIST_COUNT + 1)
#define MUSIC_MENU_COUNT     (MUSIC_PLAYLIST_COUNT + 2)

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
    MUSIC_REQ_SEARCH,    /* shell：搜索 */
    MUSIC_REQ_CHAIN,     /* shell：全链路 */
} music_req_kind_t;

/* ==================== 状态 ==================== */

typedef struct {
    bool inited;
    bool started;      /* 开机自动进音乐页只做一次 */
    uint8_t page;      /* music_page_t */
    uint8_t menu_index;
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
    char cookie[512];
    char qr_url[MUSIC_QR_URL_MAX];

    /* 杂项 */
    volatile bool net_ok;
    bool busy;
    char detail[MUSIC_SUB_MAX];
} music_state_t;

static music_state_t s_state __psram_bss__;
static music_song_t s_songs[MUSIC_SONGS_MAX] __psram_bss__;
/* 详情接口的输出缓冲：10 首 * ~370 字节，不能放 6 KB 的工作线程栈里 */
static music_song_list_t s_song_buf __psram_bss__;
static char s_lyric_text[MUSIC_LYRIC_MAX][MUSIC_LYRIC_LINE] __psram_bss__;
static char s_lrc_raw[MUSIC_LRC_CAP] __psram_bss__;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_signal;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;

static struct {
    volatile music_req_kind_t kind;
    uint8_t arg;
    volatile bool busy;
    volatile bool next_pending;
    int err;
    char keyword[MUSIC_NAME_MAX];
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
}

static void music_set_detailf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_state.detail, sizeof(s_state.detail), fmt, args);
    va_end(args);
}

static const music_song_t *music_song_locked(uint8_t index)
{
    if (index >= s_state.count) {
        return NULL;
    }
    return &s_songs[index];
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
static void music_load_playlist(uint8_t menu_index)
{
    music_playlist_t playlist;
    int err;

    if (menu_index >= MUSIC_PLAYLIST_COUNT) {
        s_req.err = MUSIC_ERR_PARAM;
        return;
    }

    music_lock();
    s_state.busy = true;
    music_set_detail("正在加载歌单 …");
    music_unlock();

    err = music_netease_playlist(s_playlists[menu_index].id, &playlist);
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
    s_state.playlist_id = s_playlists[menu_index].id;
    snprintf(s_state.list_title, sizeof(s_state.list_title), "%s", s_playlists[menu_index].name);
    music_set_detail(NULL);
    music_unlock();

    LOGI("playlist %s loaded: %u songs", s_playlists[menu_index].name,
         (unsigned)s_song_buf.count);
}

/* ==================== 扫码登录 ==================== */

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
    char cookie[512];
    int state = MUSIC_QR_WAIT_SCAN;

    music_lock();
    snprintf(key, sizeof(key), "%s", s_state.unikey);
    music_unlock();

    if (key[0] == '\0') {
        return;
    }

    cookie[0] = '\0';
    const int err = music_netease_qr_poll(key, &state, cookie, sizeof(cookie));

    music_lock();
    s_state.qr_at_ms = music_now_ms();
    if (err != MUSIC_OK) {
        music_unlock();
        return;
    }
    s_state.qr_state = (uint16_t)state;

    if (state == MUSIC_QR_OK) {
        if (cookie[0] != '\0') {
            snprintf(s_state.cookie, sizeof(s_state.cookie), "%s", cookie);
            music_set_detail("登录成功");
            music_unlock();
            /* KV 写 flash 不能在持锁时做（要几十毫秒，会拖住渲染抽帧） */
            music_netease_set_cookie(s_state.cookie);
            (void)lisa_kv_set_string(MUSIC_KV_KEY_COOKIE, s_state.cookie);
        } else {
            /* 少数情况下 803 不带 Set-Cookie：别假装登录成功 */
            music_set_detail("登录返回成功但没拿到 cookie");
            music_unlock();
        }
        LOGI("qr login ok, cookie %s", cookie[0] != '\0' ? "saved" : "missing");
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

/* ==================== 工作线程 ==================== */

static void music_execute(void)
{
    switch (s_req.kind) {
    case MUSIC_REQ_PLAYLIST:
        music_load_playlist(s_req.arg);
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

    if (sys_network_get_status(&status) == 0) {
        net_ok = status.connected;
    }
    s_state.net_ok = net_ok;

    /* 开机进页：只有配过网才抢默认页，否则让 radio 的配网二维码留在前台 */
    if (!s_state.started && sys_wifi_has_ap()) {
        s_state.started = true;
        LISA_UI_INVOKE_UI_ARG_NONE({ music_ui_show(); });
        LOGI("music page is the front page now (net=%d)", (int)net_ok);
    }

    if (music_player != NULL) {
        uint32_t pos = 0;
        uint32_t dur = 0;

        if (app_player_get_position(music_player, &pos) == 0) {
            s_state.pos_ms = pos;
        }
        if (app_player_get_duration(music_player, &dur) == 0 && dur > 0) {
            s_state.dur_ms = dur;
        }
        s_state.playing = (app_player_get_state(music_player) == APP_PLAYER_STATE_PLAYING);
    }

    music_lock();
    if (s_state.lyric_count > 0) {
        music_lyric_seek(s_state.pos_ms);
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

    memset(out, 0, sizeof(*out));
    out->page = s_state.page;
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
    snprintf(out->logon, sizeof(out->logon), "%s", (s_state.cookie[0] != '\0') ? "已登录" : "未登录");

    const music_song_t *song = music_song_locked(s_state.current);
    if (song != NULL) {
        snprintf(out->title, sizeof(out->title), "%s", song->name);
        snprintf(out->artist, sizeof(out->artist), "%s", song->artist);
        snprintf(out->album, sizeof(out->album), "%s", song->album);
    } else {
        snprintf(out->title, sizeof(out->title), "网易云音乐");
        snprintf(out->artist, sizeof(out->artist), "%s",
                 s_state.net_ok ? "双击导航页选歌单" : "未连接网络");
        snprintf(out->album, sizeof(out->album), "热歌榜 · 新歌榜 · 飙升榜");
    }

    /* 导航页：3 个推荐歌单 + 歌词 + 扫码登录 */
    for (uint8_t i = 0; i < MUSIC_MENU_COUNT; i++) {
        if (i < MUSIC_PLAYLIST_COUNT) {
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "%s", s_playlists[i].name);
            if (s_state.playlist_id == s_playlists[i].id && s_state.count > 0) {
                snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%u 首",
                         (unsigned)s_state.count);
            }
        } else if (i == MUSIC_MENU_LYRIC) {
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "歌词");
            snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%s",
                     (s_state.lyric_count > 0) ? "有" : "无");
        } else {
            snprintf(out->menu_name[i], sizeof(out->menu_name[i]), "扫码登录");
            snprintf(out->menu_value[i], sizeof(out->menu_value[i]), "%s",
                     (s_state.cookie[0] != '\0') ? "已登录" : "未登录");
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

static void music_volume_step(void)
{
    const int current = service_volume_get();
    int next = current + MUSIC_VOLUME_STEP;

    if (current >= 100 || next > 100) {
        next = 10;
    }
    service_volume_set(next);
}

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
    music_lock();
    const uint8_t index = s_state.menu_index;
    music_unlock();

    if (index < MUSIC_PLAYLIST_COUNT) {
        s_state.page = MUSIC_PAGE_SONGLIST;
        s_req.kind = MUSIC_REQ_PLAYLIST;
        s_req.arg = index;
        (void)xSemaphoreGive(s_signal);
        return;
    }
    if (index == MUSIC_MENU_LYRIC) {
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
    switch ((music_page_t)s_state.page) {
    case MUSIC_PAGE_SONGLIST:
        s_state.page = MUSIC_PAGE_MENU;
        break;
    case MUSIC_PAGE_MENU:
    case MUSIC_PAGE_LYRIC:
    case MUSIC_PAGE_LOGIN:
    case MUSIC_PAGE_PLAYER:
    default:
        s_state.page = MUSIC_PAGE_PLAYER;
        break;
    }
}

bool music_app_handle_action(int action)
{
    if (!s_state.inited || !music_app_is_on_top()) {
        return false;
    }

    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
        switch ((music_page_t)s_state.page) {
        case MUSIC_PAGE_MENU:
            s_state.menu_index = (uint8_t)((s_state.menu_index + 1u) % MUSIC_MENU_COUNT);
            break;
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
            s_state.menu_index = 0;
            s_state.page = MUSIC_PAGE_MENU;
        } else {
            music_back();
        }
        return true;

    case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        /* 关机交给 radio 的系统语义 */
        return false;

    case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
        /* 配网交给 radio（它会把自己切到配网页） */
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
        {"songlist", MUSIC_PAGE_SONGLIST},
        {"lyric", MUSIC_PAGE_LYRIC},
        {"login", MUSIC_PAGE_LOGIN},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcmp(name, map[i].name) == 0) {
            s_state.page = map[i].page;
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
        shellPrint(shell, "  qr=%u logon=%s art=%d detail=%s\n", (unsigned)s_state.qr_state,
                   s_state.cookie[0] != '\0' ? "yes" : "no", s_state.art.length,
                   s_state.detail[0] != '\0' ? s_state.detail : "-");
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
            shellPrint(shell, "usage: music ui player|menu|songlist|lyric|login\n");
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
        default:
            shellPrint(shell, "usage: music api <1..6>\n");
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
        ret = music_submit(MUSIC_REQ_PLAYLIST, (uint8_t)index, NULL);
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
        } else {
            shellPrint(shell, "usage: music login start|poll\n");
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
        shellPrint(shell, "usage: music [stat|stop|ui <page>|api <n>|search|playlist|play|"
                          "next|prev|lyrics|login start|poll|chain|song]\n");
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
                 music [stat|stop|ui|api|search|playlist|play|next|prev|lyrics|login|chain|song]);

/* ==================== 初始化 ==================== */

static void music_restore_kv(void)
{
    char *value = NULL;

    if (lisa_kv_get_string(MUSIC_KV_KEY_COOKIE, &value) == 0 && value != NULL) {
        snprintf(s_state.cookie, sizeof(s_state.cookie), "%s", value);
        lisa_kv_free(value);
        music_netease_set_cookie(s_state.cookie);
        LOGI("restored login cookie (%u bytes)", (unsigned)strlen(s_state.cookie));
    }
}

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

    /* 音乐页是这套固件的主界面：让 radio 让出音频（它开机自己拉过流）。
     * 未编译电台（CONFIG_RADIO=n）时没有这个符号，也就不需要让位。 */
#ifdef CONFIG_RADIO
    {
        extern void radio_app_audio_yield(void);
        radio_app_audio_yield();
    }
#endif

    music_restore_kv();

    if (music_player != NULL) {
        (void)app_player_register_callback(music_player, music_player_event_cb, NULL);
    } else {
        LOGW("music player instance is NULL");
    }

    s_state.inited = true;

    if (music_ui_init() != 0) {
        LOGE("music_ui_init failed");
        s_state.inited = false;
        return -1;
    }

    if (xTaskCreate(music_task, "music.app", MUSIC_TASK_STACK, NULL, MUSIC_TASK_PRIO, &s_task) !=
        pdPASS) {
        LOGE("failed to create music task");
        s_task = NULL;
        s_state.inited = false;
        return -1;
    }

    LOGI("netease music ready (milestone 2: player/playlist/lyric/login ui)");
    return 0;
}
