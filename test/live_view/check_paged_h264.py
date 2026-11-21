# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Reproduce the 1080 Annex B golden and cancellation gates on the signed guest's host VM."""
import argparse
import hashlib
import json
import subprocess
import tempfile
from contextlib import ExitStack
from pathlib import Path
from zipfile import ZipFile


def check(exe, image, handoff, work):
    sample = "test_1920x1080"
    root = "KVMpad_H264_complete/"
    work.mkdir(parents=True, exist_ok=True)
    with ZipFile(handoff) as z, ExitStack() as cleanup:
        with tempfile.NamedTemporaryFile(dir=work, delete=False) as tmp:
            source = Path(tmp.name)
            cleanup.callback(source.unlink, missing_ok=True)
            tmp.write(z.read(root + "samples/" + sample + ".h264"))
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        for mode, flag in (("rr", 0), ("rr_bank_large", 1)):
            vector = json.loads(z.read(root + "verification/golden/" + sample + "--" + mode + ".json"))
            if digest != vector["input_sha256"]:
                raise ValueError("Source hash mismatch")
            command = [str(exe), str(image), str(source), "128"]
            expected = [" ".join(frame["planes_sha256"][p] for p in "YUV") for frame in vector["frames"]]
            for count in (1, 0):
                run = subprocess.run([*command, str(count), str(flag)], capture_output=True, text=True, check=True)
                if run.stdout.splitlines() != expected[:count or len(expected)]:
                    raise ValueError(f"Unexpected YUV plane hashes or frame count for {mode}")
                print(mode, run.stderr.strip(), flush=True)
        with tempfile.NamedTemporaryFile(dir=work, delete=False) as tmp:
            bad = Path(tmp.name)
            cleanup.callback(bad.unlink, missing_ok=True)
            tmp.write(b"!" * 64)
        malformed = subprocess.run([str(exe), str(bad), str(source), "128", "0", "0"], capture_output=True)
        if malformed.returncode != 4 or malformed.stdout:
            raise ValueError("Malformed guest image was accepted")
        if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError("Source changed during playback")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("handoff", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    check(args.exe.resolve(), args.image.resolve(), args.handoff, args.work)
