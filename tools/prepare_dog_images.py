#!/usr/bin/env python3
"""生成大狗图片的 LVGL ARGB8888 C 数组。

将 tmp 目录下的 1024x1024 网页原图缩放为屏幕尺寸 (360x360)，
并输出为 LVGL 可直接渲染的原始像素 C 数组，避免运行时 PNG 解码的
长耗时 (4MB 解码约 5 秒，会阻塞 LVGL 任务并触发 task watchdog)。

用法:
    prepare_dog_images.py --out-c <path.c> --out-h <path.h>
"""

import argparse
import os
import sys

from PIL import Image

SOURCES = ("dagou_close_mouth", "dagou_open_mouth")
SIZE = 360          # 目标边长（屏幕 360x360）
PER_ROW = 8         # C 数组每行元素个数

VAR_ART = ("dog_close_art", "dog_open_art")


def argb_uint32_array(img):
    """把 RGBA 图像转成 LVGL ARGB8888 (0xAARRGGBB) uint32 序列。

    LVGL ARGB8888 在内存中按 little-endian 排列为 B,G,R,A，
    这里用 Pillow raw 编码直接取 BGRA 字节序，再拼成 32 位整型。
    """
    im = img.convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    bgra = im.tobytes("raw", "BGRA")  # 每像素 4 字节: B,G,R,A
    vals = []
    for i in range(0, len(bgra), 4):
        b, g, r, a = bgra[i], bgra[i + 1], bgra[i + 2], bgra[i + 3]
        vals.append((a << 24) | (r << 16) | (g << 8) | b)
    return vals


def format_uint32_array(name, vals):
    lines = [f"const uint32_t {name}[] = {{"]
    for i in range(0, len(vals), PER_ROW):
        chunk = vals[i:i + PER_ROW]
        lines.append("    " + ", ".join("0x%08x" % v for v in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src-dir", required=True, help="源图片所在目录")
    parser.add_argument("--out-c", required=True)
    parser.add_argument("--out-h", required=True)
    args = parser.parse_args()

    out_dir = os.path.dirname(args.out_c)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    arrays = []
    for var, src in zip(VAR_ART, SOURCES):
        path = os.path.join(args.src_dir, src + ".png")
        if not os.path.isfile(path):
            print(f"ERROR: 找不到图片 {path}", file=sys.stderr)
            sys.exit(1)
        arrays.append(format_uint32_array(var, argb_uint32_array(Image.open(path))))

    with open(args.out_h, "w") as f:
        f.write("#pragma once\n\n")
        f.write('#include "lvgl.h"\n\n')
        f.write("/* 自动生成: 大狗图片 ARGB8888 原始像素 (360x360), 由 prepare_dog_images.py 生成 */\n")
        for var in VAR_ART:
            f.write(f"extern const uint32_t {var}[];\n")
        f.write("\n")
        f.write("extern const lv_image_dsc_t dog_close;\n")
        f.write("extern const lv_image_dsc_t dog_open;\n")

    with open(args.out_c, "w") as f:
        f.write('#include "dog_images.h"\n\n')
        f.write("/* 自动生成: 大狗图片 ARGB8888 原始像素 (360x360), 由 prepare_dog_images.py 生成 */\n")
        f.write("\n".join(arrays) + "\n\n")
        for var, dsc in zip(VAR_ART, ("dog_close", "dog_open")):
            f.write(
                f"const lv_image_dsc_t {dsc} = {{\n"
                f"    .header = {{\n"
                f"        .magic  = LV_IMAGE_HEADER_MAGIC,\n"
                f"        .cf     = LV_COLOR_FORMAT_ARGB8888,\n"
                f"        .w      = {SIZE},\n"
                f"        .h      = {SIZE},\n"
                f"        .stride = {SIZE * 4},\n"
                f"    }},\n"
                f"    .data_size = {SIZE * SIZE * 4},\n"
                f"    .data      = (const uint8_t *){var},\n"
                f"}};\n\n"
            )

    print(f"生成完成: {args.out_c}, {args.out_h}")


if __name__ == "__main__":
    main()
