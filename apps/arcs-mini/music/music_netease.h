/*
 * 网易云音乐直连接口（EAPI 路线）。
 *
 * 移植自 leo-radio 之前的那套 ESP32 实现（FoloToy-wyy-musicplayer 的
 * components/netease_direct_client），这边只保留了里程碑 1 需要的部分：
 *
 *  - EAPI 加解密：AES-128-ECB + MD5，key = "e82ckenh8dichen8"
 *        明文 = path + "-36cd479b6b5-" + json + "-36cd479b6b5-" + md5("nobody"+path+"use"+json+"md5forencrypt")
 *        params = hex(AES-ECB(明文))，大写
 *  - 请求走 http（不是 https）：实测 music.163.com 的 /eapi/ 与 /api/ 都接受明文
 *    HTTP，返回的音频直链也是 http://m7xx.music.126.net/...mp3，所以本里程碑
 *    完全不需要 TLS，省掉 40 KB 的握手内存。
 *  - 匿名可用：搜索、歌单详情、歌曲详情、播放直链都不需要登录；
 *    VIP/付费曲目会返回 code=-110 且 url=null，这里统一映射成
 *    MUSIC_ERR_UNAVAILABLE，由上层跳过。
 *
 * WEAPI（RSA + 双层 AES）暂时没搬：它的用途（用户歌单、账号校验、扫码确认）
 * 都属于里程碑 2 的登录链路，而对应的读接口在 EAPI 上同样存在。
 */
#ifndef __MUSIC_NETEASE_H__
#define __MUSIC_NETEASE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MUSIC_OK = 0,
    MUSIC_ERR_NET = -1,          /* 连不上 / 超时 / HTTP 状态异常 */
    MUSIC_ERR_JSON = -2,         /* 响应不是预期结构 */
    MUSIC_ERR_PARAM = -3,        /* 入参非法 */
    MUSIC_ERR_NO_MEM = -4,       /* 内存不足 */
    MUSIC_ERR_OVERFLOW = -5,     /* 缓冲/响应超长 */
    MUSIC_ERR_UNAVAILABLE = -6,  /* 无版权、需会员、试听限制 */
} music_err_t;

#define MUSIC_NAME_MAX   64
#define MUSIC_ARTIST_MAX 48
#define MUSIC_ALBUM_MAX  48
#define MUSIC_URL_MAX    512
#define MUSIC_SONG_MAX   10

typedef struct {
    int64_t id;
    char name[MUSIC_NAME_MAX];
    char artist[MUSIC_ARTIST_MAX];
    char album[MUSIC_ALBUM_MAX];
    int duration_ms;
    int fee; /* 0/8 可播，1 需会员，4 付费专辑 —— 仅搜索结果里有 */
} music_song_t;

typedef struct {
    int count;
    music_song_t items[MUSIC_SONG_MAX];
} music_song_list_t;

typedef struct {
    int64_t id;
    char url[MUSIC_URL_MAX];
    int code;      /* 服务端 code，200 表示拿到地址 */
    int fee;
    int bitrate;
} music_track_url_t;

typedef struct {
    int64_t id;
    char name[MUSIC_NAME_MAX];
    int track_count;
    int id_count;                 /* 实际取到的 trackIds 数量 */
    int64_t ids[MUSIC_SONG_MAX];  /* 只要前几个，用来逐个查详情 */
} music_playlist_t;

/* 关键词搜索（明文 GET /api/search/get）。keyword 是 UTF-8。 */
int music_netease_search(const char *keyword, int limit, music_song_list_t *out);

/* 歌曲详情（EAPI /api/v3/song/detail），最多 MUSIC_SONG_MAX 首 */
int music_netease_song_detail(const int64_t *ids, int count, music_song_list_t *out);

/* 播放直链（EAPI /api/song/enhance/player/url），br 固定 128000 → mp3 */
int music_netease_track_url(int64_t id, music_track_url_t *out);

/* 歌单详情（EAPI /api/v6/playlist/detail），只取名字、总数、前几首 id */
int music_netease_playlist(int64_t playlist_id, music_playlist_t *out);

/*
 * 扫码登录第一步：申请 unikey（EAPI /api/login/qrcode/unikey）。
 * 拿到 key 后二维码内容就是 https://music.163.com/login?codekey=<key>。
 * 轮询/云端确认/登录态持久化属于里程碑 2，这里只验证接口可通。
 */
int music_netease_qr_start(char *key, size_t key_size);

/* 把错误码转成中文短语，便于日志/串口输出 */
const char *music_err_text(int err);

/*
 * 调试证据：最近一次响应的开头片段 + HTTP 状态码。
 * 里程碑 1 要证明「板子能拿到 JSON 和可播 URL」，串口/ADB 直接打印这段。
 */
#define MUSIC_NETEASE_SNIPPET_MAX 192
extern char music_netease_last_json[MUSIC_NETEASE_SNIPPET_MAX + 1];
extern int music_netease_last_json_len;
extern int music_netease_last_status;

#endif /* __MUSIC_NETEASE_H__ */
