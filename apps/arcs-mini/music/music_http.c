#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "music.http"
#include "lisa_log.h"

#include "esp_heap_caps.h"
#include "HTTPCUsr_api.h"

#include "music_http.h"

/* 响应缓冲上限：搜索接口实测 9.8 KB，歌单详情会更大，24 KB 足够里程碑 1 */
#define MUSIC_HTTP_RESP_CAP (24 * 1024)
/* 无 Content-Length（分块）时的读块大小 */
#define MUSIC_HTTP_READ_CHUNK 2048
/* HTTPCUsr 的 nTimeout 单位是秒 */
#define MUSIC_HTTP_TIMEOUT_S 15

/*
 * HTTPC 的 GetHeader 回调没有 user 参数，只能返回全局。
 * 本模块所有请求都在同一个 music 工作线程里串行执行，所以单指针够用。
 */
static const char *s_headers;

static void *music_http_get_header(void)
{
    return (void *)s_headers;
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

int music_http_request(const char *url, bool post, const char *body, int body_len,
                       const char *extra_headers, music_http_resp_t *resp)
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
