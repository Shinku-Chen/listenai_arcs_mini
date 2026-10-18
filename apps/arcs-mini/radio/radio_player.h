/*
 * 电台播放器：独占一个 app_player 实例（焦点通道 "radio"），自带状态机任务。
 *
 * 为什么不复用 music 播放器：
 *  - music 播放器上挂了云端歌单的回调（播完切下一首、暂停要发消息），
 *    直播流没有"播完"，两套语义混在一起会互相打架；
 *  - 直播流的 pause/resume 不可靠（服务端可能已断连），所以电台的
 *    「暂停」在播放器层等同于 stop，恢复时重新拉流。
 *
 * 任务模型（对齐 leo-radio 的 player_task）：
 *   should_play = 用户想播 && 有网 && 处于音频焦点前台 && 有 URL
 *   每秒轮询，状态不对就拉起/停止，连续失败若干次后交给上层换台。
 */
#ifndef __RADIO_PLAYER_H__
#define __RADIO_PLAYER_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RADIO_PB_STOPPED = 0, /* 空闲 / 用户已停 */
    RADIO_PB_CONNECTING,  /* 正在拉流 */
    RADIO_PB_PLAYING,     /* 出声中 */
    RADIO_PB_ERROR,       /* 刚失败，正在等待重试 */
} radio_pb_state_t;

/* 连续失败达到上限时回调（在播放器任务上下文里执行），上层据此自动换台。 */
typedef void (*radio_audio_fail_cb_t)(void);

int radio_audio_init(void);
void radio_audio_set_fail_cb(radio_audio_fail_cb_t cb);

/* 网络可用性（由上层轮询 sys_network_get_status 后同步过来） */
void radio_audio_set_network(bool ok);

/* 开始/切换到某个流地址；重复调用同一个地址不会重新拉流。 */
void radio_audio_play(const char *url);
/* 停止播放并记住「用户想停」 */
void radio_audio_stop(void);

bool radio_audio_wanted(void);
radio_pb_state_t radio_audio_get_state(void);
/* 调试用：which=0 取「已播放的代」、which=1 取「当前 URL 的代」 */
uint32_t radio_audio_debug_generation(uint8_t which);

#endif /* __RADIO_PLAYER_H__ */
