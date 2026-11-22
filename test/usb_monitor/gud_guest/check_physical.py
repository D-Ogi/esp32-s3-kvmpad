# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise both guest gates on virtual USB and verify rejection before drawing on an empty bus."""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from physical import MODESET, publish, usb_filter


def read_ready(work, phase, vm):
    path = work / f"ready-{phase}.json"
    deadline = time.monotonic() + 60
    while not path.exists():
        if vm.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError(f"Missing {path}. Inspect {work / 'guest.log'}")
        time.sleep(0.1)
    return json.loads(path.read_text("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    args = parser.parse_args()
    args.work.mkdir(parents=True)
    assert usb_filter(2, 44, "1234:5678") == "usb-host,bus=xhci.0,hostbus=2,hostaddr=44,vendorid=0x1234,productid=0x5678"
    for case in ("modeset", "strict-unregistered", "strict-upstream-virtual", "present", "absent", "wrong-serial", "stale-ack", "console"):
        work = args.work / case
        serial = "A1B2C3D4E5F7" if case == "wrong-serial" else "A1B2C3D4E5F6"
        vidpid = {"strict-unregistered": "1209:0001", "strict-upstream-virtual": "1209:4fb3"}.get(case, "ffff:ffff")
        cmd = [sys.executable, "-B", Path(__file__).with_name("physical.py"), args.inputs, work,
               "--no-device" if case == "absent" else "--loopback", "--vidpid", vidpid,
               "--serial", serial, "--clang", args.clang, "--qemu", args.qemu, "--timeout", "45"]
        if case.startswith("strict-"):
            cmd.append("--require-autobind")
        if case == "console":
            cmd.append("--console")
        if case == "modeset":
            cmd.append("--modeset")
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as vm:
            if case == "modeset":
                for phase, (_, _, width, height) in MODESET.items():
                    ready = read_ready(work, phase, vm)
                    log = (work / "guest.log").read_text("utf-8")
                    assert ready["source"] == "virtual-receiver" and "GUD_DRAW_COMPLETE" not in log
                    assert f"GUD_MODE_MATCH {phase} {width}x{height} pixels={width * height}" in log
                    publish(work / f"ack-{phase}.json", dict(ready, divisor=1, examined=width * height, mismatch=0))
            if case == "console":
                for n, phase in enumerate(("tty1", "tty2"), 1):
                    ready = read_ready(work, phase, vm)
                    log = (work / "guest.log").read_text("utf-8")
                    assert ready["source"] == "virtual-receiver" and "GUD_VT_ACTIVE tty1 fb0" in log
                    assert (f"GUD_VT_MATCH phase={n} font={ready['font_width']}x{ready['font_height']} "
                            f"pixels=307200 lit={ready['font_lit']} text={ready['text']}") in log
                    assert "GUD_CONSOLE_COMPLETE" not in log and "DRM driver=" not in log
                    assert not (work / "ready-tty2.json").exists() or n == 2
                    publish(work / f"ack-{phase}.json", dict(ready, examined=307200, divisor=1,
                                                             mismatch=0, lit=ready["font_lit"]))
            if case in ("present", "stale-ack", "strict-upstream-virtual"):
                for phase in ("full", "edge"):
                    ready = read_ready(work, phase, vm)
                    time.sleep(0.35)
                    assert not (work / "ready-edge.json").exists() or phase == "edge"
                    assert "GUD_DRAW_COMPLETE" not in (work / "guest.log").read_text("utf-8")
                    assert ready["source"] == "virtual-receiver"
                    # draw-ack-virtual waits for the production receiver to validate every pixel.
                    expected = "mismatch=0,189" if phase == "full" else "mismatch=189,0"
                    assert expected in (work / "guest.log").read_text("utf-8")
                    if case == "stale-ack":
                        ready["run"] = "old-run-token"
                    publish(work / f"ack-{phase}.json", dict(ready, examined=19200, mismatch=0,
                            inverted_samples=0 if phase == "full" else 10))
                    if case == "stale-ack":
                        break
            output = vm.communicate(timeout=60)[0]
            (work / "launcher.log").write_text(output, encoding="utf-8")
        log = (work / "guest.log").read_text("utf-8")
        assert "usb-host" not in (work / "qemu.json").read_text("utf-8")
        if case.startswith("strict-"):
            assert f"GUD_VIRTUAL_ID {vidpid} dummy_udc.0" in log
            assert "GUD_NEW_ID " not in log and "GUD_BIND runtime" not in log
        if case == "console":
            assert vm.returncode == 0 and "GUD_CONSOLE_COMPLETE" in log and "GUD_DRAW_COMPLETE" not in log
        elif case in ("present", "strict-upstream-virtual", "modeset"):
            assert vm.returncode == 0 and "PASS loopback" in output and "GUD_DRAW_COMPLETE" in log
            if case == "strict-upstream-virtual":
                assert "match=usb:v1209p4FB3d*dc*dsc*dp*icFFisc*ip*in*" in log
                assert "GUD_LOAD modalias" in log and "GUD_BIND upstream" in log and "DRM driver=gud" in log
            else:
                assert "GUD_BIND runtime" in log and "GUD_NEW_ID guest" in log
        else:
            why = {"absent": "GUD_REJECT absent", "wrong-serial": "GUD_REJECT serial",
                   "stale-ack": "Invalid acknowledgment", "strict-unregistered": "GUD_REJECT no-upstream-alias"}[case]
            assert vm.returncode != 0 and why in output and "GUD_DRAW_COMPLETE" not in log
            if case == "strict-unregistered":
                assert "match=none" in log and "GUD_BIND none" in log
            if case != "stale-ack":
                assert "DRM driver=" not in log and "GUD_LCD_READY" not in log
        print(f"PASS {case}", flush=True)


if __name__ == "__main__":
    main()
