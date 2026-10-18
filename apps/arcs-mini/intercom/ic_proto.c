/*
 * 对讲机线协议实现：分帧 / 重组 / 文本合并（纯逻辑）
 *
 * 参考 ai-passport feature/openclaw-intercom 的 oc_proto.c，按本仓约定重写：
 *  - 没有异常兜底（固件沙箱外也没有），所有入口显式判空判越界；
 *  - 重组缓冲固定 2048，绝不按帧头声明的长度去 malloc；
 *  - 三种恢复场景都要能自愈：半截帧（正常）、丢 notify（空洞）、帧头插入 payload。
 */
#include <string.h>

#include "sysutils.h"

#include "ic_proto.h"

/* 接收状态机 */
enum {
    IC_ST_HDR = 0, /* 正在凑帧头 */
    IC_ST_BODY,    /* 正在收 payload */
};

static struct {
    const ic_proto_cb_t *cb;

    uint8_t state;
    uint8_t hdr[IC_HDR_LEN];
    uint8_t hdr_len;

    uint8_t type;
    uint8_t flags;
    uint8_t body[IC_MAX_PAYLOAD];
    uint16_t body_need;
    uint16_t body_len;

    /* 文本合并缓冲（跨帧累积到 LAST 才交付） */
    uint8_t text[IC_MAX_PAYLOAD];
    uint16_t text_len;
    uint8_t text_role;
    bool text_active;
    bool text_overflow;

    ic_proto_stat_t stat;
} s __psram_bss__;

static inline uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline bool type_valid(uint8_t t)
{
    return (t >= IC_T_AUDIO_PCM && t <= IC_T_TTS_OPUS);
}

/* 把 UTF-8 尾部的残缺字符砍掉，避免屏幕出现半个字形 */
static uint16_t utf8_trim_tail(const uint8_t *buf, uint16_t len)
{
    uint16_t i = len;
    uint16_t back = 0;

    /* 最多回退 3 个字节找起始字节（UTF-8 最长 4 字节序列） */
    while (i > 0 && back < 4) {
        i--;
        back++;
        const uint8_t b = buf[i];
        if ((b & 0x80) == 0x00) {
            /* ASCII 是完整字符，多回退的部分是废字节 */
            return (i + 1 == len) ? len : (uint16_t)(i + 1);
        }
        if ((b & 0xC0) == 0xC0) {
            uint16_t need;
            if ((b & 0xE0) == 0xC0) {
                need = 2;
            } else if ((b & 0xF0) == 0xE0) {
                need = 3;
            } else if ((b & 0xF8) == 0xF0) {
                need = 4;
            } else {
                return len;
            }
            if ((uint16_t)(i + need) <= len) {
                return len; /* 已有足够续字节，尾部完整 */
            }
            return i; /* 字符被截断，砍掉 */
        }
    }
    return len;
}

static void deliver_text(void)
{
    if (!s.text_active) {
        return;
    }
    s.text_active = false;

    uint16_t len = s.text_len;
    if (s.text_overflow) {
        len = utf8_trim_tail(s.text, len);
        s.stat.text_truncated++;
    }
    s.stat.texts++;
    if (s.cb != NULL && s.cb->on_text != NULL) {
        s.cb->on_text(s.text_role, (const char *)s.text, len, s.text_overflow);
    }
    s.text_len = 0;
    s.text_overflow = false;
}

static void text_append(const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return;
    }
    uint16_t room = (uint16_t)(IC_MAX_PAYLOAD - s.text_len);
    if (len > room) {
        len = room;
        s.text_overflow = true;
    }
    if (len > 0) {
        memcpy(s.text + s.text_len, data, len);
        s.text_len = (uint16_t)(s.text_len + len);
    } else {
        s.text_overflow = true;
    }
}

static void handle_text(void)
{
    if (s.body_need == 0) {
        return;
    }
    const uint8_t role = s.body[0];
    const uint8_t *payload = s.body + 1;
    const uint16_t plen = (uint16_t)(s.body_need - 1);

    const bool fragmented = (s.flags & (IC_FLAG_FIRST | IC_FLAG_MORE | IC_FLAG_LAST)) != 0;

    if (!fragmented) {
        /* 单帧完整消息 */
        uint16_t len = utf8_trim_tail(payload, plen);
        s.stat.texts++;
        if (s.cb != NULL && s.cb->on_text != NULL) {
            s.cb->on_text(role, (const char *)payload, len, false);
        }
        return;
    }

    if (s.flags & IC_FLAG_FIRST) {
        /* 新的逻辑消息起点：丢掉未完成的上一段（对端重开一条） */
        if (s.text_active) {
            s.stat.text_truncated++;
            deliver_text();
        }
        s.text_active = true;
        s.text_role = role;
        s.text_len = 0;
        s.text_overflow = false;
    } else if (!s.text_active) {
        /* 中间帧先到（丢了 FIRST）：当作新消息的开始，避免整条丢失 */
        s.text_active = true;
        s.text_role = role;
        s.text_len = 0;
        s.text_overflow = false;
    }

    text_append(payload, plen);

    if (s.flags & IC_FLAG_LAST) {
        deliver_text();
    }
}

