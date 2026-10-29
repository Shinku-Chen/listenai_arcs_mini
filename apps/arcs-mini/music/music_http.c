#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define TAG "music.http"
#include "lisa_log.h"

#include "esp_heap_caps.h"
#include "HTTPCUsr_api.h"

#include "music_http.h"

/*
 * 响应缓冲上限。
 *
 * 实测：搜索约 9.8 KB；歌单详情（/api/v6/playlist/detail）不同榜单差异很大：
 *   新歌榜 20.6 KB、飙升榜 21.7 KB、热歌榜 38.1 KB（200 个 trackIds + 歌单元信息）。
 * 原 24 KB 会把热歌榜截断 → JSON 解不动（表现为“歌单加载失败”），所以抬到 72 KB。
 * 这份缓冲是临时的（请求结束即 free），PSRAM 堆还有 3 MB 量级，不值得再省。
 */
#define MUSIC_HTTP_RESP_CAP (72 * 1024)
/* 无 Content-Length（分块）时的读块大小 */
#define MUSIC_HTTP_READ_CHUNK 2048
/* HTTPCUsr 的 nTimeout 单位是秒 */
#define MUSIC_HTTP_TIMEOUT_S 15

/*
 * 单条响应头的读取缓冲。
 * Set-Cookie 一行就可能 1~2 KB（MUSIC_U 令牌很长），旧代码只给 416 字节，
 * SDK 会返回 HTTP_CLIENT_ERROR_NO_MEMORY，然后被当成"没有更多头"静默丢弃。
 */
#define MUSIC_HTTP_HEADER_LINE_MAX 4096

/*
 * 裸 socket 版的收发缓冲（头部 + body 共用，扫码登录响应只有几十字节 body，
 * 头部里塞着 1~2 KB 的 Set-Cookie，8 KB 足够）。
 */
#define MUSIC_RAW_RESP_MAX  (8 * 1024)
#define MUSIC_RAW_TIMEOUT_S 10

/*
 * 调用方没给扩展头时必须回退到这一串，不能返回 NULL。
 *
 * httpclient 在 HTTPClientSendRequest() 里对 GetHeader() 的返回值只判 NULL：
 *     pHttpHeaders = GetHeader();
 *     if (!pHttpHeaders) { nRetCode = -1; break; }
 * 也就是说返回 NULL 会让整个请求在「解析完 URL、还没做 DNS」时立刻失败，
 * 且失败码就是 -1（与 connect 失败、超时无法区分）。
 * 这是个隐藏地雷：EAPI 的 POST 一直带着 Content-Type/Referer，所以从来没踩到；
 * 只有不带头部的 GET（/api/search/get、/api/song/lyric）会瞬间失败。
 */
#define MUSIC_HTTP_DEFAULT_HEADERS "Accept:*/*&Referer:https://music.163.com/"

/*
 * HTTPC 的 GetHeader 回调没有 user 参数，只能返回全局。
 * 本模块所有请求都在同一个 music 工作线程里串行执行，所以单指针够用。
 */
static const char *s_headers;

static void *music_http_get_header(void)
{
    if (s_headers != NULL && s_headers[0] != '\0') {
        return (void *)s_headers;
    }
    return (void *)MUSIC_HTTP_DEFAULT_HEADERS;
}

/* 响应缓冲放 PSRAM：SRAM 只剩十几 KB，这里要 24 KB */
static char *music_http_alloc(size_t size)
{
    char *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

    if (buffer == NULL) {
        buffer = malloc(size);
    }
    return buffer;
}

void music_http_resp_free(music_http_resp_t *resp)
{
    if (resp == NULL) {
        return;
    }
    if (resp->body != NULL) {
        heap_caps_free(resp->body);
    }
    memset(resp, 0, sizeof(*resp));
}

/* ==================== cookie 罐子 ==================== */

/*
 * 把一个 "NAME=VALUE" 项并进 jar（jar 格式："A=1; B=2"）。
 *
 * 同名的旧项会被替换（服务端重新下发 MUSIC_U 时不能让旧的留着）。
 * 值里出现 '&' 直接丢弃这一项：httpclient 的扩展头用 '&' 分隔多组头，
 * 带进去会把 Cookie 头截断。
 */
