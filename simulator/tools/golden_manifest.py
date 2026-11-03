#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Write simulator/golden/MANIFEST.md: one row per golden image with its size, geometry and SHA256.

The manifest is the record that a golden set belongs to one `kvm_sim --update-golden` run. Regenerate it in the
same step as the images, never separately:

    python -I -X utf8 simulator/tools/golden_manifest.py --lvgl 9.6.0

Standard library only; no image library is needed because the PNG header is read directly.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(HERE)
GOLDEN = os.path.join(SIM, "golden")
FIXTURES = os.path.join(SIM, "fixtures")

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def png_geometry(path: str) -> tuple[int, int, int]:
    """(width, height, bit depth) from the IHDR chunk."""
    with open(path, "rb") as fh:
        head = fh.read(26)
    if len(head) < 26 or head[:8] != PNG_MAGIC or head[12:16] != b"IHDR":
        raise ValueError(f"{path} is not a PNG")
    width, height = struct.unpack(">II", head[16:24])
    return width, height, head[24]


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fixture_title(name: str) -> str:
    path = os.path.join(FIXTURES, name + ".fx")
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line.startswith("title "):
                return line[len("title "):].strip()
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl", default="9.6.0", help="LVGL version the images were rendered with")
    parser.add_argument("--command", default="kvm_sim --update-golden", help="the command that produced them")
    args = parser.parse_args()

    names = sorted(f[:-4] for f in os.listdir(GOLDEN) if f.endswith(".png"))
    if not names:
        print(f"no golden image in {GOLDEN}", file=sys.stderr)
        return 1

    lines = [
        "# Golden images of the desktop simulator",
        "",
        "Generated file - do not edit by hand. Rendered by `kvm_sim` (TASK-070D) from the fixtures in",
        "`simulator/fixtures/`, one image per fixture, and regenerated together with",
        "`python -I -X utf8 simulator/tools/golden_manifest.py`.",
        "",
        f"* Renderer: LVGL {args.lvgl} (the version pinned by `dependencies.lock`), software renderer, headless",
        "  display, snapshot taken in RGB565 and expanded to 24-bit RGB - the pixel format of the ADR-026 panel.",
        f"* Geometry: 1024x600 (ADR-026 / TECH-087).",
        f"* Command: `{args.command}`",
        "* Comparison defaults: per-channel tolerance 2, at most 64 differing pixels per image.",
        "",
        "| Golden | Size | Bytes | SHA256 | Fixture |",
        "|---|---|---|---|---|",
    ]
    for name in names:
        path = os.path.join(GOLDEN, name + ".png")
        width, height, _depth = png_geometry(path)
        lines.append(
            f"| `{name}.png` | {width}x{height} | {os.path.getsize(path)} | `{sha256(path)}` | "
            f"{fixture_title(name)} |"
        )
    lines.append("")

    out = os.path.join(GOLDEN, "MANIFEST.md")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print(f"{out}: {len(names)} golden images")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
