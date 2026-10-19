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

#endif /* __ARCADE_GB_H__ */
