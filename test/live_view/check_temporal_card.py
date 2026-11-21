# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check both temporal KVC1 profiles with the firmware card reader and original presentation timestamps."""
import argparse
from pathlib import Path
import subprocess
import sys
import uuid
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/live_view"))
import prepare_card_video as card
from check_device_h264 import display_timing


def check(source, work, reader, verifier):
    times = display_timing(source)
    for mode in ("rr_blend", "rr_smart_loose"):
        mode_id, suffix = card.MODES[mode]
        target = work / (uuid.uuid4().hex + "-" + source.name + suffix)
        try:
            count, width, height = card.prepare(source, target, reader, mode)
            rows = subprocess.check_output([verifier, source, target, "read", str(mode_id)], text=True).splitlines()
            if len(rows) != len(times) or count != len(rows):
                raise ValueError("Unexpected card frame count")
            with card.frame_source(source, reader, mode) as (w, h, frames):
                if (w, h) != (width, height):
                    raise ValueError("Unexpected display geometry")
                for i, ((duration, pts, rgb), row, timing) in enumerate(zip(frames, rows, times, strict=True)):
                    if (pts, duration) != timing or row != f"{i} {pts-times[0][0]} {zlib.crc32(rgb):08x}":
                        raise ValueError(f"Frame or timing mismatch at {mode} {i}")
            print(f"{target.name} {count} RGB565 frames, timestamps and display intervals", flush=True)
        finally:
            target.unlink(missing_ok=True)


def check_timing_required(work, reader):
    source = work / (uuid.uuid4().hex + ".mp4")
    raw = source.with_suffix(".h264")
    try:
        # A two-tick discontinuity partway through is not a CFR sampling clock.
        subprocess.run(["ffmpeg", "-nostdin", "-n", "-v", "error", "-f", "lavfi",
                        "-i", "testsrc2=size=64x64:rate=30", "-vf", r"setpts=PTS+gte(N\,6)*2",
                        "-frames:v", "15", "-fps_mode", "vfr", "-c:v", "libx264", "-threads", "1", source],
                       check=True)
        subprocess.run(["ffmpeg", "-nostdin", "-n", "-v", "error", "-i", source, "-c:v", "copy",
                        "-bsf:v", "h264_mp4toannexb", "-f", "h264", raw], check=True)
        for mode in ("rr_blend", "rr_smart_loose"):
            for kind, clip in (("mp4", source), ("annexb", raw)):
                native = subprocess.run([reader, kind, mode, clip], capture_output=True)
                if native.returncode != 1:
                    raise ValueError(f"{mode} accepted {kind} without a CFR clock")
                target = work / (clip.name + card.MODES[mode][1])
                try:
                    card.prepare(clip, target, reader, mode)
                except ValueError:
                    if target.exists():
                        raise ValueError(f"{mode} published a partial companion")
                else:
                    raise ValueError(f"{mode} accepted {kind} without a CFR clock")
        print("Both temporal modes rejected variable-rate MP4 and untimed Annex B", flush=True)
    finally:
        raw.unlink(missing_ok=True)
        source.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path)
    parser.add_argument("reader", type=Path)
    parser.add_argument("verifier", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    for name in ("bear", "flower", "joy", "pan-cut-regenerated"):
        check(args.samples / (name + ".mp4"), args.work, args.reader.resolve(), args.verifier.resolve())
    check_timing_required(args.work, args.reader.resolve())
