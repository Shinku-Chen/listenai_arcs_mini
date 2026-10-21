#ifndef __ARCADE_ROM_H__
#define __ARCADE_ROM_H__

#include <stdint.h>
#include <stdbool.h>

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

bool arcade_rom_scanned(void);
void arcade_rom_invalidate(void);

#endif /* __ARCADE_ROM_H__ */
