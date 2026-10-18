/*
 * 对讲机线协议：分帧 / 重组 / 文本合并（纯逻辑，无平台依赖）
 *
 * 契约见 docs/development/intercom-wire-protocol.md。本文件刻意不碰 BLE、不碰 LVGL：
 * 收到的字节流进来，回调与文本出去。这样协议层可以在主机上单独测试。
 *
 * 帧格式：
 *   [0] 0xA5  [1] 0x5A  [2] TYPE  [3] FLAGS  [4..5] LEN(大端)  [6..] payload
 *
 * 两字节 magic 不是装饰：PCM 采样字节大量落在 0x01..0x04，只有 1 字节 type 时丢一次
 * notify 就会把音频字节永久当成帧头。双字节 magic 让误对齐概率降到 1/65536。
 */
#ifndef __IC_PROTO_H__
#define __IC_PROTO_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* hello 里的 proto 就是本文件的版本号；不兼容改动必须提升它 */
#define IC_PROTO_VERSION 1

#define IC_MAGIC0 0xA5
#define IC_MAGIC1 0x5A
#define IC_HDR_LEN 6
#define IC_MAX_PAYLOAD 2048
#define IC_MAX_FRAME (IC_HDR_LEN + IC_MAX_PAYLOAD)

/* 帧类型 */
enum ic_proto_type {
    IC_T_AUDIO_PCM = 0x01,  /* 设备 -> 手机：[SEQ] + int16 LE 单声道 PCM（本期未实现） */
    IC_T_TEXT = 0x02,       /* 手机 -> 设备：[role] + UTF-8 */
    IC_T_CONTROL = 0x03,    /* 双向：UTF-8 JSON 命令 */
    IC_T_EVENT = 0x04,      /* 设备 -> 手机：UTF-8 JSON 事件 */
    IC_T_AUDIO_OPUS = 0x05, /* 设备 -> 手机（本期未实现） */
    IC_T_TTS_OPUS = 0x06,   /* 手机 -> 设备（本期不声明能力，不会到达） */
};

/* 多帧标志位 */
#define IC_FLAG_MORE 0x01
#define IC_FLAG_FIRST 0x02
#define IC_FLAG_LAST 0x04

/* 文本角色（TEXT payload 首字节） */
#define IC_ROLE_USER 'U'
#define IC_ROLE_ASSIST 'A'
#define IC_ROLE_SYSTEM 'R'

typedef struct {
    /* 一条完整逻辑文本（已跨帧合并）。truncated = 合并缓冲溢出被截断 */
    void (*on_text)(uint8_t role, const char *utf8, uint16_t len, bool truncated);
    /* 一条完整 CONTROL JSON（不保证以 '\0' 结尾，长度以 len 为准） */
    void (*on_control)(const char *json, uint16_t len);
} ic_proto_cb_t;

/* 统计量：丢帧/重同步次数必须能读出来，否则诊断只能靠猜 */
typedef struct {
    uint32_t frames;     /* 收下的完整帧 */
    uint32_t texts;      /* 交付的完整文本 */
    uint32_t controls;   /* 交付的完整控制消息 */
    uint32_t resyncs;    /* 因插入帧头 / magic 错位而重建的次数 */
    uint32_t bad_frames; /* type 非法或长度越界的帧头 */
    uint32_t text_truncated; /* 文本合并溢出次数 */
} ic_proto_stat_t;

void ic_proto_init(const ic_proto_cb_t *cb);

/* 把收到的字节喂进来（任意切分；半截帧、丢 notify、插入帧头都能恢复） */
void ic_proto_rx(const uint8_t *data, size_t len);

/* 组装一帧到 out，返回总长度；cap 不足或 len 越界返回 0 */
size_t ic_proto_pack(uint8_t *out, size_t cap, uint8_t type, uint8_t flags,
                     const void *payload, uint16_t plen);

const ic_proto_stat_t *ic_proto_stat(void);

#endif /* __IC_PROTO_H__ */
