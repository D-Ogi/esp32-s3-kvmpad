# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Compile and run the four-interface GT911-to-HID regression with a memory-only USB transport."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    test = Path(__file__).resolve().parent
    components = test.parents[1] / "components"
    out.mkdir(parents=True, exist_ok=True)
    for name in ("esp_err.h", "esp_timer.h", "esp_random.h", "esp_check.h", "esp_log.h", "lvgl.h", "ui_platform.h",
                 "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "freertos/queue.h"):
        path = out / name
        path.parent.mkdir(exist_ok=True)
        path.write_text('#include "ptp_test_platform.h"\n', encoding="ascii")
    sources = {"touch_input": ["touch_sample"], "ui_touch_bridge": ["ui_touch_bridge", "ptp_contacts", "touch_router", "local_contacts"],
               "gesture_engine": ["gesture_engine"], "hid_core": ["hid_core", "hid_dispatch", "hid_keymap", "kbd_fsm"],
               "usb_hid_device": ["usb_hid_reports", "hpe_reports", "hpe_studio"]}
    flags = ["-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-DCONFIG_TINYUSB_HID_COUNT=4", "-I", out, "-I", test]
    files = [test / "ptp_pipeline.c"]
    for component, names in sources.items():
        flags.extend(["-I", components / component / "include"])
        files.extend(components / component / "src" / (name + ".c") for name in names)
    objects = []
    for src in files:
        obj = out / (src.stem + ".o")
        sut = ["-DKVM_TEST_BRIDGE_TASK"] if src.stem == "ui_touch_bridge" else []
        subprocess.run([args.cc, *flags, *sut, "-c", src, "-o", obj], check=True)
        objects.append(obj)
    exe = out / "ptp-pipeline.exe"
    subprocess.run([args.cc, "-fsanitize=address,undefined", *objects, "-lpthread", "-o", exe], check=True)
    subprocess.run([exe], check=True, timeout=30)


if __name__ == "__main__":
    main()
