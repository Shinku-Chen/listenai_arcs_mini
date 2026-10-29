#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "music.netease"
#include "lisa_log.h"

#include "cJSON.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"

#include "music_http.h"
#include "music_netease.h"

/* ==================== 协议常量 ==================== */

#define MUSIC_API_BASE "http://music.163.com"
#define MUSIC_EAPI_KEY "e82ckenh8dichen8"
/* EAPI 明文里三段内容之间的固定分隔串（网易云客户端硬编码值） */
#define MUSIC_EAPI_SEP "-36cd479b6b5-"

/* params=<hex> 请求体上限（10 首歌详情实测约 700 字节） */
#define MUSIC_EAPI_BODY_MAX 1024
/* 搜索关键词编码后的最大长度 */
#define MUSIC_KEYWORD_ENCODED_MAX 256

/*
 * 所有 EAPI 请求都要带这两个头。
 * 格式是 httpclient 的扩展头约定：'&' 分隔多组、Name:Value。
 * 注意值前面不能带空格：HTTPIntrnHeadersAdd 自己会拼 ": "，多一个空格就变成
 * "Content-Type:  application/..."（两个空格）。
 */
#define MUSIC_EAPI_HEADERS \
    "Content-Type:application/x-www-form-urlencoded&Referer:https://music.163.com/"

/* 调试证据：最近一次响应 JSON 的开头片段（README/报告里要贴） */
char music_netease_last_json[MUSIC_NETEASE_SNIPPET_MAX + 1];
int music_netease_last_json_len;
int music_netease_last_status;

/* 登录态 cookie（只有 MUSIC_U=...，已去掉 '&' ';' 等会破环扩展头格式的字符） */
static char s_cookie[512];

/* 扩展头拼接缓冲（基础头 ~90 字节 + cookie 最长 512） */
#define MUSIC_EAPI_HEADERS_MAX (sizeof(MUSIC_EAPI_HEADERS) + sizeof(s_cookie) + 16)

void music_netease_set_cookie(const char *cookie)
{
    size_t n = 0;

    s_cookie[0] = '\0';
    if (cookie == NULL) {
        return;
    }
    for (const char *p = cookie; *p != '\0' && n + 1 < sizeof(s_cookie); p++) {
        if (*p == '&' || *p == ';' || *p == '\r' || *p == '\n') {
            continue;
        }
        s_cookie[n++] = *p;
    }
    s_cookie[n] = '\0';
}

/* ==================== 小工具 ==================== */

/* 按 UTF-8 字符边界截断拷贝 */
static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t n;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 取对象的字符串字段；不存在或类型不对时返回空串 */
static const char *json_text(const cJSON *object, const char *key)
{
    const cJSON *item;

    if (!cJSON_IsObject(object)) {
        return "";
    }
    item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}

static int64_t json_i64(const cJSON *item)
{
    if (cJSON_IsNumber(item)) {
        return (int64_t)item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
        return strtoll(item->valuestring, NULL, 10);
    }
    return 0;
}

/* 数组形式字段（ar / artists / al / album）的第一项名字 */
static void json_first_name(const cJSON *array, char *out, size_t out_size)
{
    const cJSON *first = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, 0) : NULL;

    copy_text(out, out_size, json_text(first, "name"));
}

static void keep_snippet(const music_http_resp_t *resp)
{
    size_t n;

    music_netease_last_status = resp->status;
    if (resp->body == NULL || resp->length <= 0) {
        music_netease_last_json[0] = '\0';
        music_netease_last_json_len = 0;
        return;
    }
    n = (size_t)resp->length;
    if (n > MUSIC_NETEASE_SNIPPET_MAX) {
        n = MUSIC_NETEASE_SNIPPET_MAX;
    }
    memcpy(music_netease_last_json, resp->body, n);
    music_netease_last_json[n] = '\0';
    music_netease_last_json_len = (int)n;
}