static void cookie_jar_merge(char *jar, size_t jar_size, const char *cookie, size_t cookie_len)
{
    const char *pair = cookie;
    const char *end;
    const char *eq;
    size_t key_len;
    size_t pair_len;
    char *next;
    size_t used = 0;
    const char *p;

    if (jar == NULL || jar_size == 0 || cookie == NULL) {
        return;
    }
    /* 只取到第一个 ';' 为止（后面是 Path/HttpOnly/Expires 等属性） */
    end = pair;
    while ((size_t)(end - cookie) < cookie_len && *end != '\0' && *end != ';' && *end != '\r' &&
           *end != '\n') {
        end++;
    }
    while (end > pair && end[-1] == ' ') {
        end--;
    }
    pair_len = (size_t)(end - pair);
    eq = memchr(pair, '=', pair_len);
    if (eq == NULL || eq == pair) {
        return;
    }
    key_len = (size_t)(eq - pair);

    for (size_t i = 0; i < pair_len; i++) {
        const unsigned char ch = (unsigned char)pair[i];

        if (ch <= 0x20 || ch >= 0x7f || ch == '&') {
            LOGW("cookie '%.*s' dropped (bad byte 0x%02x)", (int)(key_len), pair,
                 (unsigned)ch);
            return;
        }
    }

    next = music_http_alloc(jar_size);
    if (next == NULL) {
        return;
    }
    next[0] = '\0';

    /* 先把不重名的旧项搬过去 */
    p = jar;
    while (*p != '\0') {
        const char *item_end = strchr(p, ';');
        size_t item_len = (item_end != NULL) ? (size_t)(item_end - p) : strlen(p);

        while (item_len > 0 && *p == ' ') {
            p++;
            item_len--;
        }
        while (item_len > 0 && p[item_len - 1] == ' ') {
            item_len--;
        }
        if (item_len > 0 &&
            !(item_len > key_len && p[key_len] == '=' && memcmp(p, pair, key_len) == 0)) {
            if (used + item_len + 3 < jar_size) {
                if (used > 0) {
                    memcpy(next + used, "; ", 2);
                    used += 2;
                }
                memcpy(next + used, p, item_len);
                used += item_len;
                next[used] = '\0';
            }
        }
        if (item_end == NULL) {
            break;
        }
        p = item_end + 1;
    }

    if (used + pair_len + 3 < jar_size) {
        if (used > 0) {
            memcpy(next + used, "; ", 2);
            used += 2;
        }
        memcpy(next + used, pair, pair_len);
        used += pair_len;
        next[used] = '\0';
        memcpy(jar, next, used + 1);
    } else {
        LOGW("cookie jar full (%u bytes), '%.*s' dropped", (unsigned)jar_size, (int)key_len,
             pair);
    }
    heap_caps_free(next);
}

/*
 * 从 httpclient 的响应头里抓 cookie。
 *
 * httpclient 没有“取某个头”的便捷接口，只有 FindFirstHeader/GetNextHeader 这套
 * 搜索游标：先 set clue，再逐条取。同一个头可能出现多次（Set-Cookie 就是典型），
 * 所以要把所有 Set-Cookie 都翻一遍并逐条并进 jar。
 */
static void music_http_capture_cookie(HTTPParameters *param, char *jar, size_t jar_size)
{
    char *line;
    UINT32 length = sizeof(line);
    int seen = 0;

    if (jar == NULL || jar_size == 0) {
        return;
    }
    if (param == NULL || param->pHTTP == 0) {
        return;
    }

    line = music_http_alloc(MUSIC_HTTP_HEADER_LINE_MAX);
    if (line == NULL) {
        LOGW("no memory for header capture");
        return;
    }

    /* FindFirstHeader 只设置“搜什么”，真正的匹配要调 GetNextHeader */
    line[0] = '\0';
    length = MUSIC_HTTP_HEADER_LINE_MAX;
    if (HTTPClientFindFirstHeader(param->pHTTP, "Set-Cookie", line, &length) !=
        HTTP_CLIENT_SUCCESS) {
        heap_caps_free(line);
        return;
    }

    for (;;) {
        UINT32 got = MUSIC_HTTP_HEADER_LINE_MAX;
        const UINT32 rc = HTTPClientGetNextHeader(param->pHTTP, line, &got);

        if (rc == HTTP_CLIENT_SUCCESS) {
            char *colon = strchr(line, ':');

            if (colon != NULL) {
                cookie_jar_merge(jar, jar_size, colon + 1, strlen(colon + 1));
                seen++;
            }
            continue;
        }
        if (rc != HTTP_CLIENT_ERROR_HEADER_NOT_FOUND) {
            LOGW("header search stop: rc=%d need=%u", (int)rc, (unsigned)got);
        }
        break;
    }
    (void)HTTPClientFindCloseHeader(param->pHTTP);
    if (seen > 0) {
        LOGI("cookie jar %u bytes (%d Set-Cookie)", (unsigned)strlen(jar), seen);
    }
    heap_caps_free(line);
}

