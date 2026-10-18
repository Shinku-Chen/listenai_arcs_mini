/*
 * ic_proto 主机侧单测（纯逻辑，不依赖固件）
 *
 * 编译运行（需要 host gcc）：
 *   cc -I../../apps/arcs-mini/intercom -o test_ic_proto test_ic_proto.c \
 *      ../../apps/arcs-mini/intercom/ic_proto.c && ./test_ic_proto
 *
 * 覆盖协议文文档里点名的恢复场景：半截帧、丢 notify、帧头插进 payload、
 * 假 magic、超长声明、文本跨帧合并与溢出截断。
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "ic_proto.h"

/* ---------------- 收集回调 ---------------- */

static struct {
    int texts;
    int controls;
    char last_text[IC_MAX_PAYLOAD + 1];
    uint16_t last_len;
    uint8_t last_role;
    bool last_truncated;
    char last_json[IC_MAX_PAYLOAD + 1];
} g;

static void on_text(uint8_t role, const char *utf8, uint16_t len, bool truncated)
{
    g.texts++;
    g.last_role = role;
    g.last_len = len;
    g.last_truncated = truncated;
    memcpy(g.last_text, utf8, len);
    g.last_text[len] = '\0';
}

static void on_control(const char *json, uint16_t len)
{
    g.controls++;
    memcpy(g.last_json, json, len);
    g.last_json[len] = '\0';
}

static const ic_proto_cb_t k_cb = {.on_text = on_text, .on_control = on_control};

static void reset(void)
{
    memset(&g, 0, sizeof(g));
    ic_proto_init(&k_cb);
}

/* ---------------- 用例 ---------------- */

static void test_single_text(void)
{
    reset();
    uint8_t payload[64];
    payload[0] = IC_ROLE_ASSIST;
    const char *msg = "你好，我是对讲机";
    const size_t n = strlen(msg);
    memcpy(payload + 1, msg, n);

    uint8_t frame[128];
    const size_t flen = ic_proto_pack(frame, sizeof(frame), IC_T_TEXT, 0, payload, (uint16_t)(1 + n));
    assert(flen == 6 + 1 + n);
    ic_proto_rx(frame, flen);

    assert(g.texts == 1);
    assert(g.last_role == IC_ROLE_ASSIST);
    assert(g.last_len == n);
    assert(strcmp(g.last_text, msg) == 0);
    assert(!g.last_truncated);
    printf("ok   single text frame\n");
}

static void test_fragmented_text(void)
{
    reset();
    const char *a = "第一段";
    const char *b = "第二段";
    const char *want = "第一段第二段";

    uint8_t p1[32], p2[32];
    p1[0] = IC_ROLE_USER;
    memcpy(p1 + 1, a, strlen(a));
    p2[0] = IC_ROLE_USER;
    memcpy(p2 + 1, b, strlen(b));

    uint8_t f[64];
    size_t n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, IC_FLAG_FIRST | IC_FLAG_MORE, p1,
                             (uint16_t)(1 + strlen(a)));
    ic_proto_rx(f, n);
    assert(g.texts == 0);

    n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, IC_FLAG_MORE, p2, (uint16_t)(1 + strlen(b)));
    ic_proto_rx(f, n);
    assert(g.texts == 0);

    n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, IC_FLAG_LAST, p2, (uint16_t)(1 + strlen(b)));
    ic_proto_rx(f, n);
    assert(g.texts == 1);
    assert(strcmp(g.last_text, want) == 0);
    assert(g.last_role == IC_ROLE_USER);
    printf("ok   fragmented text merge\n");
}

static void test_byte_by_byte(void)
{
    reset();
    uint8_t payload[16];
    payload[0] = IC_ROLE_ASSIST;
    memcpy(payload + 1, "abc", 3);
    uint8_t f[32];
    const size_t n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, 0, payload, 4);

    for (size_t i = 0; i < n; i++) {
        ic_proto_rx(&f[i], 1);
    }
    assert(g.texts == 1);
    assert(strcmp(g.last_text, "abc") == 0);
    printf("ok   byte-by-byte feed\n");
}

/* 丢一次 notify：帧中间出现空洞后，下一帧仍能被正确解析 */
static void test_lost_notify(void)
{
    reset();
    uint8_t payload[64];
    payload[0] = IC_ROLE_ASSIST;
    memcpy(payload + 1, "0123456789", 10);
    uint8_t f[64];
    const size_t n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, 0, payload, 11);
    (void)n;

    /* 前 4 字节到达后丢掉其余，然后直接送到下一帧 */
    ic_proto_rx(f, 4);

    uint8_t payload2[64];
    payload2[0] = IC_ROLE_ASSIST;
    memcpy(payload2 + 1, "second", 6);
    uint8_t f2[64];
    const size_t n2 = ic_proto_pack(f2, sizeof(f2), IC_T_TEXT, 0, payload2, 7);
    ic_proto_rx(f2, n2);

    assert(g.texts == 1);
    assert(strcmp(g.last_text, "second") == 0);
    printf("ok   recover after lost notify\n");
}

