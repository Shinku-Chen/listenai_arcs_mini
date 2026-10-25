#!/usr/bin/env python3
"""把「今天吃啥」原始素材（200x200 I8 帧序列）转成 arcade 用的紧凑格式。

原始素材来自 ai-passport 仓库的 tools/generate_eat_what_assets.py：
    main/eat_what_g1.bin / eat_what_g2.bin
每帧布局 = 256 色 * 4 字节(lv_color32_t，内存序 B,G,R,A) + 200*200 像素索引。
两份加起来 41 帧、1.68 MB。

本脚本只做两件事：**调色板 888 → 565**、**删掉索引里的 3 个空字节/色**。
像素索引一个字节都不动，既不重采样也不重新量化 —— 上一版为了把 200x200 塞进
160x160 用了 LANCZOS 重采样 + MEDIANCUT 重新量化，那是「图片模糊」的元凶之一
（另一个元凶是固件里又把 160x160 做 1.5 倍最近邻放大到 240x240）。

新格式（分辨率与源素材一致，1:1 显示，固件里不缩放）：
    每帧 = 256 色 * 2 字节(RGB565 小端) + 200*200 像素索引 = 40512 字节
    20 帧 + 21 帧 = 1,660,992 字节（约 1.58 MB）

屏是 240x240，200x200 按 1:1 居中显示（四周各 20 px 黑边）。清晰度优先：
只要做了非整数倍缩放（1.5x / 1.2x），字就会发虚。

用法（**只在改素材时手动跑**，产物已提交到 arcade/assets/）：
    python prepare_eat_what.py <src_g1.bin> <src_g2.bin> <out_dir>

纯标准库，不依赖 Pillow。生成后 CMake 会用 tools/gen_eat_what_assets.py 把
.bin 转成 C 数组嵌进固件。
"""
import os
import struct
import sys

# ---- 源素材格式（ai-passport 侧生成，固定值） ----
SRC_RES = 200
SRC_COLORS = 256
SRC_PALETTE = SRC_COLORS * 4               # lv_color32_t = B,G,R,A
SRC_FRAME = SRC_PALETTE + SRC_RES * SRC_RES

# ---- 目标格式（arcade 侧） ----
RES = SRC_RES                              # 1:1，不缩放
COLORS = 256
PALETTE_BYTES = COLORS * 2                 # RGB565
FRAME_BYTES = PALETTE_BYTES + RES * RES

META_NAME = "eat_what_meta.txt"


def rgb565(r, g, b):
    """RGB888 → RGB565。**四舍五入**而不是截断：截断会让蓝色/红色系统性偏暗
    （最大误差 8/255），四舍五入的误差只有 4/255，肉眼无差。
    面板解码 5/6 位时按位复制扩展，所以这里按比例缩放就够了。"""
    return (((r * 31 + 127) // 255) << 11) | (((g * 63 + 127) // 255) << 5) | \
           ((b * 31 + 127) // 255)


def convert(src_path, out_path):
    with open(src_path, "rb") as f:
        data = f.read()

    if not data or len(data) % SRC_FRAME != 0:
        raise RuntimeError("%s 长度 %d 不是 %d 的整数倍"
                           % (src_path, len(data), SRC_FRAME))

    frames = len(data) // SRC_FRAME
    out = bytearray()

    for i in range(frames):
        base = i * SRC_FRAME
        raw_pal = data[base:base + SRC_PALETTE]
        idx = data[base + SRC_PALETTE:base + SRC_FRAME]

        # 原始调色板是 lv_color32_t 内存序 B,G,R,A
        for c in range(COLORS):
            r = raw_pal[4 * c + 2]
            g = raw_pal[4 * c + 1]
            b = raw_pal[4 * c + 0]
            out += struct.pack("<H", rgb565(r, g, b))

        # 索引原样照抄（这就是「不做任何重采样」的意思）
        out += idx

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

    # 给 gen_eat_what_assets.py 读的元信息：分辨率只在这里定义一次，
    # 免得「素材改了、生成脚本的 RES 忘了改」这种静默错位。
    with open(os.path.join(out_dir, META_NAME), "w", encoding="utf-8") as f:
        f.write("# 由 tools/prepare_eat_what.py 生成，勿手改\n")
        f.write("res=%d\n" % RES)
        f.write("palette=%d\n" % PALETTE_BYTES)
        f.write("frame=%d\n" % FRAME_BYTES)
        f.write("frames_g1=%d\n" % n1)
        f.write("frames_g2=%d\n" % n2)

    total = (n1 + n2) * FRAME_BYTES
    print("RES=%d 每帧=%d B（调色板 %d + 索引 %d）"
          % (RES, FRAME_BYTES, PALETTE_BYTES, RES * RES))
    print("G1 %d帧 -> eat_what_g1.bin %d B" % (n1, n1 * FRAME_BYTES))
    print("G2 %d帧 -> eat_what_g2.bin %d B" % (n2, n2 * FRAME_BYTES))
    print("合计 %d 帧 / %d B / %.3f MB" % (n1 + n2, total, total / 1024.0 / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
