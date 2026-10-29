#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/*
 * 从响应头里抓登录 cookie。
 *
 * httpclient 没有“取某个头”的便捷接口，只有 FindFirstHeader/GetNextHeader 这套
 * 搜索游标：先 set clue，再逐条取。同一个头可能出现多次（Set-Cookie 就是典型），
 * 所以要把所有 Set-Cookie 都翻一遍，找含 "MUSIC_U=" 的那条。
 */
static void music_http_capture_cookie(HTTPParameters *param, char *out, size_t out_size)
{
    char header[416];

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (param == NULL || param->pHTTP == 0) {
        return;
    }

    /* FindFirstHeader 只设置“搜什么”，真正的匹配要调 GetNextHeader */
    UINT32 length = sizeof(header);
    header[0] = '\0';
    if (HTTPClientFindFirstHeader(param->pHTTP, "Set-Cookie", header, &length) !=
        HTTP_CLIENT_SUCCESS) {
        return;
    }

    for (;;) {
        const char *hit = NULL;

        length = sizeof(header);
        header[0] = '\0';
        if (HTTPClientGetNextHeader(param->pHTTP, header, &length) != HTTP_CLIENT_SUCCESS) {
            break;
        }

        hit = strstr(header, "MUSIC_U=");
        if (hit != NULL) {
            size_t n = 0;

            hit += strlen("MUSIC_U=");
            while (hit[n] != '\0' && hit[n] != ';' && hit[n] != '\r' && hit[n] != '\n' &&
                   n + 1 < out_size) {
                n++;
            }
            memcpy(out, hit, n);
            out[n] = '\0';
            break;
        }
    }
    (void)HTTPClientFindCloseHeader(param->pHTTP);
}

int music_http_request_ex(const char *url, bool post, const char *body, int body_len,
                          const char *extra_headers, char *cookie_out, size_t cookie_size,
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
    music_http_capture_cookie(param, cookie_out, cookie_size);

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
