# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check presentation-order Y/U/V hashes from the firmware decoder API against the eight inputs."""
import argparse
import hashlib
import json
import os
import re
from fractions import Fraction
import subprocess
import tempfile
import zipfile
from pathlib import Path


def display_timing(path):
    dump = subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0",
                                    "-show_entries", "packet=pts:stream=time_base,r_frame_rate", "-of", "json", path])
    info = json.loads(dump)
    stream = info["streams"][0]
    tb = Fraction(stream["time_base"])
    pts = sorted(p["pts"] for p in info["packets"])
    # One more CFR boundary establishes the duration of the last picture.
    def nearest(x):
        return int(x + Fraction(1, 2)) if x >= 0 else -int(-x + Fraction(1, 2))
    pts += [pts[0] + nearest(len(pts) / (tb * Fraction(stream["r_frame_rate"])))]
    usec = [nearest(v * tb * 1000000) for v in pts]
    return [(a, b-a) for a, b in zip(usec, usec[1:])]


def check(archive, driver, work):
    total = 0
    modes = ("exact", "rr", "rr_bank_large", "rr_blend", "rr_smart_loose")
    tests = sorted(n for n in archive.namelist() if n.endswith(tuple("--" + m + ".json" for m in modes)))
    if len(tests) != 32:
        raise ValueError("expected 24 spatial and 8 temporal golden profiles")
    for name in tests:
        spec = json.loads(archive.read(name))
        source = spec["input"]
        data = archive.read("KVMpad_H264_complete/samples/" + source)
        if hashlib.sha256(data).hexdigest() != spec["input_sha256"]:
            raise ValueError(source)
        with tempfile.NamedTemporaryFile(dir=work, delete=False) as f:
            f.write(data)
            path = Path(f.name)
        try:
            kind = "mp4" if source.endswith(".mp4") else "annexb"
            mode = name.split("--")[-1].removesuffix(".json")
            command = [driver, kind, mode, path]
            result = subprocess.run(command, capture_output=True, check=True, text=True)
            if mode in ("rr_blend", "rr_smart_loose"):
                counts = re.search(r"temporal decoded=(\d+) synthesized=(\d+) result=(\d+)", result.stderr)
                if not counts or tuple(map(int, counts.groups())) != (spec["decoded"], spec["synthetic"], 1):
                    raise ValueError(f"{source} {mode}: wrong reference/synthetic counts")
                timing = [tuple(map(int, line.split()[1:])) for line in result.stderr.splitlines()
                          if line.startswith("timing ")]
                if timing != display_timing(path):
                    raise ValueError(f"{source} {mode}: source presentation timestamps or durations differ")
                early = subprocess.run(command, capture_output=True, text=True,
                                       env={**os.environ, "KVM_TEMPORAL_TEST_STOP": "2"})
                if early.returncode != 1 or "result=3" not in early.stderr or early.stdout.splitlines() != result.stdout.splitlines()[:2]:
                    raise ValueError(f"{source} {mode}: cancellation")

        finally:
            path.unlink()
        rows = result.stdout.splitlines()
        if len(rows) != spec["count"]:
            raise ValueError(f"{source}: expected {spec['count']} frames, got {len(rows)}")
        for i, (row, frame) in enumerate(zip(rows, spec["frames"], strict=True)):
            expected = [frame["planes_sha256"][p] for p in "YUV"]
            if row.split() != expected:
                raise ValueError(f"{source}: plane SHA256 mismatch at frame {i}")
        total += len(rows)
        print(f"{source} {mode}: {len(rows)} frames", flush=True)
    print(f"{total} frames and {total * 3} plane hashes matched")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    with zipfile.ZipFile(args.archive) as archive:
        check(archive, args.driver.resolve(), args.work)
