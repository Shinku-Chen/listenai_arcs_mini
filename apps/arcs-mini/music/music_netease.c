#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "music.netease"
#include "lisa_log.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "sysutils.h"

#include "music_http.h"
#include "music_netease.h"

/* ==================== 协议常量 ==================== */

#define MUSIC_API_BASE "http://music.163.com"
#define MUSIC_EAPI_KEY "e82ckenh8dichen8"
/* EAPI 明文里三段内容之间的固定分隔串（网易云客户端硬编码值） */
#define MUSIC_EAPI_SEP "-36cd479b6b5-"

/* 搜索关键词编码后的最大长度 */
#define MUSIC_KEYWORD_ENCODED_MAX 256

/*
 * EAPI 的登录态**不在 Cookie 头里**：真实客户端把 cookie 塞进 params 的
 * header 字段（{"header":{"os":"pc",...,"MUSIC_U":"..."}}）。
 * 只发 Cookie 头对 /eapi/ 网关不可靠 —— 未登录时它照样 code=200，只是
 * account/profile 为 null，看起来像“登录没生效”。
 */
#define MUSIC_EAPI_HEADER_PREFIX                                                             \
    "{\"header\":{\"os\":\"pc\",\"appver\":\"8.9.70\",\"osver\":\"10\",\"deviceId\"" \
    ":\"arcsmini\",\"requestId\":\"0\""

/* Cookie 头只在 jar 很短时才加：httpclient 的上下行头部共用 2048 字节预算 */
#define MUSIC_COOKIE_HEADER_MAX 480

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

/*
 * 登录态 cookie jar（形如 "MUSIC_U=...; __csrf=..."）。
 * 放 PSRAM：MUSIC_U 单个值就可能 1~2 KB，占 SRAM 划不来。
 */
static char s_cookie[MUSIC_HTTP_JAR_MAX] __psram_bss__;

/* 临时缓冲优先放 PSRAM（登后 EAPI 的明文/密文能到 3~4 KB，别去抢内部 SRAM） */
static void *music_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

    if (ptr == NULL) {
        ptr = malloc(size);
    }
    return ptr;
}

static size_t cookie_value_len(const char *name);
static size_t cookie_value_copy(const char *name, char *out, size_t out_size);

void music_netease_set_cookie(const char *cookie)
{
    size_t n = 0;

    s_cookie[0] = '\0';
    if (cookie == NULL) {
        return;
    }
    /* 原样保留 jar（'=' '+' '/' '%' 和 '; ' 分隔符都是合法的），
     * 只丢掉控制字符和双引号（后者会破坏后面拼进 JSON 的 header 字段）。 */
    for (const char *p = cookie; *p != '\0' && n + 1 < sizeof(s_cookie); p++) {
        const unsigned char ch = (unsigned char)*p;

        if (ch < 0x20 || ch == 0x7f || *p == '"') {
            continue;
        }
        s_cookie[n++] = *p;
    }
    s_cookie[n] = '\0';
}

bool music_netease_has_login(void)
{
    return cookie_value_len("MUSIC_U") > 0;
}

size_t music_netease_cookie_copy(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    snprintf(out, out_size, "%s", s_cookie);
    return strlen(out);
}

/* ==================== 小工具 ==================== */

/* 在 jar 里找 name 的值长度（0 表示没有） */
static size_t cookie_value_len(const char *name)
{
    const size_t name_len = strlen(name);
    const char *p = s_cookie;

    while (*p != '\0') {
        const char *item_end = strchr(p, ';');
        size_t item_len = (item_end != NULL) ? (size_t)(item_end - p) : strlen(p);

        while (item_len > 0 && *p == ' ') {
            p++;
            item_len--;
        }
        if (item_len > name_len + 1 && p[name_len] == '=' && memcmp(p, name, name_len) == 0) {
            size_t value_len = item_len - name_len - 1;

            while (value_len > 0 && p[name_len + 1 + value_len - 1] == ' ') {
                value_len--;
            }
            return value_len;
        }
        if (item_end == NULL) {
            break;
        }
        p = item_end + 1;
    }
    return 0;
}

