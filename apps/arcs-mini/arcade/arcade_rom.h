#ifndef __ARCADE_ROM_H__
#define __ARCADE_ROM_H__

#include <stdint.h>
#include <stdbool.h>

int arcade_rom_scan(void);
int arcade_rom_count(void);
const char *arcade_rom_name(int index);
const char *arcade_rom_path(int index);
uint32_t arcade_rom_size(int index);
bool arcade_rom_scanned(void);
void arcade_rom_invalidate(void);

#endif /* __ARCADE_ROM_H__ */