/* 解析响应根对象；EAPI 的 code 不是 200 视为失败 */
static cJSON *parse_eapi_root(const music_http_resp_t *resp)
{
    cJSON *root;
    const cJSON *code;

    if (resp->body == NULL || resp->length <= 0) {
        return NULL;
    }
    root = cJSON_ParseWithLength(resp->body, (size_t)resp->length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (code != NULL && json_i64(code) != 200) {
        LOGW("api code=%d", (int)json_i64(code));
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* ==================== EAPI 加解密 ==================== */

static int md5_hex(const char *data, size_t len, char out[33])
{
    unsigned char digest[16];

    if (mbedtls_md5_ret((const unsigned char *)data, len, digest) != 0) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    return 0;
}

/*
 * params = "params=" + upper(hex(AES-128-ECB(plain)))
 * plain  = path + "-36cd479b6b5-" + json + "-36cd479b6b5-" +
 *          md5("nobody" + path + "use" + json + "md5forencrypt")
 */
static int eapi_build_body(const char *path, const char *json, char *out, size_t out_size)
{
    const size_t path_len = strlen(path);
    const size_t json_len = strlen(json);
    const size_t message_len = 6 + path_len + 3 + json_len + 13;
    const size_t plain_len = path_len + strlen(MUSIC_EAPI_SEP) + json_len +
                             strlen(MUSIC_EAPI_SEP) + 32;
    char digest[33];
    char *message;
    unsigned char *plain;
    unsigned char *cipher;
    size_t padded;
    int written;
    int rc;

    if (message_len + 1 > MUSIC_EAPI_BODY_MAX || plain_len + 17 > MUSIC_EAPI_BODY_MAX) {
        return MUSIC_ERR_OVERFLOW;
    }

    message = malloc(message_len + 1);
    if (message == NULL) {
        return MUSIC_ERR_NO_MEM;
    }
    written = snprintf(message, message_len + 1, "nobody%suse%smd5forencrypt", path, json);
    if (written < 0 || (size_t)written != message_len) {
        free(message);
        return MUSIC_ERR_PARAM;
    }
    rc = md5_hex(message, message_len, digest);
    free(message);
    if (rc != 0) {
        return MUSIC_ERR_PARAM;
    }

    padded = plain_len + 16 - (plain_len % 16);
    if (7 + padded * 2 + 1 > out_size) {
        return MUSIC_ERR_OVERFLOW;
    }
    plain = calloc(1, padded);
    cipher = calloc(1, padded);
    if (plain == NULL || cipher == NULL) {
        free(plain);
        free(cipher);
        return MUSIC_ERR_NO_MEM;
    }
    written = snprintf((char *)plain, plain_len + 1, "%s%s%s%s%s", path, MUSIC_EAPI_SEP, json,
                       MUSIC_EAPI_SEP, digest);
    if (written < 0 || (size_t)written != plain_len) {
        free(plain);
        free(cipher);
        return MUSIC_ERR_PARAM;
    }
    /* snprintf 已经写了 NUL，AES 需要标准 PKCS#7 填充，所以补齐填充字节覆盖掉它 */
    memset(plain + plain_len, (int)(padded - plain_len), padded - plain_len);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    rc = mbedtls_aes_setkey_enc(&aes, (const unsigned char *)MUSIC_EAPI_KEY, 128);
    for (size_t i = 0; rc == 0 && i < padded; i += 16) {
        rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plain + i, cipher + i);
    }
    mbedtls_aes_free(&aes);

    if (rc == 0) {
        memcpy(out, "params=", 7);
        for (size_t i = 0; i < padded; i++) {
            snprintf(out + 7 + i * 2, 3, "%02X", cipher[i]);
        }
    }
    free(plain);
    free(cipher);
    return (rc == 0) ? MUSIC_OK : MUSIC_ERR_PARAM;
}

/* POST 一个 EAPI 接口；api_path 形如 "/api/song/enhance/player/url" */
static int eapi_post_ex(const char *api_path, const char *json, char *cookie_out,
                        size_t cookie_size, music_http_resp_t *resp)
{
    char url[128];
    char body[MUSIC_EAPI_BODY_MAX];
    char headers[MUSIC_EAPI_HEADERS_MAX];
    const char *hdrs = MUSIC_EAPI_HEADERS;
    int n;
    int ret;

    /* 登录后带上 Cookie（匿名也能用，所以只影响推荐/音质/歌单可见性） */
    if (s_cookie[0] != '\0') {
        snprintf(headers, sizeof(headers), "%s&Cookie:%s", MUSIC_EAPI_HEADERS, s_cookie);
        hdrs = headers;
    }

    /* 失败路径上调用方仍会 music_http_resp_free()，所以先清干净 */
    memset(resp, 0, sizeof(*resp));
    n = snprintf(url, sizeof(url), "%s/eapi%s", MUSIC_API_BASE, api_path + 4);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return MUSIC_ERR_PARAM;
    }
    ret = eapi_build_body(api_path, json, body, sizeof(body));
    if (ret != MUSIC_OK) {
        LOGW("eapi body build failed: %d", ret);
        return ret;
    }
    ret = music_http_request_ex(url, true, body, (int)strlen(body), hdrs, cookie_out,
                               cookie_size, resp);
    if (ret != 0) {
        return MUSIC_ERR_NET;
    }
    keep_snippet(resp);
    if (resp->status != 200) {
        LOGW("http %d: %s", resp->status, api_path);
        return MUSIC_ERR_NET;
    }
    return MUSIC_OK;
}

static int eapi_post(const char *api_path, const char *json, music_http_resp_t *resp)
{
    return eapi_post_ex(api_path, json, NULL, 0, resp);
}

/* 百分号编码（搜索关键词是 UTF-8 中文，必须编码） */
static int url_encode(const char *input, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;

    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; p++) {
        const bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' ||
                          *p == '~';
        const size_t need = safe ? 1 : 3;
        if (used + need + 1 > out_size) {
            return MUSIC_ERR_OVERFLOW;
        }
        if (safe) {
            out[used++] = (char)*p;
        } else {
            out[used++] = '%';
            out[used++] = hex[*p >> 4];
            out[used++] = hex[*p & 0x0F];
        }
    }
    out[used] = '\0';
    return MUSIC_OK;
}