/* 把 jar 里 name 的值拷进 out（NUL 结尾），返回拷贝长度 */
static size_t cookie_value_copy(const char *name, char *out, size_t out_size)
{
    const size_t name_len = strlen(name);
    size_t value_len = cookie_value_len(name);
    const char *p = s_cookie;

    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (value_len == 0 || value_len + 1 > out_size) {
        return 0;
    }
    while (*p != '\0') {
        const char *item_end = strchr(p, ';');
        size_t item_len = (item_end != NULL) ? (size_t)(item_end - p) : strlen(p);

        while (item_len > 0 && *p == ' ') {
            p++;
            item_len--;
        }
        if (item_len > name_len + 1 && p[name_len] == '=' && memcmp(p, name, name_len) == 0) {
            memcpy(out, p + name_len + 1, value_len);
            out[value_len] = '\0';
            return value_len;
        }
        if (item_end == NULL) {
            break;
        }
        p = item_end + 1;
    }
    return 0;
}

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

    if (message_len + 1 > out_size || plain_len + 17 > out_size) {
        return MUSIC_ERR_OVERFLOW;
    }

    message = music_malloc(message_len + 1);
    if (message == NULL) {
        return MUSIC_ERR_NO_MEM;
    }
    written = snprintf(message, message_len + 1, "nobody%suse%smd5forencrypt", path, json);
    if (written < 0 || (size_t)written != message_len) {
        heap_caps_free(message);
        return MUSIC_ERR_PARAM;
    }
    rc = md5_hex(message, message_len, digest);
    heap_caps_free(message);
    if (rc != 0) {
        return MUSIC_ERR_PARAM;
    }

    padded = plain_len + 16 - (plain_len % 16);
    if (7 + padded * 2 + 1 > out_size) {
        return MUSIC_ERR_OVERFLOW;
    }
    plain = music_malloc(padded);
    cipher = music_malloc(padded);
    if (plain == NULL || cipher == NULL) {
        heap_caps_free(plain);
        heap_caps_free(cipher);
        return MUSIC_ERR_NO_MEM;
    }
    written = snprintf((char *)plain, plain_len + 1, "%s%s%s%s%s", path, MUSIC_EAPI_SEP, json,
                       MUSIC_EAPI_SEP, digest);
    if (written < 0 || (size_t)written != plain_len) {
        heap_caps_free(plain);
        heap_caps_free(cipher);
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
    heap_caps_free(plain);
    heap_caps_free(cipher);
    return (rc == 0) ? MUSIC_OK : MUSIC_ERR_PARAM;
}

/*
 * 拼 EAPI 的 params 明文 JSON：auth 为 true 时把 cookie 里的 MUSIC_U / __csrf
 * 放进 header 字段（照真实客户端的做法），否则原样用调用方的 json。
 * 输出缓冲由调用方分配（一般直接放 PSRAM）。
 */
static int eapi_payload_build(const char *json, bool auth, char *out, size_t out_size)
{
    const size_t mu_len = auth ? cookie_value_len("MUSIC_U") : 0;
    const size_t cs_len = auth ? cookie_value_len("__csrf") : 0;
    size_t inner_len = strlen(json);
    const char *inner = json;
    size_t used = 0;
    int n;

    if (!auth || (mu_len == 0 && cs_len == 0)) {
        if (inner_len + 1 > out_size) {
            return MUSIC_ERR_OVERFLOW;
        }
        memcpy(out, json, inner_len + 1);
        return MUSIC_OK;
    }

    n = snprintf(out, out_size, "%s,\"__csrf\":\"", MUSIC_EAPI_HEADER_PREFIX);
    if (n < 0 || (size_t)n >= out_size) {
        return MUSIC_ERR_OVERFLOW;
    }
    used = (size_t)n;
    used += cookie_value_copy("__csrf", out + used, out_size - used);
    n = snprintf(out + used, out_size - used, "\",\"MUSIC_U\":\"");
    if (n < 0 || (size_t)n >= out_size - used) {
        return MUSIC_ERR_OVERFLOW;
    }
    used += (size_t)n;
    used += cookie_value_copy("MUSIC_U", out + used, out_size - used);
    n = snprintf(out + used, out_size - used, "\"}");
    if (n < 0 || (size_t)n >= out_size - used) {
        return MUSIC_ERR_OVERFLOW;
    }
    used += (size_t)n;

    /* 再接上调用方 json 的成员（去掉外层花括号） */
    if (inner_len > 1 && inner[0] == '{' && inner[inner_len - 1] == '}') {
        inner++;
        inner_len -= 2;
        while (inner_len > 0 && (*inner == ' ' || *inner == '\t')) {
            inner++;
            inner_len--;
        }
        if (inner_len > 0) {
            if (used + inner_len + 2 > out_size) {
                return MUSIC_ERR_OVERFLOW;
            }
            out[used++] = ',';
            memcpy(out + used, inner, inner_len);
            used += inner_len;
        }
    }
    if (used + 2 > out_size) {
        return MUSIC_ERR_OVERFLOW;
    }
    out[used++] = '}';
    out[used] = '\0';
    return MUSIC_OK;
}