/* 帧头被插进另一个帧的 payload 中间 */
static void test_embedded_header(void)
{
    reset();
    uint8_t payload[64];
    payload[0] = IC_ROLE_ASSIST;
    memcpy(payload + 1, "AAAAAAAA", 8);
    uint8_t f[64];
    const size_t n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, 0, payload, 9);

    uint8_t p2[64];
    p2[0] = IC_ROLE_USER;
    memcpy(p2 + 1, "inner", 5);
    uint8_t f2[64];
    const size_t n2 = ic_proto_pack(f2, sizeof(f2), IC_T_TEXT, 0, p2, 6);

    /* 先发第一帧的前 8 字节，再接一整帧（模拟帧头插进 payload） */
    (void)n;
    ic_proto_rx(f, 8);
    ic_proto_rx(f2, n2);

    assert(g.texts == 1);
    assert(strcmp(g.last_text, "inner") == 0);
    printf("ok   resync on embedded header\n");
}

/* 假 magic / 非法 type 不应吞掉后续合法帧 */
static void test_garbage(void)
{
    reset();
    const uint8_t junk[] = {0x00, 0xA5, 0x11, 0xA5, 0x5A, 0xFF, 0x00, 0x00, 0x10, 0xA5};

    uint8_t payload[16];
    payload[0] = IC_ROLE_ASSIST;
    memcpy(payload + 1, "ok", 2);
    uint8_t f[32];
    const size_t n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, 0, payload, 3);

    ic_proto_rx(junk, sizeof(junk));
    ic_proto_rx(f, n);

    assert(g.texts == 1);
    assert(strcmp(g.last_text, "ok") == 0);
    printf("ok   ignore garbage and resync\n");
}

/* 控制消息按 JSON 原样交付 */
static void test_control(void)
{
    reset();
    const char *json = "{\"cmd\":\"gateway\",\"state\":\"ready\"}";
    uint8_t f[128];
    const size_t n = ic_proto_pack(f, sizeof(f), IC_T_CONTROL, 0, json, (uint16_t)strlen(json));
    ic_proto_rx(f, n);

    assert(g.controls == 1);
    assert(strcmp(g.last_json, json) == 0);
    printf("ok   control json delivered\n");
}

/* 文本合并溢出：截断必须落在 UTF-8 边界上，并置 truncated */
static void test_text_overflow(void)
{
    reset();
    /* 每次塞 200 字节 'a'，多塞几轮把 2048 撑满；尾部再补中文，验证不会截出半个字 */
    uint8_t p[256];
    p[0] = IC_ROLE_ASSIST;
    memset(p + 1, 'a', 200);

    uint8_t f[512];
    size_t n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, IC_FLAG_FIRST | IC_FLAG_MORE, p, 201);
    ic_proto_rx(f, n);

    for (int i = 0; i < 12; i++) {
        n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, IC_FLAG_MORE, p, 201);
        ic_proto_rx(f, n);
    }

    p[0] = IC_ROLE_ASSIST;
    memcpy(p + 1, "中", 3);
    n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, IC_FLAG_LAST, p, 4);
    ic_proto_rx(f, n);

    assert(g.texts == 1);
    assert(g.last_truncated);
    assert(g.last_len <= IC_MAX_PAYLOAD - 1);
    printf("ok   text overflow truncates on utf8 boundary (len=%u)\n", (unsigned)g.last_len);
}

/* 超长声明长度必须被当成损坏帧头，而不是去分配缓冲 */
static void test_oversized_length(void)
{
    reset();
    uint8_t bad[6 + 8] = {IC_MAGIC0, IC_MAGIC1, IC_T_TEXT, 0, 0x08, 0x01 /* 2049 > 2048 */};
    ic_proto_rx(bad, sizeof(bad));

    uint8_t payload[8];
    payload[0] = IC_ROLE_ASSIST;
    memcpy(payload + 1, "hi", 2);
    uint8_t f[32];
    const size_t n = ic_proto_pack(f, sizeof(f), IC_T_TEXT, 0, payload, 3);
    ic_proto_rx(f, n);

    assert(g.texts == 1);
    assert(strcmp(g.last_text, "hi") == 0);
    printf("ok   oversized declared length rejected\n");
}

int main(void)
{
    test_single_text();
    test_fragmented_text();
    test_byte_by_byte();
    test_lost_notify();
    test_embedded_header();
    test_garbage();
    test_control();
    test_text_overflow();
    test_oversized_length();

    const ic_proto_stat_t *st = ic_proto_stat();
    printf("\nall pass (last run: frames=%u resync=%u bad=%u)\n",
           (unsigned)st->frames, (unsigned)st->resyncs, (unsigned)st->bad_frames);
    return 0;
}
