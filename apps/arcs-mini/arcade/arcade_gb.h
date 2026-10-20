#ifndef __ARCADE_GB_H__
#define __ARCADE_GB_H__

#include <stdint.h>
#include <stdbool.h>

int arcade_gb_start(const char *path);
void arcade_gb_stop(void);
bool arcade_gb_running(void);
void arcade_gb_run_frame(void);
void arcade_gb_set_input(uint32_t mask);

/**
 * 调试：对已载入的 ROM 算一遍 FNV-1a（走和模拟器完全相同的读路径），
 * 用来确认整份载入 / 银行缓存两条路径读出来的数据是对的。
 * 没跑起来时返回 0。
 */
uint32_t arcade_gb_rom_checksum(void);

/**
 * 调试：格式化 GB 卡带头（0x0134 起的标题 + 卡带类型 + ROM 大小码）。
 * @return 写入的字符数，<0 失败
 */
int arcade_gb_rom_header(char *out, uint32_t out_len);

/**
 * 调试：把 CPU / LCD 控制器的关键寄存器打成一行。
 * 连续调两次对比 PC 就能看出 CPU 到底在不在跑。
 * @return 写入的字符数，<0 没有运行中的 ROM
 */
int arcade_gb_machine_report(char *out, uint32_t out_len);

/**
 * 调试：按 CPU 视角读内存并格式化成十六进制（走 __gb_read，所以能看
 * ROM / VRAM / WRAM / IO 任意区域）。
 * @return 写入的字符数，<0 没有运行中的 ROM
 */
int arcade_gb_peek(char *out, uint32_t out_len, uint32_t addr, uint32_t len);

/**
 * 调试：帧边界 PC / SP 直方图（按出现次数降序）。
 * PC 集中在极少数地址 = CPU 卡在死循环；分布分散 = CPU 在正常跑。
 * @return 写入的字符数，<0 没有运行中的 ROM
 */
int arcade_gb_hist_report(char *out, uint32_t out_len);

/**
 * 调试：开/关 APU（对比开时与关时的 emu 耗时，就能量出声音占了 simulate
 * 里多少时间）。
 * @return 0 成功
 */
int arcade_gb_set_audio_enabled(bool on);

/**
 * 调试：开/关 PPU 像素渲染（BG / window / OBJ）。
 * 关掉以后游戏逻辑照跑、画面不动，用来量「画像素」本身占了多少 emu 时间。
 * @return 0 成功
 */
int arcade_gb_set_render_enabled(bool on);

/** 清零帧耗时统计 */
void arcade_gb_reset_perf(void);

#endif /* __ARCADE_GB_H__ */
