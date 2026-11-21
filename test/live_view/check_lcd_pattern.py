# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check the public 1024x600 PNG produced by test/hil/ui_snapshot.py from a native ui snap 1."""
import argparse
import hashlib
from pathlib import Path

from PIL import Image, ImageDraw


def rgb888(word):
    return ((word >> 11) * 255 + 15) // 31, (((word >> 5) & 63) * 255 + 31) // 63, ((word & 31) * 255 + 15) // 31


def expected_pixel(x, y):
    if y in (0, 511):
        return (255, 0, 0) if y == 0 else (0, 255, 0)
    if x in (0, 511):
        return (0, 0, 255) if x == 0 else (255, 255, 0)
    red, green, blue = (x + 3*y) & 31, (x ^ y) & 63, (5*x + y) & 31
    if (x + y) % 2:
        red, green, blue = 31 - red, 63 - green, 31 - blue
    return rgb888(red << 11 | green << 5 | blue)


# Masks of fully white glyph pixels from independently rendered LVGL default labels.
# Each reference used a blank 1024x600 screen with just the three literal strings.
# Bounding boxes and text shape check the controls remain legible and in their header slots.
LABELS = (
    ("CAPTURE OFF   |   Test pattern", (10, 1, 525, 30), (7, 7, 222, 17),
     "ad02e5a3923dc54a788e0eccc99a3b89264e22fee7b3a5855c864f2bb72771fe"),
    ("Stop", (795, 1, 891, 30), (32, 10, 63, 20),
     "affca65884ebecdf583cccc1d5594bc9b5f29de8ac9f406b1b1fd3669687cafb"),
    ("Home", (909, 1, 1011, 30), (31, 12, 72, 20),
     "e118e029f980ea75e7dd50b92c9cf986e92474f9d6774431fb96536a0ae56081"),
)


def verify(screen):
    if screen.size != (1024, 600):
        raise ValueError(f"expected native 1024x600, received {screen.size}")
    pixels = screen.load()
    for y in range(-1, 513):
        for x in range(-1, 513):
            outside = x in (-1, 512) or y in (-1, 512)
            expected = (0, 0, 0) if outside else expected_pixel(x, y)
            point = 24 + x, 60 + y
            if pixels[point] != expected:
                raise ValueError(f"pixel {point} contains {pixels[point]}, expected {expected}")
    for name, region, bounds, digest in LABELS:
        image = screen.crop(region)
        mask = Image.new("1", image.size)
        mask.putdata([image.getpixel((x, y)) == (255, 255, 255)
                      for y in range(image.height) for x in range(image.width)])
        if mask.getbbox() != bounds or hashlib.sha256(mask.crop(bounds).tobytes()).hexdigest() != digest:
            raise ValueError(f"unexpected raster or placement for {name!r}")


def check_faults(screen):
    for name, region, operation in (
        ("shifted viewport", (24, 60, 536, 572), "shift"),
        ("resampled viewport", (24, 60, 536, 572), "scale"),
        ("one changed pixel", (301, 278, 302, 279), "clear"),
        ("paint outside right edge", (536, 60, 537, 572), "white"),
        *((name, region, "clear") for name, region, _, _ in LABELS),
    ):
        changed = screen.copy()
        if operation == "shift":
            changed.paste(screen.crop(region), (25, 60))
        elif operation == "scale":
            changed.paste(screen.crop(region).resize((256, 256)).resize((512, 512)), region)
        else:
            color = (255, 255, 255) if operation == "white" else (0, 0, 0)
            ImageDraw.Draw(changed).rectangle((region[0], region[1], region[2]-1, region[3]-1), fill=color)
        try:
            verify(changed)
        except ValueError:
            continue
        raise AssertionError(f"failed to reject {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("png", type=Path)
    parser.add_argument("--self-test", action="store_true", help="also reject shifted, resampled, damaged and obscured copies")
    args = parser.parse_args()
    with Image.open(args.png) as source:
        image = source.convert("RGB")
    try:
        verify(image)
    except ValueError as exc:
        parser.exit(1, f"MAG13 FAIL: {exc}\n")
    print("MAG13 PASS: 262144 exact RGB565 pixels at (24,60)..(535,571), 2052 black guard pixels, CAPTURE OFF, Test pattern, Stop and Home.")
    if args.self_test:
        check_faults(image)
        print("MAG13 checker rejected all seven altered snapshots.")
