# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Compare in-place and alternating RGB565 buffers byte-for-byte with a native H.264 decode."""
import argparse
from contextlib import ExitStack, contextmanager
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/live_view"))
import prepare_card_video as card


@contextmanager
def read_pixels(checker, source, cache, layout, mode):
    command = [checker, source, cache, "pixels-" + layout, str(mode)]
    with subprocess.Popen(command, stdout=subprocess.PIPE) as child:
        try:
            yield child.stdout
            if child.stdout.read(1) or child.wait():
                raise ValueError(f"{layout}-buffer reader failed or returned extra frames")
        finally:
            if child.poll() is None:
                child.kill()


def check(source, cache, native, checker, mode):
    rows = []
    with ExitStack() as stack:
        w, h, frames = stack.enter_context(card.frame_source(source, native, mode))
        copies = {layout: stack.enter_context(read_pixels(checker, source, cache, layout, card.MODES[mode][0]))
                  for layout in ("one", "two", "two-window")}
        next_pts = 0
        for i, (duration, pts, rgb) in enumerate(frames):
            if i == 0:
                origin = pts
            timestamp = next_pts if card.UNKNOWN_PTS in (origin, pts) else pts - origin
            next_pts = timestamp + duration
            crc = zlib.crc32(rgb)
            for layout, output in copies.items():
                record = struct.unpack("<IQI", card.read(output, 16))
                if record != (i, timestamp, crc) or card.read(output, len(rgb)) != rgb:
                    raise ValueError(f"{source.name} {mode} {layout}-buffer mismatch at frame {i}")
            rows.append({"frame": i, "pts_us": timestamp, "rgb565_crc32": f"{crc:08x}",
                         "rgb565_sha256": hashlib.sha256(rgb).hexdigest()})
        if not rows:
            raise ValueError("No decoded frames")
    print(f"{cache.name}: {len(rows)} frames, {w}x{h}, identical pixels, CRC and PTS "
          "with one or two surfaces and caller-owned or reader-owned windows.", flush=True)
    return {"mode": mode, "file": cache.name, "sha256": card.digest(cache).hex(), "width": w, "height": h,
            "one_surface_bytes": 2 * w * h, "two_surface_bytes": 4 * w * h, "frames": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("native_decoder", type=Path)
    parser.add_argument("device_reader", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    result = {"source": source.name, "source_sha256": card.digest(source).hex(), "companions": []}
    for mode in ("exact", "reduced", "bank"):
        cache = Path(str(source) + card.MODES[mode][1])
        result["companions"].append(check(source, cache, args.native_decoder.resolve(), args.device_reader.resolve(), mode))
    if args.report:
        with args.report.open("x", encoding="ascii") as report:
            json.dump(result, report, indent=2)
            report.write("\n")
