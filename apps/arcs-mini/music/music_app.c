/*
 * 网易云音乐应用的里程碑 1：证明「板子能拿到歌单/直链，并且能真的出声」。
 *
 * 本轮**不做界面**，所有验证都走串口/ADB 的 shell 命令：
 *
 *   music                       打状态
 *   music api 1                 搜索（明文 GET）
 *   music api 2                 歌单详情（EAPI）
 *   music api 3                 歌曲详情（EAPI）
 *   music api 4                 播放直链（EAPI）
 *   music api 5                 扫码 unikey（登录链路入口）
 *   music search <关键词>      同上，参数可选
 *   music playlist <id>
 *   music detail <id>[,<id>...]
 *   music url <id>
 *   music play <id>             取直链并真的播
 *   music chain <关键词>        搜索 → 逐个试直链 → 播第一首能播的（全链路）
 *   music stop
 *
 * 线程模型：letter-shell 的任务栈只有 1024 字节（CONFIG_LISA_SHELL_TASK_STACK_SIZE
 * 默认值），HTTP + JSON 解析绝对跑不动，所以所有网络操作都丢给独立的
 * music 工作线程，shell 只负责提交请求 + 等完成信号 + 打印结果行。
 * 结果行是工作线程写好的静态文本（≤ SHELL_PRINT_BUFFER），shell 逐行打印。
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

#include "shell.h"
#include "sysutils.h"
#include "voice_player_comm.h"

#ifdef CONFIG_RADIO
#include "radio_player.h"
#endif

#include "music_netease.h"

/* ==================== 配置 ==================== */

#define MUSIC_TASK_STACK 6144
#define MUSIC_TASK_PRIO  4
/* shell 等结果的最长时间：EAPI 一次往返 0.3–2 s，链路级操作要连打十几次 */
#define MUSIC_CMD_WAIT_MS 60000

/* 结果行：SHELL_PRINT_BUFFER 是 128，留一点余量 */
#define MUSIC_LOG_LINES 18
#define MUSIC_LOG_LINE  120

/* 演示用固定歌单：网易云「热歌榜」 */
#define MUSIC_DEMO_PLAYLIST 3778678
/* 演示用固定关键词 */
#define MUSIC_DEMO_KEYWORD "周杰伦"

/* ==================== 请求/状态 ==================== */

typedef enum {
    MUSIC_REQ_NONE = 0,
    MUSIC_REQ_SEARCH,
    MUSIC_REQ_PLAYLIST,
    MUSIC_REQ_DETAIL,
    MUSIC_REQ_URL,
    MUSIC_REQ_PLAY,
    MUSIC_REQ_CHAIN,
    MUSIC_REQ_QR,
} music_req_kind_t;

static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t signal;
    SemaphoreHandle_t done;
    TaskHandle_t task;

    music_req_kind_t kind;
    int64_t id;
    int64_t ids[MUSIC_SONG_MAX];
    int id_count;
    char keyword[MUSIC_NAME_MAX];

    volatile bool busy;
    int err;
    int line_count;
    char lines[MUSIC_LOG_LINES][MUSIC_LOG_LINE];
    char playing_url[MUSIC_URL_MAX];
} s_music __psram_bss__;

static volatile bool s_inited;

/* 播放器事件回调（在 app_player 回调线程里执行，只打日志） */
static void music_player_event_cb(app_player_t *player, app_player_event_t event, void *user_data)
{
    (void)player;
    (void)user_data;

    const char *name = "?";
    switch (event) {
    case APP_PLAYER_EVENT_PREPARED:
        name = "prepared";
        break;
    case APP_PLAYER_EVENT_PLAYING:
        name = "playing";
        break;
    case APP_PLAYER_EVENT_PAUSED:
        name = "paused";
        break;
    case APP_PLAYER_EVENT_STOPPED:
        name = "stopped";
        break;
    case APP_PLAYER_EVENT_COMPLETED:
        name = "completed";
        break;
    case APP_PLAYER_EVENT_ERROR:
        name = "error";
        break;
    default:
        break;
    }
    LOGI("player event: %s", name);
}

/* ==================== 结果行 ==================== */

static void log_reset(void)
{
    s_music.line_count = 0;
    for (int i = 0; i < MUSIC_LOG_LINES; i++) {
        s_music.lines[i][0] = '\0';
    }
}

static void log_line(const char *fmt, ...)
{
    va_list args;

    if (s_music.line_count >= MUSIC_LOG_LINES) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(s_music.lines[s_music.line_count], MUSIC_LOG_LINE, fmt, args);
    va_end(args);
    s_music.line_count++;
}

/* ==================== 播放 ==================== */

