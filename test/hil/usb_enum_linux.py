# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check the physical boot HID pair using the Linux USB and input stacks."""

import argparse
import hashlib
import json
import platform
from pathlib import Path


def read(path):
    return path.read_text(encoding="ascii").strip()


def inspect(serial, expected_descriptor_sha256=None):
    devices = Path("/sys/bus/usb/devices")
    matches = [d for d in devices.iterdir() if (d / "idVendor").exists()
               and read(d / "idVendor") == "303a" and read(d / "idProduct") == "400c"
               and (d / "serial").exists() and read(d / "serial") == serial]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one matching KVMpad, found {len(matches)}")
    target = matches[0]
    if int(read(target / "bNumInterfaces")) != 2:
        raise RuntimeError("The selected USB configuration is not the two-interface profile")
    # sysfs concatenates the device and configuration descriptors in USB order.
    # Append both HID report descriptors in interface order to match the ELF qualification.
    descriptor_hash = hashlib.sha256((target / "descriptors").read_bytes())
    interfaces = []
    for number, protocol in enumerate(("01", "02")):
        interface = target / f"{target.name}:{read(target / 'bConfigurationValue')}.{number}"
        fields = [read(interface / name) for name in ("bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol")]
        if fields != ["03", "01", protocol] or (interface / "driver").resolve(strict=True).name != "usbhid":
            raise RuntimeError(f"Interface {number} is not bound as the expected USB boot HID")
        descriptor, = interface.glob("*/report_descriptor")
        hid_driver = (descriptor.parent / "driver").resolve(strict=True).name
        if hid_driver != "hid-generic":
            raise RuntimeError(f"Interface {number} is bound to {hid_driver}, expected hid-generic")
        event, = descriptor.parent.glob("input/input*/event*")
        if not (Path("/dev/input") / event.name).is_char_device():
            raise RuntimeError(f"Interface {number} has no input event device")
        capabilities = event.parent / "capabilities"
        # Linux exposes the least significant native word last in these sysfs bitmaps.
        if number == 0:
            if not int(read(capabilities / "key").split()[-1], 16) & (1 << 30):
                raise RuntimeError("The keyboard does not advertise KEY_A")
        else:
            relative = int(read(capabilities / "rel").split()[-1], 16)
            for bit, name in ((0, "REL_X"), (1, "REL_Y"), (8, "REL_WHEEL")):
                if not relative & (1 << bit):
                    raise RuntimeError(f"The mouse does not advertise {name}")
        report = descriptor.read_bytes()
        if not report:
            raise RuntimeError(f"Interface {number} returned an empty report descriptor")
        descriptor_hash.update(report)
        interfaces.append(dict(number=number, protocol=protocol, event=event.name, driver="usbhid",
                               hid_driver=hid_driver,
                               report_bytes=len(report), report_sha256=hashlib.sha256(report).hexdigest()))
        if number == 1:
            interfaces[-1]["relative_capabilities"] = hex(relative)
    digest = descriptor_hash.hexdigest()
    if expected_descriptor_sha256 is not None and digest != expected_descriptor_sha256.lower():
        raise RuntimeError(f"USB descriptors differ from the reference: read {digest}")
    return dict(kernel=platform.release(), bcdDevice=read(target / "bcdDevice"),
                descriptor_sha256=digest, interfaces=interfaces)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="USB serial of the physical KVMpad to inspect")
    parser.add_argument("--expect-descriptor-sha256", help="qualified SHA-256 of the four startup descriptor tables")
    args = parser.parse_args()
    print(json.dumps(inspect(args.serial, args.expect_descriptor_sha256), sort_keys=True))
