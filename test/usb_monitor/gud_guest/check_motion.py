# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the 2 Hz motion fixture with the real Linux DRM driver and a guest-local receiver."""

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

from physical import publish


def wait_for(path, process):
    deadline = time.monotonic() + 45
    while not path.exists():
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"Missing {path}; inspect guest.log")
        time.sleep(0.05)
    return json.loads(path.read_text("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    args = parser.parse_args()
    work = args.work.resolve()
    cmd = [sys.executable, "-B", Path(__file__).with_name("physical.py"), args.inputs, work,
           "--loopback", "--motion", "--vidpid", "ffff:ffff", "--serial", "A1B2C3D4E5F6",
           "--clang", args.clang, "--qemu", args.qemu, "--timeout", "30"]
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as vm:
        ready = wait_for(work / "ready-motion.json", vm)
        time.sleep(0.65)
        assert ready["source"] == "virtual-receiver" and ready["phase"] == "ready"
        assert "GUD_MOTION_FRAME " not in (work / "guest.log").read_text("utf-8")
        publish(work / "start-motion.json", ready)
        held = wait_for(work / "held-motion.json", vm)
        time.sleep(0.65)
        log = (work / "guest.log").read_text("utf-8")
        assert held == dict(ready, phase="held") and "GUD_DRAW_COMPLETE" not in log
        expected = list(range(1, 21))
        received = [int(v) for v in re.findall(r"^GUD_VIRTUAL_VERIFIED (\d+)$", log, re.M)]
        assert received == expected, received
        rows = [json.loads(s) for s in (work / "motion-frames.jsonl").read_text().splitlines()]
        assert [row["id"] for row in rows] == expected
        times = [row["host_ns"] for row in rows]
        steps_ms = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
        # Allow ordinary host scheduler jitter while detecting unpaced / drifting submissions.
        assert all(200 <= delta <= 800 for delta in steps_ms), steps_ms
        assert 9000 <= (times[-1] - times[0]) / 1e6 <= 10000, times
        publish(work / "ack-motion.json", held)
        output = vm.communicate(timeout=20)[0]
        (work / "launcher.log").write_text(output, encoding="utf-8")
        assert vm.returncode == 0 and "PASS loopback 20 DRM submissions" in output
    assert "usb-host" not in (work / "qemu.json").read_text()
    result = dict(receiver="guest-local FunctionFS", frames=received, pixels_per_frame=640 * 480,
                  first_to_last_ms=(times[-1] - times[0]) / 1e6, min_ms=min(steps_ms), max_ms=max(steps_ms))
    publish(work / "verification.json", result)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
