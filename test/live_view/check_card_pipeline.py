# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Replay original KVC1 frame transfer sizes through the LVGL presenter with controlled SD and flush delays."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import struct
import subprocess
import zlib


def transfer_sizes(companion):
    data = companion.read_bytes()
    magic, w, h, mode, frames = struct.unpack_from("<4sHHII", data)
    rows, header_crc = struct.unpack_from("<II", data, 56)
    if (magic, w, h, mode, rows) != (b"KVC1", 960, 540, 0, 8) or zlib.crc32(data[:60]) != header_crc:
        raise ValueError("Expected an original-size Exact KVC1 companion")
    pos, sizes = 64, []
    for _ in range(frames):
        start = pos
        pos += 16
        for _ in range(0, h, rows):
            pos += 4 + struct.unpack_from("<H", data, pos)[0]
        sizes.append(pos - start)
    if pos != len(data) or len(sizes) < 12:
        raise ValueError("Incomplete frame records")
    return hashlib.sha256(data).hexdigest(), sizes


def measure(args):
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    digest, sizes = transfer_sizes(args.companion)
    # One data line, 20 MHz, ignoring SD framing, turnaround, seeks and software.
    # Do not fit per-command latency from a first-to-last LCD aggregate.
    schedule = work / "delay-ms.txt"
    delays = [(size + 2499) // 2500 for size in sizes[:12]]
    schedule.write_text("".join(f"{ms}\n" for ms in delays))
    trials = {"serial": [], "pipelined": []}
    for run in range(5):
        for mode in (("serial", "pipelined") if run % 2 == 0 else ("pipelined", "serial")):
            binary = args.serial if mode == "serial" else args.updated
            log = subprocess.check_output([binary.resolve(), "--card-pipeline", schedule, mode], text=True)
            (work / f"{mode}-{run}.log").write_text(log)
            match = re.search(r"prepared_pipeline frames=12 overlaps=(\d+) first_to_last_us=(\d+)", log)
            if not match or int(match[1]) != (0 if mode == "serial" else 11):
                raise AssertionError(log)
            trials[mode].append(int(match[2]))
    for mode in ("fail", "removed"):
        log = subprocess.check_output([args.updated.resolve(), "--card-pipeline", schedule, mode], text=True)
        if f"prepared_pipeline {mode} frames=6 return=" not in log:
            raise AssertionError(log)
        (work / f"{mode}.log").write_text(log)
    median = {mode: statistics.median(samples) for mode, samples in trials.items()}
    print(json.dumps({"companion": args.companion.name, "sha256": digest, "frames": len(sizes),
                      "total_bytes": 64 + sum(sizes), "first_12_record_bytes": sizes[:12],
                      "simulated_card_ms": delays, "simulated_flush_ms": 30,
                      "minimum_wire_us_whole_companion": (64 + sum(sizes)) * 8 / 20,
                      "window_us": trials, "median_us": median,
                      "median_window_reduction_percent": 100 * (1 - median["pipelined"] / median["serial"])},
                     indent=2))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for key in ("companion", "serial", "updated", "work"):
        p.add_argument("--" + key, type=Path, required=True)
    measure(p.parse_args())
