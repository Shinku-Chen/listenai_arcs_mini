#!/usr/bin/env python3
"""把「今天吃啥」原始素材（200x200 I8 帧序列）裁剪成 arcade 用的紧凑格式。

原始素材来自 ai-passport 仓库的 tools/generate_eat_what_assets.py：
    main/eat_what_g1.bin / eat_what_g2.bin
每帧布局 = 256 色 * 4 字节(lv_color32_t，内存序 B,G,R,A) + 200*200 像素索引。
两份加起来 41 帧、1.68 MB，直接塞进 4 MB 的 app 分区太奢侈。

本脚本把它压成 arcade 的格式（分辨率 160x160、调色板改 RGB565）：
    每帧 = 256 色 * 2 字节(RGB565 小端) + 160*160 像素索引 = 26112 字节
    20 帧 + 21 帧 = 1,070,592 字节（约 1.02 MB）

160x160 在固件里按 1.5 倍最近邻放大到 240x240（和模拟器的缩放比一致）。

用法（**只在改素材时手动跑**，产物已提交到 arcade/assets/）：
    python prepare_eat_what.py <src_g1.bin> <src_g2.bin> <out_dir>

依赖 Pillow。生成后 CMake 会用 tools/gen_eat_what_assets.py 把 .bin 转成
C 数组嵌进固件。
"""
import os
import struct
import sys

SRC_RES = 200
SRC_PALETTE = 256 * 4
SRC_FRAME = SRC_PALETTE + SRC_RES * SRC_RES

RES = 160
COLORS = 256
PALETTE_BYTES = COLORS * 2            # RGB565
FRAME_BYTES = PALETTE_BYTES + RES * RES


def rgb565(r, g, b):
    return ((r * 31 // 255) << 11) | ((g * 63 // 255) << 5) | (b * 31 // 255)


def convert(src_path, out_path):
    from PIL import Image

    with open(src_path, "rb") as f:
        data = f.read()

    if len(data) % SRC_FRAME != 0:
        raise RuntimeError("%s 长度不是整帧" % src_path)

    frames = len(data) // SRC_FRAME
    out = bytearray()

    for i in range(frames):
        base = i * SRC_FRAME
        raw_pal = data[base:base + SRC_PALETTE]
        # 原始调色板是 lv_color32_t 内存序 B,G,R,A
        pal = [(raw_pal[4 * c + 2], raw_pal[4 * c + 1], raw_pal[4 * c])
               for c in range(COLORS)]
        idx = data[base + SRC_PALETTE:base + SRC_FRAME]

        flat = bytearray(SRC_RES * SRC_RES * 3)
        for p, v in enumerate(idx):
            r, g, b = pal[v]
            flat[3 * p] = r
            flat[3 * p + 1] = g
            flat[3 * p + 2] = b

        im = Image.frombytes("RGB", (SRC_RES, SRC_RES), bytes(flat))
        im = im.resize((RES, RES), Image.LANCZOS)
        q = im.quantize(colors=COLORS, method=Image.MEDIANCUT)

        qpal = q.getpalette() or []
        ncol = len(qpal) // 3
        for c in range(COLORS):
            if c < ncol:
                r, g, b = qpal[3 * c], qpal[3 * c + 1], qpal[3 * c + 2]
            else:
                r = g = b = 0
            out += struct.pack("<H", rgb565(r, g, b))

        pixels = q.tobytes()
        if len(pixels) != RES * RES:
            raise RuntimeError("索引长度异常 %d" % len(pixels))
        out += pixels

    with open(out_path, "wb") as f:
        f.write(out)

    return frames


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1

    src_g1, src_g2, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out_dir, exist_ok=True)

    n1 = convert(src_g1, os.path.join(out_dir, "eat_what_g1.bin"))
    n2 = convert(src_g2, os.path.join(out_dir, "eat_what_g2.bin"))

    total = (n1 + n2) * FRAME_BYTES
    print("RES=%d 每帧=%d B" % (RES, FRAME_BYTES))
    print("G1 %d帧 -> eat_what_g1.bin %d B" % (n1, n1 * FRAME_BYTES))
    print("G2 %d帧 -> eat_what_g2.bin %d B" % (n2, n2 * FRAME_BYTES))
    print("合计 %d 帧 / %.3f MB" % (n1 + n2, total / 1024.0 / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
