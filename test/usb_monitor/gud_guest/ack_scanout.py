# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify one newly captured 'ui snap 4 compact' log and acknowledge a waiting DRM phase."""

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

from physical import BLANK_WAKE, MODESET, VT_TEXT, publish, validate_ack

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "hil"))
from ui_snapshot import decode_row


def read_scanout(capture, divisor, width, height):
    lines = capture.splitlines()
    starts = [i for i, s in enumerate(lines) if s.startswith(b"SNAP_BEGIN ")]
    ends = [i for i, s in enumerate(lines) if s.startswith(b"SNAP_END ")]
    if len(starts) != 1 or len(ends) != 1 or lines[starts[0]] != f"SNAP_BEGIN div={divisor}".encode():
        raise ValueError(f"Expected a single divisor-{divisor} snapshot")
    end = re.fullmatch(f"SNAP_END w={width} h={height} ".encode() + rb"crc=([0-9A-F]{8}) err=ESP_OK", lines[ends[0]])
    body = lines[starts[0] + 1:ends[0]]
    sources = re.findall(rb"SNAP_SOURCE (\w+)", b"\n".join(body))
    if not end or sources != [b"scanout"]:
        raise ValueError("Missing physical scanout or incomplete snapshot")
    rows = []
    crc = 2166136261
    for line in body:
        if not line.startswith((b"SNAP_ROW ", b"SNAP_RLE ")):
            continue
        y, pixels = decode_row(line, width)
        if y != len(rows) or y >= height:
            raise ValueError("Invalid snapshot row sequence")
        row = struct.unpack(f">{width}H", pixels)
        rows.append(row)
        for color in row:
            crc = ((crc * 16777619) & 0xffffffff) ^ color
    if len(rows) != height or crc != int(end[1], 16):
        raise ValueError("Incomplete rows or scanout hash mismatch")
    return rows, f"{crc:08X}"


def measure(capture, phase):
    rows, crc = read_scanout(capture, 4, 256, 150)
    examined = mismatch = inverted = 0
    for y, row in enumerate(rows):
        for x, color in enumerate(row):
            sx, sy = 4 * x - 192, 4 * y - 84
            if 0 <= sx < 640 and 0 <= sy < 480:
                flip = phase == "edge" and sx >= 619 and sy >= 471
                white = ((sx // 13 + sy // 11) & 1) ^ flip
                mismatch += color != 65535 * white
                inverted += flip
                examined += 1
    return dict(examined=examined, inverted_samples=inverted, mismatch=mismatch, crc=crc,
                capture_sha256=hashlib.sha256(capture).hexdigest())


def acknowledge(work, phase, capture):
    ready_path = work / f"ready-{phase}.json"
    ready = json.loads(ready_path.read_text("utf-8"))
    if ready["phase"] != phase or ready["source"] != "scanout":
        raise ValueError("Expected a physical scanout readiness record")
    if capture.stat().st_mtime_ns < ready_path.stat().st_mtime_ns:
        raise ValueError("Capture predates the guest's readiness record")
    data = capture.read_bytes()
    if phase in VT_TEXT:
        metrics = measure_console(data, ready)
    elif phase in MODESET:
        metrics = measure_modeset(data, phase)
    elif phase in BLANK_WAKE:
        metrics = measure_blank_wake(data, phase)
    else:
        metrics = measure(data, phase)
    ack = dict(ready, **metrics)
    validate_ack(ready, ack)
    publish(work / f"ack-{phase}.json", ack)
    return ack


def measure_modeset(capture, phase):
    left, top, width, height = MODESET[phase]
    rows, crc = read_scanout(capture, 1, 1024, 600)
    mismatch = 0
    inverted = phase.startswith("return-")
    edge = phase.endswith("-edge")
    for y in range(height):
        for x, pixel in enumerate(rows[top + y][left:left + width]):
            white = bool((x // 13 + y // 11) & 1) ^ inverted
            if edge and y >= height - 9 and x >= width - 21:
                white = not white
            mismatch += pixel != (65535 if white else 0)
    return dict(divisor=1, examined=width * height, mismatch=mismatch, crc=crc,
                capture_sha256=hashlib.sha256(capture).hexdigest())


def measure_blank_wake(capture, phase):
    rows, crc = read_scanout(capture, 1, 1024, 600)
    mismatch = 0
    for y, row in enumerate(rows[84:564]):
        for x, pixel in enumerate(row[192:832]):
            white = phase != "blank" and (x // 13 + y // 11) % 2
            mismatch += pixel != 65535 * white
    return dict(divisor=1, examined=640 * 480, mismatch=mismatch, crc=crc,
                capture_sha256=hashlib.sha256(capture).hexdigest())


def measure_console(capture, ready):
    rows, crc = read_scanout(capture, 1, 1024, 600)
    packed = bytes.fromhex(ready["glyph_rows"])
    width = len(VT_TEXT[ready["phase"]]) * ready["font_width"]
    height = ready["font_height"]
    stride = (width + 7) // 8
    expected = [0] * 640
    mismatch = lit = 0
    for y in range(480):
        if y < height:
            for x in range(width):
                expected[x] = 65535 * ((packed[y * stride + x // 8] >> (7 - x % 8)) & 1)
        elif y == height:
            expected = [0] * 640
        actual = rows[84 + y][192:832]
        lit += actual.count(65535)
        mismatch += sum(a != b for a, b in zip(actual, expected))
    return dict(divisor=1, examined=640 * 480, lit=lit, mismatch=mismatch, crc=crc,
                capture_sha256=hashlib.sha256(capture).hexdigest())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=Path)
    parser.add_argument("phase", choices=("full", "edge", *VT_TEXT, *BLANK_WAKE, *MODESET))
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    print(json.dumps(acknowledge(args.work, args.phase, args.capture)))
