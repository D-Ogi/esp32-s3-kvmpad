# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Compare horizontal line positions across still and moving frames on an entirely synthetic monitor."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import time
from unittest.mock import patch

from media import Capture, CROP_PREFIX, FORMATS, MODES
from framing import Receiver
from test_capture_surface import decode
from test_loopback_pixels import require_fixture


def edges(mask):
    return [y for y in range(1, len(mask)) if mask[y] != mask[y - 1]]


async def sample(surface, ffmpeg, codec, mode, pace):
    width, height = FORMATS[codec].width, FORMATS[codec].height
    factor = MODES[mode]
    cw, ch = width * factor // 2, height * factor // 2
    moves = [1, 2, 4, 8, 16, 32, 16, 8, 4, 2, 1, 0, -4, -16, 0]
    offsets = [0] * 150 + moves + [0] * 5
    stream = 21
    receiver = Receiver(codec, stream)
    capture = Capture(surface, ffmpeg)
    row_coverage = {}
    hashes = {}
    worst = 0
    max_light_spill = 0
    try:
        with patch.dict(os.environ, KVMPAD_TEST_LAYOUT="horizontal-ruler", KVMPAD_TEST_GPU="warp"):
            await capture.open(codec, mode)
        start = time.monotonic()
        for seq, dy in enumerate(offsets):
            msg, record = await capture.frame_with_crop(stream, seq)
            payload = bytes(receiver.receive(msg))
            assert receiver.sequence == seq
            left, top = 1024 - cw // 2, 640 + dy - ch // 2
            prefix = CROP_PREFIX.unpack(bytes.fromhex(record)[:CROP_PREFIX.size])
            assert prefix == (b"KCM1", -1024, 460 + dy, left - 2048, top - 180, cw, ch, 0, 0, 0, bytes(3))
            digest = hashlib.sha256(payload).hexdigest()
            assert hashes.setdefault(dy, digest) == digest, (codec, mode, seq, "changed at the same cursor")
            if digest not in row_coverage:
                pixels = decode(ffmpeg, codec, payload)
                row_coverage[digest] = [
                    sum(v >= 128 for v in pixels[y * width:(y + 1) * width]) for y in range(height)]
            # Three horizontal lines have different widths, so each leading/trailing edge is identified.
            ys = [top + y * factor // 2 for y in range(height)]
            bright = [574 <= y < 586 or 640 <= y < 656 or 728 <= y < 750 for y in ys]
            expected = edges(bright)
            coverage = row_coverage[digest]
            actual = edges([count >= width // 2 for count in coverage])
            assert len(actual) == len(expected) == 6, (codec, mode, seq, actual, expected)
            difference = max(abs(a - b) for a, b in zip(actual, expected))
            assert difference <= 2, (codec, mode, seq, actual, expected)
            worst = max(worst, difference)
            # A localized coding error can look like an extra line without shifting the actual chart.
            for y, count in enumerate(coverage):
                if not bright[y] and min(abs(y-e) for e in expected) > 2:
                    max_light_spill = max(max_light_spill, count)
            if seq == 149:
                still_elapsed = time.monotonic() - start
            if pace and seq < 149:
                await asyncio.sleep(max(0, start + (seq + 1) * .2 - time.monotonic()))
    finally:
        await capture.close()
    assert max_light_spill == 0, (codec, mode, "bright pixels far from a horizontal line", max_light_spill)
    return dict(codec=codec, mode=mode, frames=len(offsets), consecutive_still=150,
                still_seconds=round(still_elapsed, 3), still_payload_sha256=hashes[0],
                unique_encoded_payloads=len(row_coverage), max_line_edge_error_px=worst,
                max_unexpected_bright_pixels_in_one_dark_row=max_light_spill, output_width=width)


async def main(args):
    surface = require_fixture(args.surface)
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise ValueError("FFmpeg is required.")
    cases = [("cinepak-intra", mode) for mode in ("PIXEL_1X", "ZOOM_2X", "OVERVIEW")]
    cases.extend([("jpeg", "PIXEL_1X"), ("h264-intra", "PIXEL_1X")])
    rows = []
    for i, (codec, mode) in enumerate(cases):
        row = await sample(surface, ffmpeg, codec, mode, pace=(i == 0))
        print(json.dumps(row), flush=True)
        rows.append(row)
    if args.report:
        args.report.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--surface", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    asyncio.run(main(parser.parse_args()))
