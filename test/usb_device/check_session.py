# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Replay USB bus and control transfers through the pinned stack and product dispatcher in memory."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    test = Path(__file__).resolve().parent
    repo = test.parents[1]
    stack = repo / "managed_components/espressif__esp_tinyusb/tinyusb/src"
    components = repo / "components"
    profiles = [(2, False, False), (2, True, False), (3, False, False), (4, False, False),
                (4, True, False), (2, True, True), (4, True, True)]
    for count, monitor, gud, shot in [(*p, False) for p in profiles] + [(2, True, False, True), (4, True, True, True)]:
        out = args.out.resolve() / (str(count) + ("-monitor" if monitor else "") + ("-gud" if gud else ""))
        if shot:
            out = out.with_name(out.name + "-screenshot")
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
        if gud:
            port = components / "usb_hid_device/src/usb_gud_port.c"
            if count == 2:
                for vid, pid in ((None, None), (0, 1), (0xffff, 1), (0xfffe, 0),
                                 (0x303a, 0x400c), (0x303a, 0x2986), (0x303a, 0x2987), (0x303a, 0x400a)):
                    defs = ["-DCONFIG_KVM_USB_GUD=1"]
                    if vid is not None:
                        defs += [f"-DCONFIG_KVM_USB_GUD_VID={vid}", f"-DCONFIG_KVM_USB_GUD_PID={pid}"]
                    result = subprocess.run([args.cc, *flags, *defs, "-fsyntax-only", port],
                                            capture_output=True, text=True)
                    error = "distinct GUD USB identity" if vid == 0x303a else "authorized device VID and PID"
                    if result.returncode == 0 or error not in result.stderr:
                        raise RuntimeError(f"GUD identity guard {vid!r}/{pid!r}: {result.stderr}")
                print("PASS GUD build rejected unset, reserved, zero and four conflicting identities.", flush=True)
            # In-memory DCD only; this identity is never emitted on a physical bus.
            flags.extend(["-DCONFIG_KVM_USB_GUD=1", "-DCONFIG_KVM_USB_GUD_VID=0xfffe",
                          "-DCONFIG_KVM_USB_GUD_PID=0xf00d"])
            files.extend([port, components / "usb_monitor/gud_receiver.c"])
        if shot:
            idf = args.idf or repo.parent / ".tools/esp-idf-v6.1"
            (out / "tinyusb_msc.h").write_text('#include "msc_session_platform.h"\n', encoding="ascii")
            flags += ["-DCONFIG_KVM_USB_SCREENSHOT=1", "-DCONFIG_TINYUSB_MSC_ASYNC_IO=1",
                      "-I", idf / "components/esp_blockdev/include", "-I", components / "screenshot_volume/include"]
            files += [components / "usb_hid_device/src/usb_screenshot.c", stack / "class/msc/msc_device.c",
                      components / "screenshot_volume/screenshot_volume.c",
                      components / "screenshot_volume/screenshot_volume_file.c"]
            wrap += ["-Wl,--wrap=mscd_xfer_cb", "-Wl,--wrap=mscd_control_xfer_cb"]
        exe = out / "usb-session.exe"
        subprocess.run([args.cc, *flags, *files, *wrap, "-o", exe], check=True)
        with tempfile.TemporaryDirectory(dir=out) as private:
            subprocess.run([exe, Path(private) / "test.img"], check=True, timeout=20)


if __name__ == "__main__":
    main()
