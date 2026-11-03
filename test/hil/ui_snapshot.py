#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Decode a `ui snap [div]` console capture (SNAP_ROW lines, RGB565 hex) into a PNG.

    python -I test/hil/ui_snapshot.py <capture.log> <out.png>

Standard library only (zlib PNG writer). The running hash printed in SNAP_END is recomputed and
compared, so a dropped or corrupted serial line is detected instead of producing a silent picture.
"""
import re
import struct
import sys
import zlib
from pathlib import Path


def rgb565_to_rgb888(px: int) -> tuple[int, int, int]:
    r = (px >> 11) & 0x1F
    g = (px >> 5) & 0x3F
    b = px & 0x1F
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def write_png(path: Path, w: int, h: int, rows: list[bytes]) -> None:
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    end = re.search(r"SNAP_END w=(\d+) h=(\d+) crc=([0-9A-F]{8}) err=(\w+)", text)
    if not end:
        print("no SNAP_END line")
        return 1
    w, h, crc_dev, err = int(end.group(1)), int(end.group(2)), int(end.group(3), 16), end.group(4)
    if err != "ESP_OK":
        print(f"device reported {err}")
        return 1
    rows: dict[int, bytes] = {}
    crc = 2166136261
    for m in re.finditer(r"SNAP_ROW (\d+) ([0-9A-F]+)", text):
        y = int(m.group(1))
        hexs = m.group(2)
        if len(hexs) != w * 4:
            print(f"row {y}: {len(hexs) // 4} pixels, expected {w}")
            return 1
        px = [int(hexs[i:i + 4], 16) for i in range(0, len(hexs), 4)]
        for p in px:
            crc = ((crc * 16777619) & 0xFFFFFFFF) ^ p
        rows[y] = b"".join(bytes(rgb565_to_rgb888(p)) for p in px)
    missing = [y for y in range(h) if y not in rows]
    if missing:
        print(f"missing rows: {missing[:10]}{'...' if len(missing) > 10 else ''}")
        return 1
    if crc != crc_dev:
        print(f"hash mismatch: device {crc_dev:08X} host {crc:08X}")
        return 1
    write_png(Path(sys.argv[2]), w, h, [rows[y] for y in range(h)])
    print(f"wrote {sys.argv[2]} {w}x{h} hash {crc:08X} OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
