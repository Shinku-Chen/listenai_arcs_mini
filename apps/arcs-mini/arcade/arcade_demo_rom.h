#ifndef __ARCADE_DEMO_ROM_H__
#define __ARCADE_DEMO_ROM_H__

/*
 * 内置演示 ROM（构建时由 arcade/tools/gen_demo_rom.py 生成）。
 *
 * 没有第三方测试 ROM 时这两个符号是空数组 / 0，此时 TF 卡里必须存在 ROM。
 */
extern const unsigned char arcade_demo_rom[];
extern const unsigned int arcade_demo_rom_size;

#endif /* __ARCADE_DEMO_ROM_H__ */
