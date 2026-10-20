/*
 * 帧缓冲像素格式开关（模拟器输出 → 送屏模块）。
 *
 * 1 = RGB565（u16，默认）
 *     binjgb 的 FrameBuffer 直接存 565，每帧 PPU 只写 46KB、blit 只读 46KB。
 *     CP 核的 D-Cache 只有 16KB，帧缓冲的一半访存就是一半的 cache miss，
 *     而且 blit 不再需要做 32→16 位转换（1x 时整行 memcpy 就够）。
 * 0 = RGBA（u32，binjgb 原版行为）
 *     色差/错色排查时用来对照：换回 32 位后 blit 负责转 565。
 *
 * 这个头被 binjgb 的 emulator.h 和送屏模块 arcade_video.h 同时包含，
 * 保证「生产者」和「消费者」永远看到同一个值。
 */
#ifndef __ARCADE_FB_H__
#define __ARCADE_FB_H__

#ifndef ARCADE_FB_RGB565
#define ARCADE_FB_RGB565 1
#endif

#endif /* __ARCADE_FB_H__ */