/*
 * POST 一个 EAPI 接口；api_path 形如 "/api/song/enhance/player/url"。
 * auth 为 true 时把登录态 cookie 放进 params 的 header 字段。
 * payload / body 全部现分配（可能到 7 KB）后再释放，不占工作线程栈。
 */
static int eapi_post_ex(const char *api_path, const char *json, bool auth,
                        music_http_resp_t *resp)
{
    char url[128];
    char headers[sizeof(MUSIC_EAPI_HEADERS) + MUSIC_COOKIE_HEADER_MAX];
    char *payload;
    char *body;
    size_t payload_cap;
    size_t body_cap;
    size_t plain_len;
    size_t padded;
    int n;
    int ret;

    memset(resp, 0, sizeof(*resp));
    n = snprintf(url, sizeof(url), "%s/eapi%s", MUSIC_API_BASE, api_path + 4);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return MUSIC_ERR_PARAM;
    }

    payload_cap = strlen(json) + (auth ? strlen(s_cookie) : 0) + 256;
    payload = music_malloc(payload_cap);
    if (payload == NULL) {
        return MUSIC_ERR_NO_MEM;
    }
    ret = eapi_payload_build(json, auth, payload, payload_cap);
    if (ret != MUSIC_OK) {
        LOGE("eapi payload build failed: %d (cap=%u)", ret, (unsigned)payload_cap);
        heap_caps_free(payload);
        return ret;
    }

    plain_len = strlen(api_path) + strlen(MUSIC_EAPI_SEP) * 2 + strlen(payload) + 32;
    padded = plain_len + 16 - (plain_len % 16);
    body_cap = 7 + padded * 2 + 1;
    body = music_malloc(body_cap);
    if (body == NULL) {
        heap_caps_free(payload);
        return MUSIC_ERR_NO_MEM;
    }
    ret = eapi_build_body(api_path, payload, body, body_cap);
    heap_caps_free(payload);
    if (ret != MUSIC_OK) {
        LOGW("eapi body build failed: %d", ret);
        heap_caps_free(body);
        return ret;
    }

    /* Cookie 头只在 jar 足够短时才加（httpclient 上下行头部共用 2048 字节） */
    {
        const size_t jar_len = strlen(s_cookie);

        if (jar_len > 0 && jar_len < MUSIC_COOKIE_HEADER_MAX) {
            snprintf(headers, sizeof(headers), "%s&Cookie:%s", MUSIC_EAPI_HEADERS, s_cookie);
        } else {
            snprintf(headers, sizeof(headers), "%s", MUSIC_EAPI_HEADERS);
        }
    }

    ret = music_http_request_ex(url, true, body, (int)strlen(body), headers, NULL, 0, resp);
    heap_caps_free(body);
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
    return eapi_post_ex(api_path, json, false, resp);
}

/*
 * 明文 /api/ 接口（不经 EAPI 加密），登录态靠 Cookie 头。
 * 作为 EAPI 网关认不出登录态时的退路 —— 两套网关读 cookie 的位置不同。
 */