int music_http_request_ex(const char *url, bool post, const char *body, int body_len,
                          const char *extra_headers, char *cookie_jar, size_t jar_size,
                          music_http_resp_t *resp)
{
    if (url == NULL || resp == NULL) {
        return -3;
    }
    memset(resp, 0, sizeof(*resp));

    const size_t url_len = strlen(url);
    if (url_len == 0 || url_len >= HTTP_CLIENT_MAX_URL_LENGTH) {
        LOGW("url too long: %u", (unsigned)url_len);
        return -3;
    }

    HTTPParameters *param = calloc(1, sizeof(HTTPParameters));
    if (param == NULL) {
        return -2;
    }

    memset(param->Uri, 0, sizeof(param->Uri));
    memcpy(param->Uri, url, url_len);
    param->HttpVerb = post ? VerbPost : VerbGet;
    param->nTimeout = MUSIC_HTTP_TIMEOUT_S;
    param->pData = (void *)body;
    param->pLength = (body_len > 0) ? (UINT32)body_len : 0;

    s_headers = extra_headers;

    LOGI("%s %s", post ? "POST" : "GET", url);

    int ret = HTTPC_open(param);
    if (ret != 0) {
        LOGE("open failed(%d): %s", ret, url);
        s_headers = NULL;
        free(param);
        return -1;
    }

    ret = HTTPC_request(param, music_http_get_header);
    if (ret != 0) {
        LOGE("request failed(%d): %s", ret, url);
        HTTPC_close(param);
        s_headers = NULL;
        free(param);
        return -1;
    }

    HTTP_CLIENT info;
    memset(&info, 0, sizeof(info));
    ret = HTTPC_get_request_info(param, &info);
    if (ret != 0) {
        LOGE("get info failed(%d)", ret);
        HTTPC_close(param);
        s_headers = NULL;
        free(param);
        return -1;
    }
    resp->status = (int)info.HTTPStatusCode;
    LOGI("http=%d body=%u", resp->status, (unsigned)info.TotalResponseBodyLength);

    /* 登录态 cookie 必须在 HTTPC_close 之前取（响应头只活在 session 里） */
    music_http_capture_cookie(param, cookie_jar, jar_size);

    char *buffer = music_http_alloc(MUSIC_HTTP_RESP_CAP + 1);
    if (buffer == NULL) {
        LOGE("no memory for response (%d bytes)", MUSIC_HTTP_RESP_CAP);
        HTTPC_close(param);
        s_headers = NULL;
        free(param);
        return -2;
    }

    size_t used = 0;
    if (info.TotalResponseBodyLength > 0) {
        size_t total = info.TotalResponseBodyLength;
        if (total > MUSIC_HTTP_RESP_CAP) {
            LOGW("response %u > cap %u, truncating", (unsigned)total,
                 (unsigned)MUSIC_HTTP_RESP_CAP);
            total = MUSIC_HTTP_RESP_CAP;
        }
        while (used < total) {
            UINT32 received = 0;
            const UINT32 want = (UINT32)((total - used) > MUSIC_HTTP_READ_CHUNK
                                             ? MUSIC_HTTP_READ_CHUNK
                                             : (total - used));
            if (HTTPC_read(param, buffer + used, want, (void *)&received) != 0) {
                if (received > 0) {
                    used += received;
                }
                break;
            }
            if (received == 0) {
                break;
            }
            used += received;
        }
    } else {
        /* 没有 Content-Length（服务端分块）：一直读到对端关闭或超过上限 */
        for (;;) {
            UINT32 received = 0;
            const UINT32 want = (UINT32)((MUSIC_HTTP_RESP_CAP - used) > MUSIC_HTTP_READ_CHUNK
                                             ? MUSIC_HTTP_READ_CHUNK
                                             : (MUSIC_HTTP_RESP_CAP - used));
            if (want == 0) {
                LOGW("response hit cap %u", (unsigned)MUSIC_HTTP_RESP_CAP);
                break;
            }
            if (HTTPC_read(param, buffer + used, want, (void *)&received) != 0) {
                if (received > 0) {
                    used += received;
                }
                break;
            }
            if (received == 0) {
                break;
            }
            used += received;
        }
    }

    HTTPC_close(param);
    s_headers = NULL;
    free(param);

    buffer[used] = '\0';
    resp->body = buffer;
    resp->length = (int)used;
    return 0;
}

