# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Generate a 48-picture CFR H.264 fixture with distinct Blend and Smart loose outputs."""
import argparse
from pathlib import Path
import subprocess


def frames():
    for n in range(48):
        # The moving square covers a small region. The faster pan between
        # frames 16 and 32 also changes the reference thumbnails.
        offset = min(16, max(0, n - 16)) * 15
        yield bytes(
            230 if 8 + 3*n <= x < 40 + 3*n and 30 <= y < 62
            else 32 + ((x + offset) // 8 % 4) * 45 + (y // 16 % 3) * 10
            for y in range(180) for x in range(320)
        )
        u = bytes(64 + ((2*x + offset) // 20 % 4) * 32 for x in range(160))
        yield u * 90
        yield b"".join(bytes([60 + (2*y // 16 % 4) * 40]) * 160 for y in range(90))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="New MP4 path for the 320x180 source")
    args = parser.parse_args()
    command = [
        "ffmpeg", "-nostdin", "-v", "error", "-n", "-f", "rawvideo", "-pix_fmt", "yuv420p",
        "-s", "320x180", "-r", "24", "-i", "-", "-an", "-frames:v", "48", "-threads", "1",
        "-c:v", "libx264", "-profile:v", "main", "-crf", "19", "-x264-params",
        "bframes=3:b-adapt=0:b-pyramid=none:ref=2:keyint=48:min-keyint=48:scenecut=0:threads=1",
        "-map_metadata", "-1", "-fflags", "+bitexact", args.output,
    ]
    subprocess.run(command, input=b"".join(frames()), check=True)