static int plain_api_post(const char *api_path, const char *form_body,
                          music_http_resp_t *resp)
{
    char url[128];
    char headers[sizeof(MUSIC_EAPI_HEADERS) + MUSIC_COOKIE_HEADER_MAX];
    const size_t jar_len = strlen(s_cookie);
    int n;
    int ret;

    memset(resp, 0, sizeof(*resp));
    if (jar_len == 0 || jar_len >= MUSIC_COOKIE_HEADER_MAX) {
        return MUSIC_ERR_PARAM;
    }
    n = snprintf(url, sizeof(url), "%s%s", MUSIC_API_BASE, api_path);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return MUSIC_ERR_PARAM;
    }
    snprintf(headers, sizeof(headers), "%s&Cookie:%s", MUSIC_EAPI_HEADERS, s_cookie);

    ret = music_http_request_ex(url, true, form_body, form_body ? (int)strlen(form_body) : 0,
                                headers, NULL, 0, resp);
    if (ret != 0) {
        return MUSIC_ERR_NET;
    }
    keep_snippet(resp);
    if (resp->status != 200) {
        LOGW("http %d: %s", resp->status, api_path);
        return MUSIC_ERR_NET;
    }
    LOGI("plain api ok: %s", api_path);
    return MUSIC_OK;
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

int music_netease_qr_poll(const char *key, int *state, bool *got_login)
{
    char json[128];
    char *body;
    const size_t body_cap = 1024 + strlen(key) * 4;
    music_http_resp_t resp;
    cJSON *root;
    const cJSON *code;
    int ret;

    if (key == NULL || key[0] == '\0' || state == NULL) {
        return MUSIC_ERR_PARAM;
    }
    *state = MUSIC_QR_WAIT_SCAN;
    if (got_login != NULL) {
        *got_login = false;
    }

    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"type\":3}", key);

    body = music_malloc(body_cap);
    if (body == NULL) {
        return MUSIC_ERR_NO_MEM;
    }
    /* 不能走 parse_eapi_root：这个接口的 code 就是 800/801/802/803 */
    ret = eapi_build_body("/api/login/qrcode/client/login", json, body, body_cap);
    if (ret != MUSIC_OK) {
        LOGE("qr poll body build failed: %d", ret);
        heap_caps_free(body);
        return ret;
    }

    /*
     * 这一步刻意不用 httpclient：登录成功（803）时登录令牌在 Set-Cookie 里，
     * MUSIC_U 单条就可能超过 1200 字节，而 httpclient 的上下行头部**共用**
     * 2048 字节，放不下就会整个请求失败或者只拿到半条 cookie。
     * 裸 socket 版把头部上限放到 8 KB，并把每一条 Set-Cookie 合并进 s_cookie。
     */
    ret = music_http_raw_post(MUSIC_API_BASE "/eapi/login/qrcode/client/login",
                              "Content-Type: application/x-www-form-urlencoded\r\n"
                              "Referer: https://music.163.com/\r\n",
                              body, (int)strlen(body), s_cookie, sizeof(s_cookie), &resp);
    heap_caps_free(body);
    if (ret != 0) {
        LOGE("qr poll request failed: %d", ret);
        music_http_resp_free(&resp);
        return MUSIC_ERR_NET;
    }
    keep_snippet(&resp);

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

    if (got_login != NULL) {
        *got_login = music_netease_has_login();
    }
    LOGI("qr poll code=%d jar=%u bytes login=%d", *state, (unsigned)strlen(s_cookie),
         (int)music_netease_has_login());
    return MUSIC_OK;
}

/* 解析 /api/nuser/account/get 的响应（EAPI 与明文 /api/ 两种网关结构一致） */
static int account_parse(const music_http_resp_t *resp, int64_t *uid_out, char *nickname,
                         size_t nickname_size)
{
    cJSON *root;
    const cJSON *profile;
    const cJSON *account;

    root = parse_eapi_root(resp);
    if (root == NULL) {
        return MUSIC_ERR_JSON;
    }
    profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    account = cJSON_GetObjectItemCaseSensitive(root, "account");
    if (cJSON_IsObject(profile)) {
        *uid_out = json_i64(cJSON_GetObjectItemCaseSensitive(profile, "userId"));
        if (nickname != NULL && nickname_size > 0) {
            copy_text(nickname, nickname_size, json_text(profile, "nickname"));
        }
    }
    if (*uid_out <= 0 && cJSON_IsObject(account)) {
        *uid_out = json_i64(cJSON_GetObjectItemCaseSensitive(account, "id"));
    }
    cJSON_Delete(root);
    return MUSIC_OK;
}

