#!/usr/bin/env python3
"""把「今天吃啥」的成品素材（arcade/assets/*.bin）转成 C 数组嵌入固件。

用法（CMake 在配置阶段调用，纯标准库，不依赖 Pillow）:
    gen_eat_what_assets.py <g1.bin> <g2.bin> <out.c> <out.h>

素材格式见 prepare_eat_what.py：每帧 = 256 色 RGB565 调色板(512B) + 索引。
文件不存在时输出空数组并给出 warning，保证固件照样能编译。
"""

import os
import sys

RES = 160
PALETTE_BYTES = 256 * 2
FRAME_BYTES = PALETTE_BYTES + RES * RES

# 滚动速度沿用原 GIF 的节奏：去处盘 10 fps、菜品盘 20 fps。
# 固件里滚动固定用 14 fps（两边之间的手感折中），这两个值只作为上限参考。
G1_DELAY_MS = 70
G2_DELAY_MS = 50


def emit_array(out, name, data):
    out.append("const unsigned char %s[] __attribute__((aligned(4))) = {" % name)
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        out.append("    " + " ".join("0x%02x," % b for b in chunk))
    if not data:
        out.append("    0x00,")
    out.append("};")
    out.append("")
    out.append("const unsigned int %s_size = %uu;" % (name, len(data)))
    out.append("")


def load(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        sys.stderr.write("gen_eat_what_assets: 缺少 %s，按空素材处理\n" % path)
        return b""

    if len(data) % FRAME_BYTES != 0:
        sys.stderr.write("gen_eat_what_assets: %s 长度不是整帧（%d），忽略\n"
                         % (path, len(data)))
        return b""

    return data


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 1

    g1_path, g2_path, out_c, out_h = sys.argv[1:5]

    g1 = load(g1_path)
    g2 = load(g2_path)

    out = [
        "/* 由 arcade/tools/gen_eat_what_assets.py 生成，不要手改 */",
        '#include "arcade_eat_assets.h"',
        "",
    ]
    emit_array(out, "arcade_eat_g1", g1)
    emit_array(out, "arcade_eat_g2", g2)

    with open(out_c, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    hdr = f"""/* 由 arcade/tools/gen_eat_what_assets.py 生成，不要手改 */
#ifndef __ARCADE_EAT_ASSETS_H__
#define __ARCADE_EAT_ASSETS_H__

/* 每帧 = {PALETTE_BYTES}B RGB565 调色板 + {RES}x{RES} 索引 */
#define ARCADE_EAT_RES          {RES}
#define ARCADE_EAT_FRAME_BYTES  {FRAME_BYTES}

/* 去处盘（今天午餐吃什麼呢?） / 菜品盘（今天吃什么选择器） */
#define ARCADE_EAT_G1_DELAY_MS  {G1_DELAY_MS}
#define ARCADE_EAT_G2_DELAY_MS  {G2_DELAY_MS}

extern const unsigned char arcade_eat_g1[];
extern const unsigned int arcade_eat_g1_size;
extern const unsigned char arcade_eat_g2[];
extern const unsigned int arcade_eat_g2_size;

#endif /* __ARCADE_EAT_ASSETS_H__ */
"""
    with open(out_h, "w", encoding="utf-8") as f:
        f.write(hdr)

    print("gen_eat_what_assets: g1=%d 帧 g2=%d 帧 共 %d B -> %s"
          % (len(g1) // FRAME_BYTES, len(g2) // FRAME_BYTES, len(g1) + len(g2),
             out_c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
