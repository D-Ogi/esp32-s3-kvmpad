# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise the production touch bridge on the real headless LVGL from a simulator build."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim-build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ninja", default="ninja")
    args = parser.parse_args()
    test = Path(__file__).resolve().parent
    components = test.parents[1] / "components"
    out, build = args.out.resolve(), args.sim_build.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for header in ("esp_err.h", "esp_check.h", "esp_log.h", "esp_timer.h", "freertos/FreeRTOS.h",
                   "freertos/task.h", "freertos/queue.h"):
        shim = out / header
        shim.parent.mkdir(exist_ok=True)
        shim.write_text('#include "local_pointer_platform.h"\n', encoding="ascii")
    commands = subprocess.check_output([args.ninja, "-C", build, "-t", "commands", "kvm_sim"], text=True)
    base = next(c for c in commands.splitlines() if c.endswith("/kvm_sim.c")).split(" -MD ", 1)[0]
    cc = base.split(" -", 1)[0].strip('"')
    env = dict(os.environ, PATH=str(Path(cc).parent) + os.pathsep + os.environ["PATH"])
    modules = ("ui_touch_bridge", "gesture_engine", "hid_core", "usb_hid_device", "touch_input", "ui_platform")
    includes = " ".join(f'-I"{p.as_posix()}"' for p in [out, test, *(components / m / "include" for m in modules)])
    bridge = components / "ui_touch_bridge/src"
    sources = [test / "local_pointer.c", bridge / "ui_touch_bridge.c", bridge / "local_contacts.c",
               bridge / "touch_router.c", bridge / "ptp_contacts.c", components / "gesture_engine/src/gesture_engine.c"]
    objects = []
    for source in sources:
        obj = out / (source.stem + ".o")
        shim_flag = "-DKVM_TEST_BRIDGE_TASK" if source.name == "ui_touch_bridge.c" else ""
        command = f'{base} -UNDEBUG -DCONFIG_TINYUSB_HID_COUNT=4 {includes} {shim_flag} -c "{source.as_posix()}" -o "{obj.as_posix()}"'
        subprocess.run(command, cwd=build, env=env, shell=os.name != "nt", check=True)
        objects.append(obj)
    libs = re.search(r"build kvm_sim(?:\.exe)?: [^|]+?\| ([^|]+) \|\|", (build / "build.ninja").read_text())[1].split()
    exe = out / ("local-pointer.exe" if os.name == "nt" else "local-pointer")
    subprocess.run([cc, "-fsanitize=address,undefined", *objects, *(build / lib for lib in libs), "-o", exe], env=env, check=True)
    subprocess.run([exe], cwd=out, env=env, check=True, timeout=20)


if __name__ == "__main__":
    main()
