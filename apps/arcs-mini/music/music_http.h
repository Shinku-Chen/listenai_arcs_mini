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
 *
 * 另有 music_http_raw_post()：扫码登录专用，绕开 httpclient 的 2048 字节
 * 「上下行头部共享」上限（网易云的 MUSIC_U 令牌单个就可能 1~2 KB）。
 */
#ifndef __MUSIC_HTTP_H__
#define __MUSIC_HTTP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * cookie jar 建议容量。
 * MUSIC_U 是登录态令牌，实测/上游注释都表明单个值可以超过 1200 字节，
 * 加上 __csrf 等零碎项，jar 用 3 KB 才算安全（旧的 512 字节会把令牌截断）。
 */
#define MUSIC_HTTP_JAR_MAX 3072

typedef struct {
    int status;   /* HTTP 状态码；<0 表示本地失败（-1 网络/协议，-2 无内存，-3 超长） */
    int length;   /* body 字节数 */
    char *body;   /* NUL 结尾，可能为 NULL；用完调 music_http_resp_free() */
} music_http_resp_t;

/*
 * extra_headers：httpclient 的扩展头串，格式 "Name:Value&Name2:Value2"，
 * 注意分隔符是 '&' 而不是换行，且值里不能出现 '&'。可以为 NULL。
 *
 * cookie_jar / jar_size：cookie 罐子。
 *   进来时是"已有的 cookie 串"（可以是空串），出去时是合并了本次响应里
 *   所有 Set-Cookie 的结果（"A=1; B=2"）。传 NULL 表示不抓 cookie。
 *
 * 返回 0 表示 HTTP 层成功（status 可能是任何值），非 0 表示连不上/超时等。
 */
int music_http_request_ex(const char *url, bool post, const char *body, int body_len,
                          const char *extra_headers, char *cookie_jar, size_t jar_size,
                          music_http_resp_t *resp);

int music_http_request(const char *url, bool post, const char *body, int body_len,
                       const char *extra_headers, music_http_resp_t *resp);

/*
 * 裸 socket 版 POST（只支持 http://，不支持 TLS / 重定向）。
 *
 * 为什么单独做一条：httpclient 的 HTTP_CLIENT_MAX_SEND_RECV_HEADERS 是 2048，
 * 且**上行与下行头部共用**这块缓冲。网易云扫码登录成功（code=803）时把登录
 * 令牌放在 Set-Cookie 里，MUSIC_U 单个值就可能超过 1200 字节，连头部区都放
 * 不下 —— 结果是请求整体失败或者只拿到半条 cookie，表现为"手机提示登录成功，
 * 设备毫无反应"。这里用 lwip 的 BSD socket 自己收发，头部上限放到 8 KB，
 * 并把每一条 Set-Cookie 都并进 cookie_jar。
 *
 * headers：完整的请求头（每行以 "\r\n" 结尾），可以为 NULL。
 * 成功返回 0，resp->status / resp->body 照常填。
 */
int music_http_raw_post(const char *url, const char *headers, const char *body, int body_len,
                        char *cookie_jar, size_t jar_size, music_http_resp_t *resp);

void music_http_resp_free(music_http_resp_t *resp);

#endif /* __MUSIC_HTTP_H__ */
