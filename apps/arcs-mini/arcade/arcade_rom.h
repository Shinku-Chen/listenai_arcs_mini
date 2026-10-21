#ifndef __ARCADE_ROM_H__
#define __ARCADE_ROM_H__

#include <stdint.h>
#include <stdbool.h>

/* 内置小游戏编号（arcade_rom_builtin_id 的返回值） */
#define ARCADE_BUILTIN_JUMP 0

int arcade_rom_scan(void);
int arcade_rom_count(void);
const char *arcade_rom_name(int index);
const char *arcade_rom_path(int index);
uint32_t arcade_rom_size(int index);

/**
 * 这一项是不是「内置条目」（不占用 TF 卡，由固件自己实现）。
 * 内置条目固定排在列表最前面，所以不用插卡也能玩。
 */
bool arcade_rom_is_builtin(int index);

/**
 * 内置条目的编号（ARCADE_BUILTIN_JUMP），非内置返回 -1。
 * 上层据此决定拉起哪个小游戏。
 */
int arcade_rom_builtin_id(int index);

bool arcade_rom_scanned(void);
void arcade_rom_invalidate(void);

#endif /* __ARCADE_ROM_H__ */
