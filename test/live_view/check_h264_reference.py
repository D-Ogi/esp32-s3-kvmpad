# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Compare an independent H.264 decode with the handoff's exact-mode YUV420p vectors."""

import argparse
import hashlib
import json
import subprocess
import tempfile
import zipfile
from pathlib import Path

import numpy as np


def compare(archive, golden, directory):
    root = "KVMpad_H264_complete/"
    spec = json.loads(archive.read(golden))
    source = spec["input"]
    if Path(source).name != source:
        raise ValueError("expected a sample filename")
    compressed = archive.read(root + "samples/" + source)
    if hashlib.sha256(compressed).hexdigest() != spec["input_sha256"]:
        raise ValueError(f"{source}: input hash")
    with tempfile.NamedTemporaryFile(dir=directory, suffix=Path(source).suffix, delete=False) as f:
        path = Path(f.name)
        f.write(compressed)
    try:
        args = ["ffmpeg", "-nostdin", "-v", "error", "-i", str(path), "-map", "0:v:0",
                "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"]
        with subprocess.Popen(args, stdout=subprocess.PIPE) as proc:
            try:
                w, h = spec["width"], spec["height"]
                for index, frame in enumerate(spec["frames"]):
                    digest = hashlib.sha256()
                    for plane, scale in (("Y", 2), ("U", 1), ("V", 1)):
                        height, width = h * scale, w * scale
                        raw = proc.stdout.read(width * height)
                        if len(raw) != width * height:
                            raise ValueError(f"{source}: truncated frame {index}")
                        # Exact mode averages each 2x2 source block with half-up rounding.
                        blocks = np.frombuffer(raw, np.uint8).reshape(height // 2, 2, width // 2, 2)
                        reduced = ((blocks.sum(axis=(1, 3), dtype=np.uint16) + 2) >> 2).astype(np.uint8)
                        if hashlib.sha256(reduced).hexdigest() != frame["planes_sha256"][plane]:
                            raise ValueError(f"{source}: {plane} mismatch at {index}")
                        digest.update(reduced)
                    if digest.hexdigest() != frame["sha256"]:
                        raise ValueError(f"{source}: frame hash at {index}")
                if proc.stdout.read(1) or proc.wait() or len(spec["frames"]) != spec["count"]:
                    raise ValueError(f"{source}: frame count or decoder exit")
            finally:
                if proc.poll() is None:
                    proc.kill()
        print(f"{source}: {spec['count']} exact frames")
        return spec["count"]
    finally:
        path.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    with zipfile.ZipFile(args.archive) as archive:
        entries = sorted(n for n in archive.namelist() if n.endswith("--exact.json"))
        if len(entries) != 8:
            raise ValueError(f"expected eight reference inputs, found {len(entries)}")
        frames = sum(compare(archive, n, args.work) for n in entries)
    print(f"{frames} exact frames across {len(entries)} inputs")
