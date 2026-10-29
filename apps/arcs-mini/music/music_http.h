/*
 * 网易云协议用的极简 HTTP 客户端（阻塞、整包读 body）。
 *
 * 为什么不直接复用 lisa_http：
 *  1. lisa_http_t::body 是 1024 字节的定长数组（BODY_MAX_LENGTH），EAPI 请求体
 *     一旦超过 1023 字节就直接 init 失败；
 *  2. lisa_http 的 headers 参数被当成 HTTP_CLIENT_GET_HEADER 函数指针传给
 *     HTTPC_request，传字符串会直接跳飞，所以没法加自定义请求头。
 *
 * 而网易云 EAPI 网关**必须**收到 Content-Type: application/x-www-form-urlencoded，
 * 否则返回 200 + 空 body（PC 上用裸 socket 复现过：不带该头 bodylen=0，
 * 带上就正常）。所以这里直接压在 HTTPCUsr_api 上做一层，请求头走
 * HTTPClientSetHeaderExtention 的 "Name:Value&Name2:Value2" 约定。
 */
#ifndef __MUSIC_HTTP_H__
#define __MUSIC_HTTP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int status;   /* HTTP 状态码；<0 表示本地失败（-1 网络/协议，-2 无内存，-3 超长） */
    int length;   /* body 字节数 */
    char *body;   /* NUL 结尾，可能为 NULL；用完调 music_http_resp_free() */
} music_http_resp_t;

/*
 * extra_headers：httpclient 的扩展头串，格式 "Name:Value&Name2:Value2"，
 * 注意分隔符是 '&' 而不是换行，且值里不能出现 '&'。可以为 NULL。
 * 返回 0 表示 HTTP 层成功（status 可能是任何值），非 0 表示连不上/超时等。
 */
int music_http_request(const char *url, bool post, const char *body, int body_len,
                       const char *extra_headers, music_http_resp_t *resp);

void music_http_resp_free(music_http_resp_t *resp);

#endif /* __MUSIC_HTTP_H__ */
