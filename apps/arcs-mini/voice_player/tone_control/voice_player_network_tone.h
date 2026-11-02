#ifndef VOICE_PLAYER_NETWORK_TONE_H
#define VOICE_PLAYER_NETWORK_TONE_H

/**
 * @file voice_player_network_tone.h
 * @brief 网络、云端和配网状态对应的运行时提示音策略。
 */

/**
 * @brief 初始化网络提示音策略、定时器和消息订阅。
 * @return 0 初始化成功，其他值表示失败。
 */
int voice_player_network_tone_init(void);

/* 请求播放配网提示音；UI 未就绪时会延迟到 UI 就绪后播放。 */
void voice_player_network_tone_request_netcfg_tone(void);

/* UI 完成初始化后再允许播放启动阶段的网络提示音。 */
void voice_player_network_tone_ui_ready(void);

#endif /* VOICE_PLAYER_NETWORK_TONE_H */
