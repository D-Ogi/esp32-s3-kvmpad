# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build the Windows importer's native reader from the host archives produced by build_ffmpeg.py."""
import argparse
from pathlib import Path
import sys
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "tools/live_view"))
from prepare_card_video import build_reader


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ffmpeg_build", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--output", type=Path, default=HERE / "bin/card-pixels.exe")
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".card-pixels-{uuid.uuid4().hex}.exe")
    try:
        build_reader(args.cc, args.ffmpeg_build.resolve(), temporary)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