/* ==================== 业务接口 ==================== */

const char *music_err_text(int err)
{
    switch (err) {
    case MUSIC_OK:
        return "ok";
    case MUSIC_ERR_NET:
        return "网络/HTTP 失败";
    case MUSIC_ERR_JSON:
        return "响应结构异常";
    case MUSIC_ERR_PARAM:
        return "参数非法";
    case MUSIC_ERR_NO_MEM:
        return "内存不足";
    case MUSIC_ERR_OVERFLOW:
        return "缓冲超长";
    case MUSIC_ERR_UNAVAILABLE:
        return "不可播放（无版权/需会员）";
    default:
        return "未知错误";
    }
}

int music_netease_search(const char *keyword, int limit, music_song_list_t *out)
{
    char encoded[MUSIC_KEYWORD_ENCODED_MAX];
    char url[512];
    music_http_resp_t resp;
    cJSON *root;
    const cJSON *result;
    const cJSON *songs;
    const cJSON *item;
    int ret;
    int n;

    if (keyword == NULL || keyword[0] == '\0' || out == NULL) {
        return MUSIC_ERR_PARAM;
    }
    if (limit <= 0 || limit > MUSIC_SONG_MAX) {
        limit = MUSIC_SONG_MAX;
    }
    memset(out, 0, sizeof(*out));

    ret = url_encode(keyword, encoded, sizeof(encoded));
    if (ret != MUSIC_OK) {
        return ret;
    }
    n = snprintf(url, sizeof(url), "%s/api/search/get?s=%s&type=1&limit=%d", MUSIC_API_BASE,
                 encoded, limit);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return MUSIC_ERR_OVERFLOW;
    }

    ret = music_http_request(url, false, NULL, 0, NULL, &resp);
    if (ret != 0) {
        return MUSIC_ERR_NET;
    }
    keep_snippet(&resp);
    if (resp.status != 200) {
        LOGW("search http %d", resp.status);
        music_http_resp_free(&resp);
        return MUSIC_ERR_NET;
    }

    root = (resp.body != NULL) ? cJSON_ParseWithLength(resp.body, (size_t)resp.length) : NULL;
    music_http_resp_free(&resp);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }

    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    songs = cJSON_IsObject(result) ? cJSON_GetObjectItemCaseSensitive(result, "songs") : NULL;
    if (!cJSON_IsArray(songs)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }

    cJSON_ArrayForEach(item, songs) {
        music_song_t *song;
        const cJSON *album;

        if (out->count >= MUSIC_SONG_MAX) {
            break;
        }
        if (!cJSON_IsObject(item)) {
            continue;
        }
        song = &out->items[out->count];
        song->id = json_i64(cJSON_GetObjectItemCaseSensitive(item, "id"));
        if (song->id <= 0) {
            continue;
        }
        copy_text(song->name, sizeof(song->name), json_text(item, "name"));
        json_first_name(cJSON_GetObjectItemCaseSensitive(item, "artists"), song->artist,
                        sizeof(song->artist));
        album = cJSON_GetObjectItemCaseSensitive(item, "album");
        copy_text(song->album, sizeof(song->album), json_text(album, "name"));
        copy_text(song->cover, sizeof(song->cover), json_text(album, "picUrl"));
        song->duration_ms = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "duration"));
        song->fee = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "fee"));
        out->count++;
    }
    cJSON_Delete(root);
    return (out->count > 0) ? MUSIC_OK : MUSIC_ERR_JSON;
}