static int music_start_play(const char *url)
{
    if (music_player == NULL) {
        LOGE("music player instance is NULL");
        return -1;
    }
    if (url == NULL || url[0] == '\0') {
        return -1;
    }

    snprintf(s_music.playing_url, sizeof(s_music.playing_url), "%s", url);
#ifdef CONFIG_RADIO
    /*
     * 里程碑 1 的临时桥接：本分支上 leo-radio 的页面还在，开机就会自动拉流，
     * 而它的焦点优先级高于 music，不先停掉的话音乐刚起播就被抢走。
     * 里程碑 2 用音乐播放页替掉电台页后，这段必须删。
     */
    radio_audio_stop();
#endif
    (void)app_player_stop(music_player);
    const int ret = app_player_play(music_player, url);
    LOGI("play(%d): %s", ret, url);
    return ret;
}

/* ==================== 各请求的实现（工作线程里跑） ==================== */

static void run_search(const char *keyword, int limit)
{
    music_song_list_t list;
    const int err = music_netease_search(keyword, limit, &list);

    s_music.err = err;
    log_line("search '%s' -> err=%d(%s) n=%d", keyword, err, music_err_text(err), list.count);
    for (int i = 0; i < list.count; i++) {
        const music_song_t *song = &list.items[i];
        log_line(" %d. %s / %s / %s id=%lld fee=%d %us", i + 1, song->name, song->artist,
                 song->album, (long long)song->id, song->fee,
                 (unsigned)(song->duration_ms / 1000));
    }
}

static void run_playlist(int64_t id)
{
    music_playlist_t playlist;
    const int err = music_netease_playlist(id, &playlist);

    s_music.err = err;
    log_line("playlist %lld -> err=%d(%s)", (long long)id, err, music_err_text(err));
    log_line(" name=%s tracks=%d ids=%d", playlist.name, playlist.track_count,
             playlist.id_count);
    for (int i = 0; i < playlist.id_count; i++) {
        log_line("  id[%d]=%lld", i, (long long)playlist.ids[i]);
    }
}

static void run_detail(const int64_t *ids, int count)
{
    music_song_list_t list;
    const int err = music_netease_song_detail(ids, count, &list);

    s_music.err = err;
    log_line("detail(%d) -> err=%d(%s) n=%d", count, err, music_err_text(err), list.count);
    for (int i = 0; i < list.count; i++) {
        const music_song_t *song = &list.items[i];
        log_line(" %d. %s / %s / %s id=%lld fee=%d", i + 1, song->name, song->artist,
                 song->album, (long long)song->id, song->fee);
    }
}

static void run_url(int64_t id)
{
    music_track_url_t track;
    const int err = music_netease_track_url(id, &track);

    s_music.err = err;
    log_line("url %lld -> err=%d(%s) code=%d fee=%d br=%d", (long long)id, err,
             music_err_text(err), track.code, track.fee, track.bitrate);
    if (err == MUSIC_OK) {
        log_line(" %s", track.url);
    }
}

static void run_play(int64_t id)
{
    music_track_url_t track;
    const int err = music_netease_track_url(id, &track);

    s_music.err = err;
    log_line("play %lld -> err=%d(%s) code=%d", (long long)id, err, music_err_text(err),
             track.code);
    if (err != MUSIC_OK) {
        return;
    }
    log_line(" url=%s", track.url);
    const int ret = music_start_play(track.url);
    log_line(" app_player_play=%d state=%d", ret, (int)app_player_get_state(music_player));
}

/*
 * 全链路：搜索 → 逐个取直链（跳过不可播的）→ 播第一首成功的。
 * 这是里程碑 1 的验收命令：一条命令走完「拿到歌单 → 拿到可播 URL → 出声」。
 */
static void run_chain(const char *keyword)
{
    music_song_list_t list;
    music_track_url_t track;
    int err;
    int tried = 0;

    log_reset();
    err = music_netease_search(keyword, 6, &list);
    log_line("1) search '%s' err=%d(%s) n=%d", keyword, err, music_err_text(err), list.count);
    if (err != MUSIC_OK) {
        s_music.err = err;
        return;
    }

    for (int i = 0; i < list.count; i++) {
        const music_song_t *song = &list.items[i];
        log_line("2) %d. %s / %s fee=%d", i + 1, song->name, song->artist, song->fee);
        tried++;
        err = music_netease_track_url(song->id, &track);
        if (err == MUSIC_OK) {
            log_line("3) url ok id=%lld br=%d", (long long)song->id, track.bitrate);
            log_line("   %s", track.url);
            const int ret = music_start_play(track.url);
            log_line("4) app_player_play=%d state=%d", ret,
                     (int)app_player_get_state(music_player));
            s_music.err = MUSIC_OK;
            return;
        }
        log_line("   直链失败: %d(%s)", err, music_err_text(err));
    }
    s_music.err = MUSIC_ERR_UNAVAILABLE;
    log_line("所有 %d 首均不可播（多为 VIP/版权限制）", tried);
}

