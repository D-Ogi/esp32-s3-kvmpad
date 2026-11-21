# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Replay USB bus and control transfers through the pinned stack and product dispatcher in memory."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    test = Path(__file__).resolve().parent
    repo = test.parents[1]
    stack = repo / "managed_components/espressif__esp_tinyusb/tinyusb/src"
    components = repo / "components"
    for count, monitor in ((3, False), (4, False), (4, True)):
        out = args.out.resolve() / ("4-monitor" if monitor else str(count))
        out.mkdir(parents=True, exist_ok=True)
        for name in ("sdkconfig.h", "esp_err.h", "esp_timer.h", "esp_check.h", "esp_log.h", "esp_mac.h",
                     "esp_random.h", "esp_app_desc.h", "tinyusb.h", "tinyusb_default_config.h",
                     "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h"):
            header = out / name
            header.parent.mkdir(exist_ok=True)
            header.write_text('#include "session_platform.h"\n', encoding="ascii")
        flags = ["-std=gnu11", "-O1", "-g", "-I", out, "-I", test, "-I", stack,
                 f"-DCONFIG_TINYUSB_HID_COUNT={count}"]
        files = [test / "session.c", stack / "tusb.c", stack / "device/usbd.c",
                 stack / "class/hid/hid_device.c", stack / "common/tusb_fifo.c"]
        for component, names in (("usb_hid_device", ("usb_hid_device", "usb_hid_reports", "hpe_reports", "hpe_studio")),
                                 ("hid_core", ("hid_dispatch", "hid_core", "kbd_fsm", "hid_keymap"))):
            flags.extend(["-I", components / component / "include"])
            files.extend(components / component / "src" / (name + ".c") for name in names)
        wrap = ["-Wl,--wrap=hidd_reset", "-Wl,--wrap=hidd_control_xfer_cb"]
        if monitor:
            flags.extend(["-DCONFIG_KVM_USB_MONITOR_EVAL=1", "-DKVM_MEDIA_SYNTHETIC_DIAG=1",
                          "-I", components / "usb_monitor/include", "-I", components / "usb_monitor", "-I", repo / "main"])
            files += [components / "usb_hid_device/src/usb_monitor_port.c", components / "usb_monitor/monitor_wire.c",
                      stack / "class/vendor/vendor_device.c"]
            wrap += ["-Wl,--wrap=tud_descriptor_" + name + "_cb" for name in ("device", "configuration", "string")]
        exe = out / "usb-session.exe"
        subprocess.run([args.cc, *flags, *files, *wrap, "-o", exe], check=True)
        subprocess.run([exe], check=True, timeout=20)


if __name__ == "__main__":
    main()
