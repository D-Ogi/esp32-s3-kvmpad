#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Decode a `ui snap [div] [compact]` console capture into a PNG.

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


def decode_row(line: bytes, width: int) -> tuple[int, bytes]:
    """Return the row index and its exact big-endian RGB565 words."""
    fields = line.split(b" ", 2)
    if not (1 <= width <= 1024 and len(fields) == 3 and fields[0] in (b"SNAP_ROW", b"SNAP_RLE")
            and re.fullmatch(rb"[0-9]{1,4}", fields[1]) and 0 < len(fields[2]) <= width * 4):
        raise ValueError("Invalid snapshot row")
    parts = fields[2].split(b"*")
    if (fields[0] == b"SNAP_ROW") != (len(parts) == 1):
        raise ValueError("Unexpected snapshot row encoding")
    row = bytearray()
    for i, part in enumerate(parts):
        if len(part) % 4 or not re.fullmatch(b"[0-9A-F]*", part):
            raise ValueError("Invalid RGB565 word")
        if i:
            count = int(part[:4], 16) if len(part) >= 8 else 0
            if not 3 <= count <= width - len(row) // 2:
                raise ValueError("Invalid RGB565 repeat length")
            row.extend(bytes.fromhex(part[4:8].decode("ascii")) * count)
            part = part[8:]
        if len(row) + len(part) // 2 > width * 2:
            raise ValueError("Snapshot row exceeds the expected width")
        row.extend(bytes.fromhex(part.decode("ascii")))
    if len(row) != width * 2:
        raise ValueError("Incomplete snapshot row")
    return int(fields[1]), bytes(row)


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
    if not (1 <= w <= 1024 and 1 <= h <= 600):
        print("Invalid snapshot dimensions")
        return 1
    rows = []
    crc = 2166136261
    try:
        for line in text[:end.start()].splitlines():
            if not line.startswith(("SNAP_ROW ", "SNAP_RLE ")):
                continue
            y, pixels = decode_row(line.encode("ascii"), w)
            if y != len(rows) or y >= h:
                raise ValueError(f"Unexpected snapshot row {y}")
            rgb = bytearray()
            for (p,) in struct.iter_unpack(">H", pixels):
                crc = ((crc * 16777619) & 0xFFFFFFFF) ^ p
                rgb.extend(rgb565_to_rgb888(p))
            rows.append(bytes(rgb))
    except ValueError as exc:
        print(exc)
        return 1
    if len(rows) != h:
        print(f"Incomplete snapshot: received {len(rows)} of {h} rows")
        return 1
    if crc != crc_dev:
        print(f"hash mismatch: device {crc_dev:08X} host {crc:08X}")
        return 1
    write_png(Path(sys.argv[2]), w, h, rows)
    print(f"wrote {sys.argv[2]} {w}x{h} hash {crc:08X} OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
