# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Export the 16-color preview as a flash-resident LVGL I4 image."""
from pathlib import Path

from PIL import Image


def export():
    assets = Path(__file__).resolve().parent
    with Image.open(assets / "magnifier-landscape.png") as thumbnail:
        if thumbnail.mode != "P" or thumbnail.size != (240, 135) or thumbnail.getextrema()[1] > 15:
            raise ValueError("Expected a 240x135 16-color indexed PNG")
        palette = thumbnail.getpalette()
        data = bytearray()
        for offset in range(0, 48, 3):
            r, g, b = palette[offset:offset + 3]
            data.extend((b, g, r, 255))
        pixels = thumbnail.tobytes()
        data.extend((a << 4) | b for a, b in zip(pixels[::2], pixels[1::2], strict=True))
    notice = Path(__file__).read_text(encoding="utf-8").splitlines()[:3]
    lines = [*(line.replace("# ", "// ", 1) for line in notice), "#pragma once", '#include "lvgl.h"',
             "// Generated from design/assets/magnifier-landscape.png by export_magnifier_landscape.py.",
             "static const LV_ATTRIBUTE_MEM_ALIGN uint8_t landscape_pixels[] = {"]
    for start in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{v:02x}" for v in data[start:start + 16]) + ",")
    lines.extend(["};", "static const lv_image_dsc_t magnifier_landscape = {",
                  "    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I4,",
                  "               .w = 240, .h = 135, .stride = 120},",
                  "    .data_size = sizeof(landscape_pixels), .data = landscape_pixels,", "};"])
    target = assets.parents[1] / "main/magnifier_landscape.h"
    target.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


if __name__ == "__main__":
    export()