int music_netease_song_detail(const int64_t *ids, int count, music_song_list_t *out)
{
    char json[512];
    music_http_resp_t resp;
    cJSON *root;
    const cJSON *songs;
    const cJSON *item;
    size_t used;
    int ret;

    if (ids == NULL || count <= 0 || count > MUSIC_SONG_MAX || out == NULL) {
        return MUSIC_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));

    /* 这个接口要的是被转义成字符串的数组：{"c":"[{\"id\":1},{\"id\":2}]"} */
    used = (size_t)snprintf(json, sizeof(json), "{\"c\":\"[");
    for (int i = 0; i < count; i++) {
        int n = snprintf(json + used, sizeof(json) - used, "%s{\\\"id\\\":%lld}", i ? "," : "",
                         (long long)ids[i]);
        if (n < 0 || (size_t)n >= sizeof(json) - used) {
            return MUSIC_ERR_OVERFLOW;
        }
        used += (size_t)n;
    }
    if (used + 3 > sizeof(json)) {
        return MUSIC_ERR_OVERFLOW;
    }
    snprintf(json + used, sizeof(json) - used, "]\"}");

    ret = eapi_post("/api/v3/song/detail", json, &resp);
    if (ret != MUSIC_OK) {
        music_http_resp_free(&resp);
        return ret;
    }
    root = parse_eapi_root(&resp);
    music_http_resp_free(&resp);
    if (root == NULL) {
        return MUSIC_ERR_JSON;
    }

    songs = cJSON_GetObjectItemCaseSensitive(root, "songs");
    if (!cJSON_IsArray(songs)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }
    cJSON_ArrayForEach(item, songs) {
        music_song_t *song;
        const cJSON *album;

        if (out->count >= MUSIC_SONG_MAX) {
            break;
        }
        if (!cJSON_IsObject(item)) {
            continue;
        }
        song = &out->items[out->count];
        song->id = json_i64(cJSON_GetObjectItemCaseSensitive(item, "id"));
        if (song->id <= 0) {
            continue;
        }
        copy_text(song->name, sizeof(song->name), json_text(item, "name"));
        json_first_name(cJSON_GetObjectItemCaseSensitive(item, "ar"), song->artist,
                        sizeof(song->artist));
        album = cJSON_GetObjectItemCaseSensitive(item, "al");
        copy_text(song->album, sizeof(song->album), json_text(album, "name"));
        copy_text(song->cover, sizeof(song->cover), json_text(album, "picUrl"));
        song->duration_ms = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "dt"));
        song->fee = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "fee"));
        out->count++;
    }
    cJSON_Delete(root);
    return (out->count > 0) ? MUSIC_OK : MUSIC_ERR_JSON;
}

