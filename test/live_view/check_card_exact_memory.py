# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check the four 640x360 card clips in the arena measured on unit 1, including IDR boundaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from check_device_h264 import display_timing


def verify(directory, driver, capacity):
    rows = []
    clips = [c for c in json.loads((directory / "manifest.json").read_text())["clips"]
             if (c["width"], c["height"]) == (640, 360)]
    assert sorted(c["fps"] for c in clips) == [5, 10, 15, 25]
    for c in clips:
        path = directory / c["mp4"]["name"]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == c["mp4"]["sha256"]
        env = {**os.environ, "KVM_TEST_ARENA_BYTES": str(capacity), "KVM_TEST_NATIVE_PLANES": "1",
               "KVM_TEST_REPORT_TIMING": "1"}
        result = subprocess.run([driver, "mp4", "exact", path], env=env, check=True,
                                text=True, capture_output=True)
        timing = [tuple(map(int, line.split()[1:])) for line in result.stderr.splitlines()
                  if line.startswith("timing ")]
        # MOV's DTS-based sample duration can differ by a microsecond from the
        # difference between adjacent, individually rounded display timestamps.
        oracle = display_timing(path)
        assert len(timing) == len(oracle) == c["frames"]
        assert all(a == x and abs(b - y) <= 1 for (a,b), (x,y) in zip(timing, oracle, strict=True))
        frames = result.stdout.splitlines()
        assert len(frames) == c["frames"]
        command = ["ffmpeg", "-nostdin", "-v", "error", "-i", path, "-map", "0:v:0",
                   "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"]
        with subprocess.Popen(command, stdout=subprocess.PIPE) as stock:
            for i, frame in enumerate(frames):
                for size, digest in zip((640*360, 320*180, 320*180), frame.split(), strict=True):
                    plane = stock.stdout.read(size)
                    if len(plane) != size or hashlib.sha256(plane).hexdigest() != digest:
                        raise ValueError(f"{path.name} frame {i}: native YUV plane mismatch")
            assert not stock.stdout.read(1) and stock.wait() == 0
        rows.append({"file": path.name, "frames": len(frames), "native_plane_hashes": 3*len(frames),
                     "pts_verified": len(timing)})
    return {"arena_bytes": capacity, "results": rows}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("driver", type=Path, help="32-bit bounded-frames from build_checks.py --tlsf")
    p.add_argument("--arena-bytes", type=int, default=1802240)
    args = p.parse_args()
    print(json.dumps(verify(args.directory, args.driver.resolve(), args.arena_bytes), indent=2))
