#!/usr/bin/env python3
"""把「今天吃啥」的成品素材（arcade/assets/*.bin）转成 C 数组嵌入固件。

用法（CMake 在配置阶段调用，纯标准库，不依赖 Pillow）:
    gen_eat_what_assets.py <g1.bin> <g2.bin> <out.c> <out.h>

素材格式见 prepare_eat_what.py：每帧 = 256 色 RGB565 调色板(512B) + 索引。
分辨率不是写死在这里的：优先读同目录的 eat_what_meta.txt（prepare 脚本生成），
没有元信息时按文件长度反推（候选 240/200/160）。这样换素材尺寸只改一个地方，
不会出现「素材是 200、生成脚本还以为 160」的静默错位。
文件不存在时输出空数组并给出 warning，保证固件照样能编译。
"""

import os
import sys

PALETTE_BYTES = 256 * 2

# 反推分辨率用的候选（大的在前；240 = 满屏，200 = 源素材原生，160 = 旧版）
RES_CANDIDATES = (240, 200, 160)

# 滚动速度沿用原 GIF 的节奏：去处盘 10 fps、菜品盘 20 fps。
# 固件里滚动固定用 14 fps（两边之间的手感折中），这两个值只作为上限参考。
G1_DELAY_MS = 70
G2_DELAY_MS = 50


def read_meta(path):
    """读 prepare 脚本写的元信息；缺文件/缺字段返回 None。"""
    try:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.readlines()
    except OSError:
        return None

    meta = {}
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        try:
            meta[k.strip()] = int(v.strip())
        except ValueError:
            pass

    return meta if "res" in meta else None


def infer_res(sizes):
    """按「每份素材都是整数帧」反推分辨率。"""
    hits = []
    for res in RES_CANDIDATES:
        frame = PALETTE_BYTES + res * res
        if all(s > 0 and s % frame == 0 for s in sizes):
            hits.append((res, frame))
    return hits[0] if hits else (None, None)


def emit_array(out, name, data):
    out.append("const unsigned char %s[] __attribute__((aligned(4))) = {" % name)
    # 一行 16 字节，1.6MB 的素材就是 10 万行左右，编译能吃下（实测 ~40s）
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        out.append("    " + " ".join("0x%02x," % b for b in chunk))
    if not data:
        out.append("    0x00,")
    out.append("};")
    out.append("")
    out.append("const unsigned int %s_size = %uu;" % (name, len(data)))
    out.append("")


def load(path, frame_bytes):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        sys.stderr.write("gen_eat_what_assets: 缺少 %s，按空素材处理\n" % path)
        return b""

    if len(data) % frame_bytes != 0:
        sys.stderr.write("gen_eat_what_assets: %s 长度 %d 不是整帧（%d），忽略\n"
                         % (path, len(data), frame_bytes))
        return b""

    return data


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 1

    g1_path, g2_path, out_c, out_h = sys.argv[1:5]

    meta = read_meta(os.path.join(os.path.dirname(os.path.abspath(g1_path)),
                                  "eat_what_meta.txt"))

    if meta is not None:
        res = meta["res"]
        pal = meta.get("palette", PALETTE_BYTES)
        frame_bytes = meta.get("frame", pal + res * res)
        sys.stderr.write("gen_eat_what_assets: 元信息 RES=%d frame=%d\n"
                         % (res, frame_bytes))
    else:
        sizes = []
        for p in (g1_path, g2_path):
            try:
                sizes.append(os.path.getsize(p))
            except OSError:
                sizes.append(0)
        res, frame_bytes = infer_res(sizes)
        if res is None:
            sys.stderr.write("gen_eat_what_assets: 无法反推分辨率（%s），按 200 处理\n"
                             % sizes)
            res = 200
            frame_bytes = PALETTE_BYTES + res * res

    g1 = load(g1_path, frame_bytes)
    g2 = load(g2_path, frame_bytes)

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

/* 每帧 = {frame_bytes - res * res}B RGB565 调色板 + {res}x{res} 索引 */
#define ARCADE_EAT_RES          {res}
#define ARCADE_EAT_FRAME_BYTES  {frame_bytes}

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

    print("gen_eat_what_assets: RES=%d g1=%d 帧 g2=%d 帧 共 %d B -> %s"
          % (res, len(g1) // frame_bytes, len(g2) // frame_bytes,
             len(g1) + len(g2), out_c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