static void run_qr(void)
{
    char key[96];
    const int err = music_netease_qr_start(key, sizeof(key));

    s_music.err = err;
    log_line("qr unikey -> err=%d(%s)", err, music_err_text(err));
    if (err == MUSIC_OK) {
        log_line(" unikey=%s", key);
        log_line(" qr=https://music.163.com/login?codekey=%s", key);
    }
}

/* ==================== 工作线程 ==================== */

static void music_execute(void)
{
    switch (s_music.kind) {
    case MUSIC_REQ_SEARCH:
        run_search(s_music.keyword, 6);
        break;
    case MUSIC_REQ_PLAYLIST:
        run_playlist(s_music.id);
        break;
    case MUSIC_REQ_DETAIL:
        run_detail(s_music.ids, s_music.id_count);
        break;
    case MUSIC_REQ_URL:
        run_url(s_music.id);
        break;
    case MUSIC_REQ_PLAY:
        run_play(s_music.id);
        break;
    case MUSIC_REQ_CHAIN:
        run_chain(s_music.keyword);
        break;
    case MUSIC_REQ_QR:
        run_qr();
        break;
    default:
        break;
    }
}

static void music_task(void *argument)
{
    (void)argument;

    for (;;) {
        if (xSemaphoreTake(s_music.signal, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_music.kind == MUSIC_REQ_NONE) {
            continue;
        }
        s_music.busy = true;
        music_execute();
        s_music.busy = false;
        s_music.kind = MUSIC_REQ_NONE;
        (void)xSemaphoreGive(s_music.done);
    }
}

/* ==================== shell 命令 ==================== */

/* 解析逗号分隔的 id 列表 */
static int parse_ids(const char *text, int64_t *ids, int max)
{
    int count = 0;
    const char *p = text;

    while (p != NULL && *p != '\0' && count < max) {
        char *end = NULL;
        const long long value = strtoll(p, &end, 10);

        if (end == p) {
            break;
        }
        ids[count++] = (int64_t)value;
        p = (*end == ',') ? end + 1 : NULL;
    }
    return count;
}

/* 提交一次请求并等结果；返回 0 表示请求跑完（具体错误看 s_music.err） */
static int music_submit(music_req_kind_t kind, const char *keyword, int64_t id,
                        const int64_t *ids, int id_count)
{
    if (!s_inited) {
        return -1;
    }
    if (s_music.busy) {
        return -2;
    }

    s_music.kind = kind;
    s_music.id = id;
    s_music.keyword[0] = '\0';
    if (keyword != NULL) {
        snprintf(s_music.keyword, sizeof(s_music.keyword), "%s", keyword);
    }
    s_music.id_count = 0;
    if (ids != NULL && id_count > 0) {
        s_music.id_count = (id_count > MUSIC_SONG_MAX) ? MUSIC_SONG_MAX : id_count;
        memcpy(s_music.ids, ids, (size_t)s_music.id_count * sizeof(int64_t));
    }
    log_reset();
    s_music.err = MUSIC_OK;
    s_music.busy = true; /* 先占位，避免工作线程醒来前又被提交一次 */

    /* 排空可能残留的完成信号，避免上一轮超时后这次直接“秒回” */
    (void)xSemaphoreTake(s_music.done, 0);
    (void)xSemaphoreGive(s_music.signal);

    if (xSemaphoreTake(s_music.done, pdMS_TO_TICKS(MUSIC_CMD_WAIT_MS)) != pdTRUE) {
        s_music.busy = false;
        return -3;
    }
    return 0;
}

static void music_print_lines(Shell *shell)
{
    for (int i = 0; i < s_music.line_count; i++) {
        shellPrint(shell, "%s\n", s_music.lines[i]);
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

static int music_cmd(int argc, char *argv[])
{
    Shell *shell = shellGetCurrent();
    const char *sub = (argc >= 2) ? argv[1] : NULL;
    int ret;

    if (shell == NULL) {
        return -1;
    }
    if (!s_inited) {
        shellPrint(shell, "music: not initialized\n");
        return -1;
    }

    if (sub == NULL || strcmp(sub, "stat") == 0) {
        shellPrint(shell, "music: busy=%u player=%d url=%s\n", (unsigned)s_music.busy,
                   music_player != NULL ? (int)app_player_get_state(music_player) : -1,
                   s_music.playing_url);
        shellPrint(shell, "  last err=%d(%s) json=%d\n", s_music.err, music_err_text(s_music.err),
                   music_netease_last_json_len);
        return 0;
    }

    if (strcmp(sub, "stop") == 0) {
        if (music_player != NULL) {
            (void)app_player_stop(music_player);
        }
        s_music.playing_url[0] = '\0';
        shellPrint(shell, "music: stopped\n");
        return 0;
    }

    if (strcmp(sub, "api") == 0) {
        const int which = (argc >= 3) ? atoi(argv[2]) : 0;

        switch (which) {
        case 1:
            ret = music_submit(MUSIC_REQ_SEARCH, MUSIC_DEMO_KEYWORD, 0, NULL, 0);
            break;
        case 2:
            ret = music_submit(MUSIC_REQ_PLAYLIST, NULL, MUSIC_DEMO_PLAYLIST, NULL, 0);
            break;
        case 3: {
            const int64_t demo_ids[3] = {347230, 288750, 400876427};
            ret = music_submit(MUSIC_REQ_DETAIL, NULL, 0, demo_ids, 3);
            break;
        }
        case 4: {
            const int64_t demo_ids[2] = {288750, 400876427};
            ret = music_submit(MUSIC_REQ_URL, NULL, demo_ids[0], NULL, 0);
            break;
        }
        case 5:
            ret = music_submit(MUSIC_REQ_QR, NULL, 0, NULL, 0);
            break;
        default:
            shellPrint(shell, "usage: music api <1..5>\n");
            return -1;
        }
    } else if (strcmp(sub, "search") == 0) {
        ret = music_submit(MUSIC_REQ_SEARCH, (argc >= 3) ? argv[2] : MUSIC_DEMO_KEYWORD, 0, NULL,
                           0);
    } else if (strcmp(sub, "playlist") == 0) {
        ret = music_submit(MUSIC_REQ_PLAYLIST, NULL,
                           (argc >= 3) ? strtoll(argv[2], NULL, 10) : MUSIC_DEMO_PLAYLIST, NULL,
                           0);
    } else if (strcmp(sub, "detail") == 0) {
        int64_t ids[MUSIC_SONG_MAX];
        int count = (argc >= 3) ? parse_ids(argv[2], ids, MUSIC_SONG_MAX) : 0;

        if (count <= 0) {
            shellPrint(shell, "usage: music detail <id>[,<id>...]\n");
            return -1;
        }
        ret = music_submit(MUSIC_REQ_DETAIL, NULL, 0, ids, count);
    } else if (strcmp(sub, "url") == 0) {
        if (argc < 3) {
            shellPrint(shell, "usage: music url <id>\n");
            return -1;
        }
        ret = music_submit(MUSIC_REQ_URL, NULL, strtoll(argv[2], NULL, 10), NULL, 0);
    } else if (strcmp(sub, "play") == 0) {
        if (argc < 3) {
            shellPrint(shell, "usage: music play <id>\n");
            return -1;
        }
        ret = music_submit(MUSIC_REQ_PLAY, NULL, strtoll(argv[2], NULL, 10), NULL, 0);
    } else if (strcmp(sub, "chain") == 0) {
        ret = music_submit(MUSIC_REQ_CHAIN, (argc >= 3) ? argv[2] : MUSIC_DEMO_KEYWORD, 0, NULL,
                           0);
    } else {
        shellPrint(shell, "usage: music [stat|stop|api <n>|search|playlist|detail|url|play|chain]"
                          "\n");
        return -1;
    }

    if (ret == -2) {
        shellPrint(shell, "music: busy\n");
        return -1;
    }
    if (ret == -3) {
        shellPrint(shell, "music: timeout (网络太慢或请求卡住)\n");
        return -1;
    }
    if (ret != 0) {
        shellPrint(shell, "music: submit failed %d\n", ret);
        return -1;
    }
    music_print_lines(shell);
    return (s_music.err == MUSIC_OK) ? 0 : -1;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 music, music_cmd,
                 music [stat|stop|api n|search|playlist|detail|url|play|chain]);

/* ==================== 初始化 ==================== */

int music_app_init(void)
{
    if (s_inited) {
        return 0;
    }

    memset(&s_music, 0, sizeof(s_music));
    s_music.lock = xSemaphoreCreateMutex();
    s_music.signal = xSemaphoreCreateBinary();
    s_music.done = xSemaphoreCreateBinary();
    if (s_music.lock == NULL || s_music.signal == NULL || s_music.done == NULL) {
        LOGE("failed to create sync objects");
        return -1;
    }

    if (music_player != NULL) {
        (void)app_player_register_callback(music_player, music_player_event_cb, NULL);
    } else {
        LOGW("music player instance is NULL");
    }

    if (xTaskCreate(music_task, "music.app", MUSIC_TASK_STACK, NULL, MUSIC_TASK_PRIO,
                    &s_music.task) != pdPASS) {
        LOGE("failed to create music task");
        s_music.task = NULL;
        return -1;
    }

    s_inited = true;
    LOGI("netease music ready (milestone 1: API + playback, no UI yet)");
    return 0;
}