int music_netease_track_url(int64_t id, music_track_url_t *out)
{
    char json[256];
    music_http_resp_t resp;
    cJSON *root;
    const cJSON *data;
    const cJSON *item;
    const cJSON *url;
    int ret;

    if (id <= 0 || out == NULL) {
        return MUSIC_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));
    out->id = id;

    snprintf(json, sizeof(json),
             "{\"ids\":\"[%lld]\",\"br\":128000,\"encodeType\":\"mp3\","
             "\"header\":{\"os\":\"pc\",\"appver\":\"2.9.7\"}}",
             (long long)id);

    ret = eapi_post("/api/song/enhance/player/url", json, &resp);
    if (ret != MUSIC_OK) {
        music_http_resp_free(&resp);
        return ret;
    }
    root = parse_eapi_root(&resp);
    music_http_resp_free(&resp);
    if (root == NULL) {
        return MUSIC_ERR_JSON;
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    item = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
    if (!cJSON_IsObject(item)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }
    out->code = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "code"));
    out->fee = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "fee"));
    out->bitrate = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "br"));

    url = cJSON_GetObjectItemCaseSensitive(item, "url");
    if (cJSON_IsString(url) && url->valuestring != NULL) {
        copy_text(out->url, sizeof(out->url), url->valuestring);
    }
    cJSON_Delete(root);

    if (out->url[0] == '\0') {
        /* 无版权 / 需会员：code 一般是 -110，fee 是 1 或 4 */
        return MUSIC_ERR_UNAVAILABLE;
    }
    if (out->code != 200) {
        return MUSIC_ERR_UNAVAILABLE;
    }
    if (strncmp(out->url, "http://", 7) != 0 && strncmp(out->url, "https://", 8) != 0) {
        return MUSIC_ERR_JSON;
    }
    return MUSIC_OK;
}

int music_netease_playlist(int64_t playlist_id, music_playlist_t *out)
{
    char json[128];
    music_http_resp_t resp;
    cJSON *root;
    const cJSON *playlist;
    const cJSON *ids;
    const cJSON *item;
    int ret;

    if (playlist_id <= 0 || out == NULL) {
        return MUSIC_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));
    out->id = playlist_id;

    snprintf(json, sizeof(json), "{\"id\":%lld,\"n\":0,\"s\":0}", (long long)playlist_id);

    ret = eapi_post("/api/v6/playlist/detail", json, &resp);
    if (ret != MUSIC_OK) {
        music_http_resp_free(&resp);
        return ret;
    }
    root = parse_eapi_root(&resp);
    music_http_resp_free(&resp);
    if (root == NULL) {
        return MUSIC_ERR_JSON;
    }

    playlist = cJSON_GetObjectItemCaseSensitive(root, "playlist");
    if (!cJSON_IsObject(playlist)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }
    copy_text(out->name, sizeof(out->name), json_text(playlist, "name"));
    out->track_count = (int)json_i64(cJSON_GetObjectItemCaseSensitive(playlist, "trackCount"));

    ids = cJSON_GetObjectItemCaseSensitive(playlist, "trackIds");
    if (cJSON_IsArray(ids)) {
        cJSON_ArrayForEach(item, ids) {
            if (out->id_count >= MUSIC_SONG_MAX) {
                break;
            }
            const int64_t id = json_i64(cJSON_GetObjectItemCaseSensitive(item, "id"));
            if (id > 0) {
                out->ids[out->id_count++] = id;
            }
        }
    }
    cJSON_Delete(root);
    return MUSIC_OK;
}