int music_http_request(const char *url, bool post, const char *body, int body_len,
                       const char *extra_headers, music_http_resp_t *resp)
{
    return music_http_request_ex(url, post, body, body_len, extra_headers, NULL, 0, resp);
}

/* ==================== 裸 socket 版（扫码登录用） ==================== */

static int raw_send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        const int n = (int)send(fd, data + sent, length - sent, 0);

        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* 头部里找一个字段名（大小写不敏感），返回值的起点 */
static const char *raw_header_value(const char *head, size_t head_len, const char *name)
{
    const size_t name_len = strlen(name);
    const char *p = head;

    while ((size_t)(p - head) < head_len) {
        const char *eol = strstr(p, "\r\n");

        if (eol == NULL || (size_t)(eol - head) > head_len) {
            break;
        }
        if ((size_t)(eol - p) > name_len && strncasecmp(p, name, name_len) == 0 &&
            p[name_len] == ':') {
            const char *value = p + name_len + 1;

            while (*value == ' ') {
                value++;
            }
            return value;
        }
        p = eol + 2;
    }
    return NULL;
}

/* 分块传输解码（就地压紧），返回 body 长度 */
static size_t raw_dechunk(char *body, size_t length)
{
    size_t in = 0;
    size_t out = 0;

    while (in < length) {
        char *end = NULL;
        unsigned long chunk = strtoul(body + in, &end, 16);

        if (end == NULL || end == body + in) {
            break;
        }
        while (*end == '\r' || *end == '\n') {
            end++;
        }
        in = (size_t)(end - body);
        if (chunk == 0 || in + chunk > length) {
            break;
        }
        if (out != in) {
            memmove(body + out, body + in, chunk);
        }
        out += chunk;
        in += chunk;
        while (in < length && (body[in] == '\r' || body[in] == '\n')) {
            in++;
        }
    }
    return out;
}

