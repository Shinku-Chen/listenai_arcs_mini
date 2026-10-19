#ifndef __ARCADE_AUDIO_H__
#define __ARCADE_AUDIO_H__

#include <stdint.h>
#include <stdbool.h>

/*
 * 模拟器声音输出：binjgb 的 APU → 单声道 16bit PCM → audio0（DAC）。
 * 采样率就是 binjgb 的重采样目标，也是 DAC 的播放采样率。
 */
#define ARCADE_AUDIO_RATE   48000

/**
 * 打开 audio0 播放并起消费任务。重复调用无副作用。
 * @return 0 成功；负值表示没有音频设备 / 配置失败（此时所有接口变成空操作）
 */
int arcade_audio_init(void);

/** 关掉播放与消费任务（正常流程不需要调用，留给退出/低功耗场景） */
void arcade_audio_deinit(void);

/** 音频链路是否可用 */
bool arcade_audio_ready(void);

/**
 * 播放会话开关。进游戏置 true，回列表置 false。
 * false 时持续向 DAC 写静音块，保证喇叭不会重复播放上一块。
 */
void arcade_audio_set_active(bool active);

/** 丢掉环形缓冲里残留的样本、复位插值与 DC 阻断状态（换 ROM 时用） */
void arcade_audio_flush(void);

/**
 * 推入一帧 APU 输出。src 是 binjgb 的 u8 立体声交织采样（左、右），
 * frames 是采样帧数。内部会混成单声道、按音量缩放后写进环形缓冲。
 */
void arcade_audio_push(const uint8_t *u8_stereo, uint32_t frames);

/** 音量 0..100（同时写进 KV，下次开机沿用） */
void arcade_audio_set_volume(uint32_t vol);
uint32_t arcade_audio_volume(void);

/** 调试：采样率/块大小/水位/欠载/重采样比等一行状态 */
int arcade_audio_report(char *out, uint32_t out_len);

/** 调试：最近 ~1 秒的峰值与 RMS（没法听声音，用它看波形有没有出来） */
int arcade_audio_peak_report(char *out, uint32_t out_len);

/** 调试：清零统计计数 */
void arcade_audio_reset_stats(void);

#endif /* __ARCADE_AUDIO_H__ */