int music_netease_account(int64_t *uid_out, char *nickname, size_t nickname_size)
{
    music_http_resp_t resp;
    int ret;

    if (uid_out == NULL) {
        return MUSIC_ERR_PARAM;
    }
    *uid_out = 0;
    if (nickname != NULL && nickname_size > 0) {
        nickname[0] = '\0';
    }
    if (!music_netease_has_login()) {
        return MUSIC_ERR_UNAVAILABLE;
    }

    /* 1) EAPI：cookie 放在 params 的 header 字段里 */
    ret = eapi_post_ex("/api/nuser/account/get", "{}", true, &resp);
    if (ret == MUSIC_OK) {
        ret = account_parse(&resp, uid_out, nickname, nickname_size);
    }
    music_http_resp_free(&resp);

    /* 2) 明文 /api/：cookie 放在 Cookie 头里（两套网关读的位置不同） */
    if (*uid_out <= 0) {
        LOGI("eapi account empty (ret=%d), retry plain /api/", ret);
        ret = plain_api_post("/api/nuser/account/get", "", &resp);
        if (ret == MUSIC_OK) {
            (void)account_parse(&resp, uid_out, nickname, nickname_size);
        }
        music_http_resp_free(&resp);
    }

    /* 未登录时服务端照样返回 200，只是 account/profile 为 null */
    LOGI("account uid=%lld nick=%s", (long long)*uid_out, (nickname != NULL) ? nickname : "");
    return (*uid_out > 0) ? MUSIC_OK : MUSIC_ERR_UNAVAILABLE;
}

/* 解析 /api/user/playlist 的响应 */
static int playlists_parse(const music_http_resp_t *resp, music_user_playlist_list_t *out)
{
    cJSON *root;
    const cJSON *list;
    const cJSON *item;

    root = parse_eapi_root(resp);
    if (root == NULL) {
        return MUSIC_ERR_JSON;
    }
    list = cJSON_GetObjectItemCaseSensitive(root, "playlist");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return MUSIC_ERR_JSON;
    }
    cJSON_ArrayForEach(item, list) {
        music_user_playlist_t *slot;

        if (out->count >= MUSIC_USER_PLAYLIST_MAX) {
            break;
        }
        if (!cJSON_IsObject(item)) {
            continue;
        }
        slot = &out->items[out->count];
        slot->id = json_i64(cJSON_GetObjectItemCaseSensitive(item, "id"));
        if (slot->id <= 0) {
            continue;
        }
        copy_text(slot->name, sizeof(slot->name), json_text(item, "name"));
        slot->track_count = (int)json_i64(cJSON_GetObjectItemCaseSensitive(item, "trackCount"));
        if (slot->name[0] == '\0') {
            snprintf(slot->name, sizeof(slot->name), "歌单 %lld", (long long)slot->id);
        }
        out->count++;
    }
    cJSON_Delete(root);
    return MUSIC_OK;
}

int music_netease_user_playlists(int64_t uid, music_user_playlist_list_t *out)
{
    char json[160];
    char form[160];
    music_http_resp_t resp;
    int ret;

    if (uid < 0 || out == NULL) {
        return MUSIC_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));

    snprintf(json, sizeof(json), "{\"uid\":%lld,\"limit\":%d,\"offset\":0}", (long long)uid,
             MUSIC_USER_PLAYLIST_MAX);
    snprintf(form, sizeof(form), "uid=%lld&limit=%d&offset=0&csrf_token=", (long long)uid,
             MUSIC_USER_PLAYLIST_MAX);

    /* 1) EAPI：cookie 放在 params 的 header 字段里 */
    ret = eapi_post_ex("/api/user/playlist", json, true, &resp);
    if (ret == MUSIC_OK) {
        ret = playlists_parse(&resp, out);
    }
    music_http_resp_free(&resp);

    /* 2) 明文 /api/：cookie 放在 Cookie 头里 */
    if (out->count == 0) {
        LOGI("eapi playlist empty (ret=%d), retry plain /api/", ret);
        ret = plain_api_post("/api/user/playlist", form, &resp);
        if (ret == MUSIC_OK) {
            (void)playlists_parse(&resp, out);
        }
        music_http_resp_free(&resp);
    }

    LOGI("my playlists: %d (ret=%d)", (int)out->count, ret);
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