int music_netease_qr_start(char *key, size_t key_size)
{
    music_http_resp_t resp;
    cJSON *root;
    const char *value;
    size_t n;
    int ret;

    if (key == NULL || key_size < 32) {
        return MUSIC_ERR_PARAM;
    }
    key[0] = '\0';

    ret = eapi_post("/api/login/qrcode/unikey", "{\"type\":3}", &resp);
    if (ret != MUSIC_OK) {
        music_http_resp_free(&resp);
        return ret;
    }
    root = parse_eapi_root(&resp);
    music_http_resp_free(&resp);
    if (root == NULL) {
        return MUSIC_ERR_JSON;
    }
    value = json_text(root, "unikey");
    n = strlen(value);
    if (n == 0 || n >= key_size) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }
    memcpy(key, value, n + 1);
    cJSON_Delete(root);
    return MUSIC_OK;
}

int music_netease_qr_poll(const char *key, int *state, char *cookie_out, size_t cookie_size)
{
    char json[128];
    music_http_resp_t resp;
    cJSON *root;
    const cJSON *code;
    int ret;

    if (key == NULL || key[0] == '\0' || state == NULL) {
        return MUSIC_ERR_PARAM;
    }
    *state = MUSIC_QR_WAIT_SCAN;
    if (cookie_out != NULL && cookie_size > 0) {
        cookie_out[0] = '\0';
    }

    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"type\":3}", key);

    /* 不能走 parse_eapi_root：这个接口的 code 就是 800/801/802/803 */
    ret = eapi_post_ex("/api/login/qrcode/client/login", json, cookie_out, cookie_size, &resp);
    if (ret != MUSIC_OK) {
        music_http_resp_free(&resp);
        return ret;
    }
    root = (resp.body != NULL) ? cJSON_ParseWithLength(resp.body, (size_t)resp.length) : NULL;
    music_http_resp_free(&resp);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }

    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    *state = (int)json_i64(code);
    if (*state == 0) {
        /* 有些服务端版本成功时不回 code，只回 message="success" */
        *state = MUSIC_QR_WAIT_SCAN;
    }
    cJSON_Delete(root);
    return MUSIC_OK;
}

int music_netease_lyric(int64_t id, char *lrc, size_t lrc_size)
{
    char url[160];
    music_http_resp_t resp;
    cJSON *root;
    const cJSON *lrcobj;
    int ret;
    int n;

    if (id <= 0 || lrc == NULL || lrc_size < 64) {
        return MUSIC_ERR_PARAM;
    }
    lrc[0] = '\0';

    n = snprintf(url, sizeof(url), "%s/api/song/lyric?id=%lld&lv=-1&kv=-1&tv=-1", MUSIC_API_BASE,
                 (long long)id);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return MUSIC_ERR_OVERFLOW;
    }

    memset(&resp, 0, sizeof(resp));
    ret = music_http_request(url, false, NULL, 0, NULL, &resp);
    if (ret != 0) {
        return MUSIC_ERR_NET;
    }
    keep_snippet(&resp);
    if (resp.status != 200) {
        LOGW("lyric http %d", resp.status);
        music_http_resp_free(&resp);
        return MUSIC_ERR_NET;
    }

    root = (resp.body != NULL) ? cJSON_ParseWithLength(resp.body, (size_t)resp.length) : NULL;
    music_http_resp_free(&resp);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }

    lrcobj = cJSON_GetObjectItemCaseSensitive(root, "lrc");
    if (cJSON_IsObject(lrcobj)) {
        copy_text(lrc, lrc_size, json_text(lrcobj, "lyric"));
    }
    cJSON_Delete(root);
    return (lrc[0] != '\0') ? MUSIC_OK : MUSIC_ERR_UNAVAILABLE;
}
