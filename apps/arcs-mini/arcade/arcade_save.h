/*
 * 卡带存档（cartridge RAM / .sav）落盘。
 *
 * 路径约定：<ROM 同目录>/<ROM 主文件名>.sav
 *   例：/SD:/roms/pokemon.gb  ->  /SD:/roms/pokemon.sav
 * 内置演示 ROM（虚拟路径 "@demo"）没有卡上路径，直接跳过存档。
 *
 * 写入顺序：<name>.sav.tmp -> 删旧 .sav -> rename 成 .sav。
 * 掉电最坏情况是「少存一次」，不会把上一份完好的存档写坏。
 */
#ifndef __ARCADE_SAVE_H__
#define __ARCADE_SAVE_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * 绑定当前 ROM：把已有的 .sav 读进 cart_ram。
 * @param rom_path      ROM 的完整路径（不是卡上的路径则自动关闭存档）
 * @param cart_ram      卡带 RAM 缓冲区
 * @param cart_ram_size 缓冲区大小
 * @return true 表示这个 ROM 具备存档能力
 */
bool arcade_save_attach(const char *rom_path, uint8_t *cart_ram, uint32_t cart_ram_size);

/** 解绑（不会自动落盘，调用方应先 arcade_save_flush()） */
void arcade_save_detach(void);

/** 模拟器写入 cart RAM 时调用，标记有改动 */
void arcade_save_mark_dirty(void);

/**
 * 有改动才写盘。
 * @return >0 写入字节数；0 无改动或不可存档；<0 失败（保持 dirty，下次重试）
 */
int arcade_save_flush(void);

/** 主循环调用：每 30 秒自动落盘一次（有改动才写） */
void arcade_save_tick(void);

/** 当前 .sav 路径，未绑定或不可存档返回 NULL */
const char *arcade_save_path(void);

/** 是否有未落盘的改动 */
bool arcade_save_pending(void);

#endif /* __ARCADE_SAVE_H__ */
