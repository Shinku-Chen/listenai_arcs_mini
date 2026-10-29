#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "music.netease"
#include "lisa_log.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
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

/* cookie 里 __csrf 的最大长度 */
#define MUSIC_CSRF_MAX 64

/*
 * 客户端身份。网易云把登录令牌（MUSIC_U）跟**签发时的请求身份**绑在一起：
 * 扫码轮询时如果 EAPI header 里没有 os/appver/deviceId 这些字段，服务端照样
 * 回 803 并下发 cookie，但之后拿这条 cookie 去查账号只会得到
 * {"code":200,"account":null,"profile":null} —— 和“没登录”一模一样。
 * 所以每次请求都要带同一套身份，deviceId 形状照真实客户端用 MAC 地址。
 */
#define MUSIC_DEVICE_ID     "a8:9c:ed:3f:21:07"
#define MUSIC_CLIENT_APPVER "3.1.17.204416"
#define MUSIC_CLIENT_OSVER  "Microsoft-Windows-10-Professional-build-19045-64bit"

/*
 * Cookie 头缓冲上限：整条 jar 都要塞进去。
 * MUSIC_U 单个值就 500~1800 字节，旧的 480 字节上限导致登录后**根本不发
 * Cookie 头**，明文 /api/ 退路也直接返回参数非法。
 */
#define MUSIC_COOKIE_HEADER_MAX (MUSIC_HTTP_JAR_MAX + 256)

/* WEAPI 常量：RSA 加密的 encSecKey + 双层 AES-128-CBC 的 params */
#define MUSIC_WEAPI_PRESET_KEY "0CoJUm6Qyw8W8jud"
#define MUSIC_WEAPI_IV         "0102030405060708"

/* URL 编码在文件后半段定义，WEAPI 组装请求体时要用 */
static int url_encode(const char *input, char *out, size_t out_size);

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

/*
 * 临时缓冲优先放 PSRAM（登后 EAPI 的明文/密文能到 3~4 KB，别去抢内部 SRAM）。
 *
 * 注意：本 SDK 的堆失败回调是 __builtin_trap()，分配失败 = 整机静默停机。
 * 所以这里也先看余量：大块请求不够就直接返回 NULL 让调用方走错误分支，
 * 只有小块（≤ 4 KB）才允许退回内部 SRAM。
 */
#define MUSIC_ALLOC_PSRAM_RESERVE (128 * 1024)
#define MUSIC_ALLOC_INTERNAL_MAX  (4 * 1024)

