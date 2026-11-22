# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify saved divisor-16 GUD motion captures. Reads files only."""

import argparse
import json
from pathlib import Path

from ack_scanout import read_scanout


def identify(rows):
    # Divisor 16 samples the viewport from physical (192,96), source (0,12).
    view = [row[12:52] for row in rows[6:36]]
    if all(color == 65535 * ((16*x//13 + (16*y+12)//11) % 2)
           for y, row in enumerate(view) for x, color in enumerate(row)):
        return None
    frame = sum((view[0][6*bit] == 65535) << bit for bit in range(6))
    if not 1 <= frame <= 20:
        raise ValueError(f"Unrecognized frame ID {frame}")
    for y, row in enumerate(view):
        for x, color in enumerate(row):
            sx, sy = x * 16, y * 16 + 12
            bit = (frame >> (sx // 96)) & 1 if sy < 64 and sx < 576 else frame & 1
            if color != 65535 * bit:
                raise ValueError(f"Frame {frame}, source pixel ({sx},{sy}) differs")
    return frame


def read_guest(work):
    events = (work / "guest.log").read_text(encoding="utf-8").splitlines()
    expected = ["GUD_MOTION_READY", "START", *(f"GUD_MOTION_FRAME {i}" for i in range(1, 21)),
                "GUD_MOTION_HELD", "ACK motion", "GUD_DRAW_COMPLETE"]
    events = [line for line in events if line.startswith("GUD_MOTION_") or line in expected]
    if events != expected:
        raise ValueError("Guest control or frame sequence is incomplete or out of order")
    previous = 0
    entries = (work / "motion-frames.jsonl").read_text(encoding="utf-8").splitlines()
    if len(entries) != 20:
        raise ValueError("Expected 20 timestamped submissions")
    for i, line in enumerate(entries, 1):
        entry = json.loads(line)
        stamp = entry["host_ns"]
        if type(entry["id"]) is not int or entry["id"] != i or type(stamp) is not int or stamp <= previous:
            raise ValueError(f"Invalid timestamped submission {i}")
        previous = stamp


def verify(work):
    read_guest(work)
    records = json.loads((work / "scans.json").read_text(encoding="utf-8"))
    names = [f"scan-{i:03d}.log" for i in range(len(records))]
    if set(names) != {p.name for p in work.glob("scan-*.log")}:
        raise ValueError("Raw scan files differ from the scans.json index")
    baseline = work / "baseline.log"
    border = None
    initial = last_id = previous_end = 0
    captures = list(zip(names, records))
    if baseline.exists():
        captures.insert(0, (baseline.name, None))
    for name, record in captures:
        try:
            rows, crc = read_scanout((work / name).read_bytes(), 16, 64, 37)
            frame = identify(rows)
            outside = tuple(color for y, row in enumerate(rows) for x, color in enumerate(row)
                            if not (6 <= y < 36 and 12 <= x < 52))
            if border is not None and border != outside:
                raise ValueError("Pixels outside the viewport changed")
            border = outside
            if name == baseline.name:
                if frame is not None:
                    raise ValueError("Pre-START scan is not the checkerboard")
                continue
            if frame is None:
                if last_id:
                    raise ValueError("Checkerboard returned after motion started")
                initial += 1
            elif frame not in (last_id, last_id + 1):
                raise ValueError(f"Frame {frame} after {last_id}; missed or reordered ID")
            else:
                last_id = frame
            if record["crc"] != crc or type(record["frame"]) is not type(frame) or record["frame"] != frame:
                raise ValueError("Recorded CRC or frame ID does not match the raw scan")
            begin, end = record["begin_ns"], record["end_ns"]
            if type(begin) is not int or type(end) is not int or not (previous_end <= begin < end):
                raise ValueError("Invalid scan time interval")
            previous_end = end
        except (ValueError, KeyError, TypeError) as exc:
            raise ValueError(f"{name}: {exc}") from exc
    if last_id != 20:
        raise ValueError(f"Final observed ID was {last_id}, expected 20")
    return dict(run=work.name, scans=len(records), initial_checkerboards=initial,
                prestart_baseline_raw=baseline.exists(), numbered_scans=len(records) - initial,
                distinct_frames=last_id, viewport_samples_per_scan=1200,
                checksummed_samples_per_scan=2368)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, nargs="+")
    try:
        for folder in parser.parse_args().runs:
            print(json.dumps(verify(folder)))
    except (ValueError, OSError, KeyError, TypeError) as exc:
        parser.exit(1, f"Verification failed: {exc}\n")
