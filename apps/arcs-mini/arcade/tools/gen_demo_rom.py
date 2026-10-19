#!/usr/bin/env python3
"""把 GB ROM 转成 C 数组，用于内置演示 ROM。

用法:
    gen_demo_rom.py <rom.bin> <out.c> [symbol]

没有 rom 时输出一个空数组，保证固件照样能编译。
"""

import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    rom_path = sys.argv[1]
    out_path = sys.argv[2]
    symbol = sys.argv[3] if len(sys.argv) > 3 else "arcade_demo_rom"

    try:
        with open(rom_path, "rb") as f:
            data = f.read()
    except OSError:
        data = b""

    lines = [
        "/* 由 arcade/tools/gen_demo_rom.py 生成，不要手改 */",
        '#include "arcade_demo_rom.h"',
        "",
        "const unsigned char %s[] = {" % symbol,
    ]

    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        lines.append("    " + " ".join("0x%02x," % b for b in chunk))

    if not data:
        lines.append("    0x00,")

    lines.append("};")
    lines.append("")
    lines.append("const unsigned int %s_size = %uu;" % (symbol, len(data)))
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("gen_demo_rom: %s -> %s (%d bytes)" % (rom_path, out_path, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