int music_http_raw_post(const char *url, const char *headers, const char *body, int body_len,
                        char *cookie_jar, size_t jar_size, music_http_resp_t *resp)
{
    char host[96];
    char port[8] = "80";
    const char *path;
    const char *host_start;
    const char *slash;
    const char *colon;
    struct addrinfo hints;
    struct addrinfo *resolved = NULL;
    struct timeval tv;
    char *request = NULL;
    char *buffer = NULL;
    size_t request_len = 0;
    size_t used = 0;
    int fd = -1;
    int rc = -1;

    if (url == NULL || resp == NULL || body_len < 0) {
        return -3;
    }
    memset(resp, 0, sizeof(*resp));

    if (strncmp(url, "http://", 7) != 0) {
        LOGE("raw http only, got %s", url);
        return -3;
    }
    host_start = url + 7;
    slash = strchr(host_start, '/');
    if (slash == NULL) {
        return -3;
    }
    colon = memchr(host_start, ':', (size_t)(slash - host_start));
    const size_t host_len = (size_t)((colon != NULL ? colon : slash) - host_start);

    if (host_len == 0 || host_len >= sizeof(host)) {
        return -3;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    if (colon != NULL) {
        const size_t port_len = (size_t)(slash - colon - 1);

        if (port_len > 0 && port_len < sizeof(port)) {
            memcpy(port, colon + 1, port_len);
            port[port_len] = '\0';
        }
    }
    path = slash;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &resolved) != 0 || resolved == NULL) {
        LOGE("dns failed: %s", host);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket failed");
        freeaddrinfo(resolved);
        return -1;
    }
    tv.tv_sec = MUSIC_RAW_TIMEOUT_S;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, resolved->ai_addr, (socklen_t)resolved->ai_addrlen) != 0) {
        LOGE("connect failed: %s:%s", host, port);
        freeaddrinfo(resolved);
        close(fd);
        return -1;
    }
    freeaddrinfo(resolved);

    request = music_http_alloc(strlen(path) + strlen(host) + (headers ? strlen(headers) : 0) +
                               (size_t)body_len + 320);
    if (request == NULL) {
        close(fd);
        return -2;
    }
    const int head_len =
        snprintf(request, strlen(path) + strlen(host) + (headers ? strlen(headers) : 0) + 320,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "%s"
                 "Accept: */*\r\n"
                 "User-Agent: NeteaseMusic/8.9.70 (Android)\r\n"
                 "Connection: close\r\n"
                 "Content-Length: %d\r\n\r\n",
                 path, host, (headers != NULL) ? headers : "", body_len);
    if (head_len <= 0) {
        heap_caps_free(request);
        close(fd);
        return -3;
    }
    request_len = (size_t)head_len;
    if (body_len > 0 && body != NULL) {
        memcpy(request + request_len, body, (size_t)body_len);
        request_len += (size_t)body_len;
    }

    LOGI("raw POST %s (%u bytes)", url, (unsigned)request_len);
    if (raw_send_all(fd, request, request_len) != 0) {
        LOGE("send failed: %s", url);
        heap_caps_free(request);
        close(fd);
        return -1;
    }
    heap_caps_free(request);

    buffer = music_http_alloc(MUSIC_RAW_RESP_MAX + 1);
    if (buffer == NULL) {
        close(fd);
        return -2;
    }
    for (;;) {
        if (used >= MUSIC_RAW_RESP_MAX) {
            LOGW("raw response hit cap %u", (unsigned)MUSIC_RAW_RESP_MAX);
            break;
        }
        const int got = (int)recv(fd, buffer + used, MUSIC_RAW_RESP_MAX - used, 0);

        if (got <= 0) {
            break;
        }
        used += (size_t)got;
        buffer[used] = '\0';

        /* 头部到齐 + body 收满就提前退出（不必等对端关连接） */
        const char *head_end = strstr(buffer, "\r\n\r\n");
        if (head_end != NULL) {
            const size_t body_off = (size_t)(head_end - buffer) + 4;
            const char *len_field = raw_header_value(buffer, body_off, "Content-Length");

            if (len_field != NULL) {
                const size_t want = (size_t)strtoul(len_field, NULL, 10);

                if (used - body_off >= want) {
                    break;
                }
            }
        }
    }
    close(fd);
    buffer[used] = '\0';

    const char *head_end = strstr(buffer, "\r\n\r\n");
    if (head_end == NULL) {
        LOGE("no header terminator (got %u bytes): %.60s", (unsigned)used, buffer);
        heap_caps_free(buffer);
        return -1;
    }
    resp->status = (buffer[0] == 'H') ? atoi(buffer + 9) : 0;

    const size_t body_off = (size_t)(head_end - buffer) + 4;
    const bool chunked = (raw_header_value(buffer, body_off, "Transfer-Encoding") != NULL);
    const char *len_field = raw_header_value(buffer, body_off, "Content-Length");
    const size_t content_len = (len_field != NULL) ? (size_t)strtoul(len_field, NULL, 10) : 0;

    /* 抓所有 Set-Cookie（一行一条，逐条并进 jar） */
    {
        /* line 指向“上一行的结尾 CRLF”，所以头行从 line + 2 开始 */
        const char *line = strstr(buffer, "\r\n");
        int seen = 0;

        if (cookie_jar != NULL && jar_size > 0) {
            while (line != NULL && line < head_end) {
                const char *field = line + 2;
                const char *eol = strstr(field, "\r\n");

                if (eol == NULL || eol > head_end) {
                    break;
                }
                if ((size_t)(eol - field) >= 11 && strncasecmp(field, "Set-Cookie:", 11) == 0) {
                    const char *value = field + 11;

                    while (value < eol && *value == ' ') {
                        value++;
                    }
                    cookie_jar_merge(cookie_jar, jar_size, value, (size_t)(eol - value));
                    seen++;
                }
                line = eol;
            }
        }
        LOGI("http=%d cookie jar %u bytes (%d Set-Cookie)", resp->status,
             (unsigned)(cookie_jar != NULL ? strlen(cookie_jar) : 0), seen);
    }

    /* body：先按 Content-Length 截断，搬到缓冲头部，再按需解 chunked */
    {
        size_t body_size = used - body_off;

        if (body_size > 0) {
            memmove(buffer, buffer + body_off, body_size);
            buffer[body_size] = '\0';
            if (chunked) {
                body_size = raw_dechunk(buffer, body_size);
            } else if (content_len > 0 && content_len < body_size) {
                body_size = content_len;
            }
            buffer[body_size] = '\0';
        } else {
            body_size = 0;
            buffer[0] = '\0';
        }
        resp->body = buffer;
        resp->length = (int)body_size;
        rc = 0;
    }

    return rc;
}