static void handle_control(void)
{
    /* 控制消息是单帧 JSON；带 flags 的按单帧处理（协议没有给 JSON 定义分片） */
    char json[IC_MAX_PAYLOAD + 1];
    uint16_t len = s.body_need;
    if (len > IC_MAX_PAYLOAD) {
        len = IC_MAX_PAYLOAD;
    }
    memcpy(json, s.body, len);
    json[len] = '\0';
    s.stat.controls++;
    if (s.cb != NULL && s.cb->on_control != NULL) {
        s.cb->on_control(json, len);
    }
}

static void dispatch(void)
{
    s.stat.frames++;

    switch (s.type) {
    case IC_T_TEXT:
        handle_text();
        break;
    case IC_T_CONTROL:
        handle_control();
        break;
    default:
        /* AUDIO / TTS 本期不实现：收到就当没收到，不报错（新手机兼容旧设备） */
        break;
    }
}

/* 从候选位置重建状态机：把 hdr 里已有的字节重新喂一遍 */
static void ic_proto_feed(uint8_t b);

static void enter_header_from(const uint8_t *bytes, uint8_t n)
{
    s.state = IC_ST_HDR;
    s.hdr_len = 0;
    s.body_need = 0;
    s.body_len = 0;
    for (uint8_t i = 0; i < n; i++) {
        ic_proto_feed(bytes[i]);
    }
}

static void ic_proto_feed(uint8_t b)
{
    if (s.state == IC_ST_HDR) {
        s.hdr[s.hdr_len++] = b;

        if (s.hdr_len < 2) {
            return;
        }

        /* 第一个字节不是 magic：丢掉它，重新对齐 */
        if (s.hdr[0] != IC_MAGIC0) {
            memmove(s.hdr, s.hdr + 1, (size_t)(s.hdr_len - 1));
            s.hdr_len--;
            return;
        }
        if (s.hdr[1] != IC_MAGIC1) {
            /* 0xA5 0xA5 的情形：保留后一个 */
            if (s.hdr[1] == IC_MAGIC0) {
                s.hdr[0] = IC_MAGIC0;
                s.hdr_len = 1;
            } else {
                s.hdr_len = 0;
            }
            return;
        }
        if (s.hdr_len < IC_HDR_LEN) {
            return;
        }

        const uint8_t type = s.hdr[2];
        const uint16_t plen = rd_be16(&s.hdr[4]);

        if (!type_valid(type) || plen > IC_MAX_PAYLOAD) {
            /* 假帧头：只前进一字节继续扫描 */
            s.stat.bad_frames++;
            memmove(s.hdr, s.hdr + 1, (size_t)(s.hdr_len - 1));
            s.hdr_len--;
            return;
        }

        s.type = type;
        s.flags = s.hdr[3];
        s.body_need = plen;
        s.body_len = 0;
        s.hdr_len = 0;

        if (plen == 0) {
            dispatch();
            return;
        }
        s.state = IC_ST_BODY;
        return;
    }

    /* IC_ST_BODY */
    s.body[s.body_len++] = b;

    /*
     * 检测插进 payload 里的帧头：只有当当前 payload 尾部够 6 字节、且这 6 字节
     * 构成合法帧头时才认为命中了插入。判据刻意收窄（合法 type + 长度能装进
     * 缓冲 + 不能是"音频帧里再嵌音频帧"），否则正常数据会被误判。
     */
    if (s.body_len >= IC_HDR_LEN) {
        const uint8_t *cand = &s.body[s.body_len - IC_HDR_LEN];
        if (cand[0] == IC_MAGIC0 && cand[1] == IC_MAGIC1) {
            const uint8_t ctype = cand[2];
            const uint16_t clen = rd_be16(&cand[4]);
            const bool audio_nested = (type_valid(ctype) &&
                                       ((ctype == IC_T_AUDIO_PCM) || (ctype == IC_T_AUDIO_OPUS)) &&
                                       ((s.type == IC_T_AUDIO_PCM) || (s.type == IC_T_AUDIO_OPUS)));
            if (type_valid(ctype) && clen <= IC_MAX_PAYLOAD && !audio_nested) {
                s.stat.resyncs++;
                uint8_t carry[IC_HDR_LEN];
                memcpy(carry, cand, IC_HDR_LEN);
                enter_header_from(carry, IC_HDR_LEN);
                return;
            }
        }
    }

    if (s.body_len >= s.body_need) {
        s.state = IC_ST_HDR;
        dispatch();
    }
}

void ic_proto_rx(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        ic_proto_feed(data[i]);
    }
}

size_t ic_proto_pack(uint8_t *out, size_t cap, uint8_t type, uint8_t flags,
                     const void *payload, uint16_t plen)
{
    if (out == NULL || plen > IC_MAX_PAYLOAD) {
        return 0;
    }
    if (cap < (size_t)IC_HDR_LEN + plen) {
        return 0;
    }
    if (!type_valid(type)) {
        return 0;
    }

    out[0] = IC_MAGIC0;
    out[1] = IC_MAGIC1;
    out[2] = type;
    out[3] = flags;
    out[4] = (uint8_t)(plen >> 8);
    out[5] = (uint8_t)(plen & 0xFF);
    if (plen > 0 && payload != NULL) {
        memcpy(out + IC_HDR_LEN, payload, plen);
    }
    return (size_t)IC_HDR_LEN + plen;
}

void ic_proto_init(const ic_proto_cb_t *cb)
{
    memset(&s, 0, sizeof(s));
    s.cb = cb;
    s.state = IC_ST_HDR;
}

const ic_proto_stat_t *ic_proto_stat(void)
{
    return &s.stat;
}