static void *music_malloc(size_t size)
{
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (free_psram < size + MUSIC_ALLOC_PSRAM_RESERVE) {
        LOGE("alloc %u rejected: psram free %u", (unsigned)size, (unsigned)free_psram);
        return NULL;
    }
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

    if (ptr == NULL) {
        LOGE("psram alloc %u failed (free %u)", (unsigned)size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (size <= MUSIC_ALLOC_INTERNAL_MAX) {
            ptr = malloc(size);
        }
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

size_t music_netease_login_token_len(void)
{
    return cookie_value_len("MUSIC_U");
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
 * 生成 EAPI params 里的 header 对象开头（不含 __csrf / MUSIC_U，也不关闭花括号）：
 *   {"header":{"os":"pc",...,"requestId":"<ms>_<seq>"
 * 所有 EAPI 请求（包括申请二维码 / 轮询）都用它，保证身份一致。
 */
static int eapi_header_text(char *out, size_t out_size)
{
    static unsigned sequence;
    unsigned long long stamp = (unsigned long long)time(NULL);
    int n;

    if (stamp == 0) {
        /* SNTP 未同步时退回单调计数，反正服务端只看“有没有这个字段” */
        stamp = 1700000000ull + sequence;
    }
    n = snprintf(out, out_size,
                 "{\"header\":{\"os\":\"pc\",\"appver\":\"%s\",\"osver\":\"%s\","
                 "\"channel\":\"netease\",\"deviceId\":\"%s\",\"versioncode\":\"140\","
                 "\"mobilename\":\"\",\"buildver\":\"%llu\",\"resolution\":\"1920x1080\","
                 "\"requestId\":\"%llu%03u_%04u\"",
                 MUSIC_CLIENT_APPVER, MUSIC_CLIENT_OSVER, MUSIC_DEVICE_ID, stamp, stamp,
                 sequence % 1000u, sequence % 10000u);
    sequence++;
    if (n < 0 || (size_t)n >= out_size) {
        return MUSIC_ERR_OVERFLOW;
    }
    return MUSIC_OK;
}

/*
 * 拼 EAPI 的 params 明文 JSON：auth 为 true 时把 cookie 里的 MUSIC_U / __csrf
 * 放进 header 字段（照真实客户端的做法）。
 * 输出缓冲由调用方分配（一般直接放 PSRAM）。
 */
static int eapi_payload_build(const char *json, bool auth, char *out, size_t out_size)
{
    const size_t mu_len = auth ? cookie_value_len("MUSIC_U") : 0;
    const size_t cs_len = auth ? cookie_value_len("__csrf") : 0;
    char header[384];
    size_t inner_len = strlen(json);
    const char *inner = json;
    size_t used;
    int n;

    if (eapi_header_text(header, sizeof(header)) != MUSIC_OK) {
        return MUSIC_ERR_OVERFLOW;
    }
    n = snprintf(out, out_size, "%s", header);
    if (n < 0 || (size_t)n >= out_size) {
        return MUSIC_ERR_OVERFLOW;
    }
    used = (size_t)n;

    if (mu_len > 0 || cs_len > 0) {
        n = snprintf(out + used, out_size - used, ",\"__csrf\":\"");
        if (n < 0 || (size_t)n >= out_size - used) {
            return MUSIC_ERR_OVERFLOW;
        }
        used += (size_t)n;
        used += cookie_value_copy("__csrf", out + used, out_size - used);
        n = snprintf(out + used, out_size - used, "\",\"MUSIC_U\":\"");
        if (n < 0 || (size_t)n >= out_size - used) {
            return MUSIC_ERR_OVERFLOW;
        }
        used += (size_t)n;
        used += cookie_value_copy("MUSIC_U", out + used, out_size - used);
        n = snprintf(out + used, out_size - used, "\"");
        if (n < 0 || (size_t)n >= out_size - used) {
            return MUSIC_ERR_OVERFLOW;
        }
        used += (size_t)n;
    }

    /* 关闭 header 对象 */
    if (used + 2 > out_size) {
        return MUSIC_ERR_OVERFLOW;
    }
    out[used++] = '}';

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
    char headers[256];
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

    /* header 文本 + __csrf + 整条 MUSIC_U 都要装进去，余量给到 1024 */
    payload_cap = strlen(json) + (auth ? strlen(s_cookie) : 0) + 1024;
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

    /*
     * 登录后必须同时带 Cookie 头（整条 jar，不截断）。
     * jar 较长时走裸 socket：httpclient 的上下行头部共用 2048 字节，
     * 一条 700 字节的 MUSIC_U 加上响应头很容易超预算，整条请求会直接失败。
     */
    const size_t jar_len = strlen(s_cookie);

    if (auth && jar_len > 0) {
        char *raw_headers = music_malloc(MUSIC_COOKIE_HEADER_MAX + 256);

        if (raw_headers == NULL) {
            heap_caps_free(body);
            return MUSIC_ERR_NO_MEM;
        }
        snprintf(raw_headers, MUSIC_COOKIE_HEADER_MAX + 256,
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Referer: https://music.163.com/\r\n"
                 "Cookie: %s\r\n",
                 s_cookie);
        ret = music_http_raw_post(url, raw_headers, body, (int)strlen(body), NULL, 0, resp);
        heap_caps_free(raw_headers);
    } else {
        if (jar_len > 0 && jar_len + sizeof(MUSIC_EAPI_HEADERS) < sizeof(headers)) {
            snprintf(headers, sizeof(headers), "%s&Cookie:%s", MUSIC_EAPI_HEADERS, s_cookie);
        } else {
            snprintf(headers, sizeof(headers), "%s", MUSIC_EAPI_HEADERS);
        }
        ret = music_http_request_ex(url, true, body, (int)strlen(body), headers, NULL, 0, resp);
    }
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
    char *raw_headers;
    const size_t jar_len = strlen(s_cookie);
    int n;
    int ret;

    memset(resp, 0, sizeof(*resp));
    if (jar_len == 0) {
        return MUSIC_ERR_PARAM;
    }
    n = snprintf(url, sizeof(url), "%s%s", MUSIC_API_BASE, api_path);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return MUSIC_ERR_PARAM;
    }
    /* 整条 jar 都塞进 Cookie 头，所以固定走裸 socket（不抢 httpclient 的 2048 预算） */
    raw_headers = music_malloc(MUSIC_COOKIE_HEADER_MAX + 256);
    if (raw_headers == NULL) {
        return MUSIC_ERR_NO_MEM;
    }
    snprintf(raw_headers, MUSIC_COOKIE_HEADER_MAX + 256,
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Referer: https://music.163.com/\r\n"
             "Cookie: %s\r\n",
             s_cookie);
    ret = music_http_raw_post(url, raw_headers, form_body,
                              form_body ? (int)strlen(form_body) : 0, NULL, 0, resp);
    heap_caps_free(raw_headers);
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

/* ==================== WEAPI 加解密 ==================== */

/*
 * WEAPI 是真实客户端读「账号 / 我的歌单」用的网关：
 *   params      = urlescape(base64(AES-CBC(base64(AES-CBC(json, 固定key)), 随机key)))
 *   encSecKey   = hex(RSA_NoPadding(随机key，模幂 65537))
 * 固定 key 和 IV、RSA 模数都是客户端硬编码值。
 */
static const unsigned char s_weapi_rsa_modulus[128] = {
    0xe0, 0xb5, 0x09, 0xf6, 0x25, 0x9d, 0xf8, 0x64, 0x2d, 0xbc, 0x35, 0x66, 0x29, 0x01, 0x47,
    0x7d, 0xf2, 0x26, 0x77, 0xec, 0x15, 0x2b, 0x5f, 0xf6, 0x8a, 0xce, 0x61, 0x5b, 0xb7, 0xb7,
    0x25, 0x15, 0x2b, 0x3a, 0xb1, 0x7a, 0x87, 0x6a, 0xea, 0x8a, 0x5a, 0xa7, 0x6d, 0x2e, 0x41,
    0x76, 0x29, 0xec, 0x4e, 0xe3, 0x41, 0xf5, 0x61, 0x35, 0xfc, 0xcf, 0x69, 0x52, 0x80, 0x10,
    0x4e, 0x03, 0x12, 0xec, 0xbd, 0xa9, 0x25, 0x57, 0xc9, 0x38, 0x70, 0x11, 0x4a, 0xf6, 0xc9,
    0xd0, 0x5c, 0x4f, 0x7f, 0x0c, 0x36, 0x85, 0xb7, 0xa4, 0x6b, 0xee, 0x25, 0x59, 0x32, 0x57,
    0x5c, 0xce, 0x10, 0xb4, 0x24, 0xd8, 0x13, 0xcf, 0xe4, 0x87, 0x5d, 0x3e, 0x82, 0x04, 0x7b,
    0x97, 0xdd, 0xef, 0x52, 0x74, 0x1d, 0x54, 0x6b, 0x8e, 0x28, 0x9d, 0xc6, 0x93, 0x5b, 0x3e,
    0xce, 0x04, 0x62, 0xdb, 0x0a, 0x22, 0xb8, 0xe7,
};

/* AES-128-CBC + base64（PKCS#7 填充，IV 固定 0102030405060708） */
static int weapi_aes_b64(const char *input, size_t input_len, const char *key, char *out,
                         size_t out_size)
{
    unsigned char iv[16];
    unsigned char *plain;
    unsigned char *cipher;
    size_t padded;
    size_t encoded = 0;
    int rc;
    mbedtls_aes_context aes;

    memcpy(iv, MUSIC_WEAPI_IV, 16);
    padded = input_len + 16 - (input_len % 16);
    if (out_size < ((padded + 2) / 3) * 4 + 1) {
        return MUSIC_ERR_OVERFLOW;
    }
    plain = music_malloc(padded);
    cipher = music_malloc(padded);
    if (plain == NULL || cipher == NULL) {
        heap_caps_free(plain);
        heap_caps_free(cipher);
        return MUSIC_ERR_NO_MEM;
    }
    memcpy(plain, input, input_len);
    memset(plain + input_len, (int)(padded - input_len), padded - input_len);

    mbedtls_aes_init(&aes);
    rc = mbedtls_aes_setkey_enc(&aes, (const unsigned char *)key, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv, plain, cipher);
    }
    mbedtls_aes_free(&aes);
    if (rc == 0) {
        rc = mbedtls_base64_encode((unsigned char *)out, out_size - 1, &encoded, cipher, padded);
    }
    heap_caps_free(plain);
    heap_caps_free(cipher);
    if (rc != 0) {
        out[0] = '\0';
        return MUSIC_ERR_PARAM;
    }
    out[encoded] = '\0';
    return MUSIC_OK;
}

/* 随机 16 字符密钥（不做密码学级别要求，只要每次请求不同即可） */
static void weapi_random_key(char secret[17])
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static uint32_t seed;
    const uint32_t now = (uint32_t)time(NULL);

    seed ^= now ^ (uint32_t)(uintptr_t)secret;
    for (int i = 0; i < 16; i++) {
        /* xorshift32 */
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        secret[i] = alphabet[seed % (sizeof(alphabet) - 1)];
    }
    secret[16] = '\0';
}

/* encSecKey = hex(RSA(secret 倒序，放在 128 字节块末尾，指数 65537)) */
static int weapi_enc_sec_key(const char secret[17], char out[257])
{
    unsigned char block[128];
    unsigned char result[128];
    mbedtls_mpi n;
    mbedtls_mpi e;
    mbedtls_mpi m;
    mbedtls_mpi c;
    mbedtls_mpi rr;
    int rc;

    memset(block, 0, sizeof(block));
    for (int i = 0; i < 16; i++) {
        block[112 + i] = (unsigned char)secret[15 - i];
    }
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&m);
    mbedtls_mpi_init(&c);
    mbedtls_mpi_init(&rr);
    rc = mbedtls_mpi_read_binary(&n, s_weapi_rsa_modulus, sizeof(s_weapi_rsa_modulus));
    if (rc == 0) {
        rc = mbedtls_mpi_lset(&e, 65537);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_read_binary(&m, block, sizeof(block));
    }
    if (rc == 0) {
        /*
         * 本 SDK 在 prec_RR == NULL 时走 HSU；当前驱动对 WEAPI 的
         * 128 字节模数会直接断言。显式准备 Montgomery 缓存以走软件
         * 实现，并检查预计算错误（SDK 内部预计算遗漏了返回值检查）。
         * 固定模数占满 128 字节，R^2 = 2^2048，缓存只属于当前请求。
         */
        rc = mbedtls_mpi_lset(&rr, 1);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_shift_l(&rr, 2 * sizeof(s_weapi_rsa_modulus) * 8);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_mod_mpi(&rr, &rr, &n);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_exp_mod(&c, &m, &e, &n, &rr);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&c, result, sizeof(result));
    }
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&m);
    mbedtls_mpi_free(&c);
    mbedtls_mpi_free(&rr);
    if (rc != 0) {
        return MUSIC_ERR_PARAM;
    }
    for (size_t i = 0; i < sizeof(result); i++) {
        snprintf(out + i * 2, 3, "%02x", result[i]);
    }
    out[256] = '\0';
    return MUSIC_OK;
}

/*
 * POST 一个 WEAPI 接口；path 形如 "/nuser/account/get"（不含 /weapi 前缀）。
 * Cookie 头带整条 jar，走裸 socket（服务端不认的话至少不会因为头部预算失败）。
 */
static int weapi_post(const char *path, const char *json, music_http_resp_t *resp)
{
    char secret[17];
    char enc_key[257];
    char url[128];
    char *first = NULL;
    char *encoded = NULL;
    char *body = NULL;
    char *raw_headers = NULL;
    size_t body_cap;
    int n;
    int ret;

    memset(resp, 0, sizeof(*resp));
    if (path == NULL || json == NULL) {
        return MUSIC_ERR_PARAM;
    }
    n = snprintf(url, sizeof(url), "%s/weapi%s", MUSIC_API_BASE, path);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return MUSIC_ERR_PARAM;
    }

    /* 第一层：固定 key */
    const size_t first_cap = ((strlen(json) + 16) / 16) * 16 * 4 / 3 + 64;
    first = music_malloc(first_cap);
    encoded = music_malloc(first_cap * 2 + 64);
    body_cap = first_cap * 3 + 512;
    body = music_malloc(body_cap);
    raw_headers = music_malloc(MUSIC_COOKIE_HEADER_MAX + 256);
    if (first == NULL || encoded == NULL || body == NULL || raw_headers == NULL) {
        ret = MUSIC_ERR_NO_MEM;
        goto done;
    }

    ret = weapi_aes_b64(json, strlen(json), MUSIC_WEAPI_PRESET_KEY, first, first_cap);
    if (ret != MUSIC_OK) {
        LOGE("weapi layer1 failed: %d", ret);
        goto done;
    }
    weapi_random_key(secret);
    ret = weapi_aes_b64(first, strlen(first), secret, encoded, first_cap * 2 + 64);
    if (ret != MUSIC_OK) {
        LOGE("weapi layer2 failed: %d", ret);
        goto done;
    }
    ret = weapi_enc_sec_key(secret, enc_key);
    if (ret != MUSIC_OK) {
        LOGE("weapi rsa failed: %d", ret);
        goto done;
    }

    memcpy(body, "params=", 7);
    ret = url_encode(encoded, body + 7, body_cap - 7);
    if (ret != MUSIC_OK) {
        LOGE("weapi urlencode failed: %d", ret);
        goto done;
    }
    n = snprintf(body + strlen(body), body_cap - strlen(body), "&encSecKey=%s", enc_key);
    if (n < 0 || (size_t)n >= body_cap - strlen(body)) {
        ret = MUSIC_ERR_OVERFLOW;
        goto done;
    }

    snprintf(raw_headers, MUSIC_COOKIE_HEADER_MAX + 256,
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Referer: https://music.163.com/\r\n"
             "Origin: https://music.163.com\r\n"
             "Cookie: %s\r\n",
             s_cookie);
    ret = music_http_raw_post(url, raw_headers, body, (int)strlen(body), NULL, 0, resp);
    if (ret != 0) {
        ret = MUSIC_ERR_NET;
        goto done;
    }
    keep_snippet(resp);
    if (resp->status != 200) {
        LOGW("weapi http %d: %s", resp->status, path);
        ret = MUSIC_ERR_NET;
        goto done;
    }
    LOGI("weapi ok: %s (%u bytes)", path, (unsigned)resp->length);
    ret = MUSIC_OK;

done:
    heap_caps_free(first);
    heap_caps_free(encoded);
    heap_caps_free(body);
    heap_caps_free(raw_headers);
    return ret;
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

    ret = eapi_post_ex("/api/login/qrcode/unikey", "{\"type\":3}", true, &resp);
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
    char *payload;
    const size_t body_cap = 2048 + strlen(key) * 4;
    const size_t payload_cap = sizeof(json) + strlen(s_cookie) + 512;
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
    /*
     * 不能走 parse_eapi_root：这个接口的 code 就是 800/801/802/803。
     * params 里必须带完整客户端身份（os/appver/deviceId/...），否则服务端
     * 会下发一条“看起来正常、但用不了”的 MUSIC_U（查账号永远 profile=null）。
     */
    payload = music_malloc(payload_cap);
    if (payload == NULL) {
        heap_caps_free(body);
        return MUSIC_ERR_NO_MEM;
    }
    ret = eapi_payload_build(json, true, payload, payload_cap);
    if (ret == MUSIC_OK) {
        ret = eapi_build_body("/api/login/qrcode/client/login", payload, body, body_cap);
    }
    heap_caps_free(payload);
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
     *
     * 同时带上已有的匿名 cookie（NMTID / MUSIC_A）：真实客户端每次请求都带，
     * 新下发的登录令牌也会挂在同一条会话上。
     */
    {
        char *raw_headers = music_malloc(MUSIC_COOKIE_HEADER_MAX + 256);

        if (raw_headers == NULL) {
            heap_caps_free(body);
            return MUSIC_ERR_NO_MEM;
        }
        if (s_cookie[0] != '\0') {
            snprintf(raw_headers, MUSIC_COOKIE_HEADER_MAX + 256,
                     "Content-Type: application/x-www-form-urlencoded\r\n"
                     "Referer: https://music.163.com/\r\n"
                     "Cookie: %s\r\n",
                     s_cookie);
        } else {
            snprintf(raw_headers, MUSIC_COOKIE_HEADER_MAX + 256,
                     "Content-Type: application/x-www-form-urlencoded\r\n"
                     "Referer: https://music.163.com/\r\n");
        }
        ret = music_http_raw_post(MUSIC_API_BASE "/eapi/login/qrcode/client/login", raw_headers,
                                  body, (int)strlen(body), s_cookie, sizeof(s_cookie), &resp);
        heap_caps_free(raw_headers);
    }
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

/* 把响应前 200 字节打进日志：真机上“code 是 200 但字段全是 null”这种克最难猜 */
static void log_resp_body(const char *tag, const music_http_resp_t *resp)
{
    char snippet[201];
    size_t n;

    if (resp == NULL || resp->body == NULL || resp->length <= 0) {
        LOGI("%s: http=%d (empty body)", tag, resp != NULL ? resp->status : -1);
        return;
    }
    n = (size_t)resp->length;
    if (n > sizeof(snippet) - 1) {
        n = sizeof(snippet) - 1;
    }
    memcpy(snippet, resp->body, n);
    snippet[n] = '\0';
    LOGI("%s: http=%d len=%d body=%s", tag, resp->status, resp->length, snippet);
}

/* 解析账号响应（WEAPI / EAPI / 明文 /api/ 三种网关结构一致，都是直接 JSON） */
static void account_parse(const music_http_resp_t *resp, int64_t *uid_out, char *nickname,
                          size_t nickname_size)
{
    cJSON *root;
    const cJSON *profile;
    const cJSON *account;

    if (resp->body == NULL || resp->length <= 0) {
        return;
    }
    root = cJSON_ParseWithLength(resp->body, (size_t)resp->length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
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
}

/*
 * 取登录账号。三条路线都试，因为三个网关读登录态的位置不同：
 *   1) WEAPI  /weapi/nuser/account/get + Cookie 头（真实客户端路线）
 *   2) EAPI   /eapi/nuser/account/get  + params.header.MUSIC_U + Cookie 头
 *   3) 明文   /api/nuser/account/get   + Cookie 头
 * cookie 失效时三条都会回 code=200 但 account/profile 为 null。
 */
int music_netease_account(int64_t *uid_out, char *nickname, size_t nickname_size)
{
    char json[320];
    char csrf[MUSIC_CSRF_MAX];
    music_http_resp_t resp;
    int err;

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
    cookie_value_copy("__csrf", csrf, sizeof(csrf));

    /* 1) WEAPI */
    snprintf(json, sizeof(json), "{\"csrf_token\":\"%s\"}", csrf);
    err = weapi_post("/nuser/account/get", json, &resp);
    if (err == MUSIC_OK) {
        log_resp_body("account/weapi", &resp);
        account_parse(&resp, uid_out, nickname, nickname_size);
    } else {
        LOGW("account/weapi failed: %d(%s)", err, music_err_text(err));
    }
    music_http_resp_free(&resp);

    /* 2) EAPI（cookie 同时进 params.header 与 Cookie 头） */
    if (*uid_out <= 0) {
        err = eapi_post_ex("/api/nuser/account/get", "{}", true, &resp);
        if (err == MUSIC_OK) {
            log_resp_body("account/eapi", &resp);
            account_parse(&resp, uid_out, nickname, nickname_size);
        } else {
            LOGW("account/eapi failed: %d(%s)", err, music_err_text(err));
        }
        music_http_resp_free(&resp);
    }

    /* 3) 明文 /api/ + Cookie 头 */
    if (*uid_out <= 0) {
        err = plain_api_post("/api/nuser/account/get", "", &resp);
        if (err == MUSIC_OK) {
            log_resp_body("account/plain", &resp);
            account_parse(&resp, uid_out, nickname, nickname_size);
        } else {
            LOGW("account/plain failed: %d(%s)", err, music_err_text(err));
        }
        music_http_resp_free(&resp);
    }

    LOGI("account uid=%lld nick=%s jar=%u", (long long)*uid_out,
         (nickname != NULL) ? nickname : "", (unsigned)strlen(s_cookie));
    return (*uid_out > 0) ? MUSIC_OK : MUSIC_ERR_UNAVAILABLE;
}

/* 解析 /api/user/playlist 的响应 */
static int playlists_parse(const music_http_resp_t *resp, music_user_playlist_list_t *out)
{
    cJSON *root;
    const cJSON *list;
    const cJSON *item;

    if (resp->body == NULL || resp->length <= 0) {
        return MUSIC_ERR_JSON;
    }
    root = cJSON_ParseWithLength(resp->body, (size_t)resp->length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
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
    char json[256];
    char form[256];
    char csrf[MUSIC_CSRF_MAX];
    music_http_resp_t resp;
    int ret;

    if (uid < 0 || out == NULL) {
        return MUSIC_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));
    cookie_value_copy("__csrf", csrf, sizeof(csrf));

    snprintf(json, sizeof(json),
             "{\"uid\":%lld,\"limit\":%d,\"offset\":0,\"includeVideo\":true,"
             "\"csrf_token\":\"%s\"}",
             (long long)uid, MUSIC_USER_PLAYLIST_MAX, csrf);
    snprintf(form, sizeof(form), "uid=%lld&limit=%d&offset=0&csrf_token=%s", (long long)uid,
             MUSIC_USER_PLAYLIST_MAX, csrf);

    /* 1) WEAPI（真实客户端路线） */
    ret = weapi_post("/user/playlist", json, &resp);
    if (ret == MUSIC_OK) {
        log_resp_body("my/weapi", &resp);
        ret = playlists_parse(&resp, out);
    } else {
        LOGW("my/weapi failed: %d(%s)", ret, music_err_text(ret));
    }
    music_http_resp_free(&resp);

    /* 2) EAPI */
    if (out->count == 0) {
        ret = eapi_post_ex("/api/user/playlist", json, true, &resp);
        if (ret == MUSIC_OK) {
            log_resp_body("my/eapi", &resp);
            ret = playlists_parse(&resp, out);
        } else {
            LOGW("my/eapi failed: %d(%s)", ret, music_err_text(ret));
        }
        music_http_resp_free(&resp);
    }

    /* 3) 明文 /api/ */
    if (out->count == 0) {
        ret = plain_api_post("/api/user/playlist", form, &resp);
        if (ret == MUSIC_OK) {
            log_resp_body("my/plain", &resp);
            (void)playlists_parse(&resp, out);
        } else {
            LOGW("my/plain failed: %d(%s)", ret, music_err_text(ret));
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
