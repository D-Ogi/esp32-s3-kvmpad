# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check the physical boot HID pair using the Linux USB and input stacks."""

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
import platform
from pathlib import Path
import stat
import subprocess


def read(path):
    return path.read_text(encoding="ascii").strip()


def observe_nodes(helper, nodes, target, serial, timeout):
    if not 1 <= timeout <= 900:
        raise ValueError("Input observation timeout must be between 1 and 900 seconds")
    with ExitStack() as stack:
        handles = []
        for sysnode, devnode in nodes:
            expected = sysnode.resolve(strict=True)
            fd = os.open(devnode, os.O_RDONLY | os.O_NONBLOCK)
            stack.callback(os.close, fd)
            info = os.fstat(fd)
            dev = f"{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}"
            if (not stat.S_ISCHR(info.st_mode) or read(expected / "dev") != dev
                    or (Path("/sys/dev/char") / dev).resolve(strict=True) != expected):
                raise RuntimeError("Input device changed during enumeration")
            handles.append(fd)
        if read(target / "serial") != serial:
            raise RuntimeError("Board changed during enumeration")
        subprocess.run([helper.resolve(strict=True), *map(str, handles), str(timeout)],
                       pass_fds=handles, stdin=subprocess.DEVNULL, timeout=timeout + 10, check=True)


def inspect(serial, expected_descriptor_sha256=None, *, observer=None, timeout=60):
    devices = Path("/sys/bus/usb/devices")
    matches = [d for d in devices.iterdir() if (d / "idVendor").exists()
               and read(d / "idVendor") == "303a" and read(d / "idProduct") == "400c"
               and (d / "serial").exists() and read(d / "serial") == serial]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one matching KVMpad, found {len(matches)}")
    target = matches[0]
    if int(read(target / "bNumInterfaces")) != 2:
        raise RuntimeError("Expected the startup USB configuration with two HID interfaces")
    # sysfs concatenates the device and configuration descriptors in USB order.
    # Append both HID report descriptors in interface order to match the ELF qualification.
    descriptor_hash = hashlib.sha256((target / "descriptors").read_bytes())
    interfaces = []
    event_nodes = []
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
        if observer is not None:
            event_nodes.append((event, Path("/dev/input") / event.name))
            if number == 0:
                raw, = descriptor.parent.glob("hidraw/hidraw*")
                keyboard_raw = (raw, Path("/dev") / raw.name)
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
    result = dict(kernel=platform.release(), bcdDevice=read(target / "bcdDevice"),
                  descriptor_sha256=digest, interfaces=interfaces)
    if observer is not None:
        observe_nodes(observer, [*event_nodes, keyboard_raw], target, serial, timeout)
        result["input_probe_passed"] = True
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="USB serial of the physical KVMpad to inspect")
    parser.add_argument("--expect-descriptor-sha256", help="qualified SHA-256 of the four startup descriptor tables")
    parser.add_argument("--observe-input", type=Path, metavar="HELPER",
                        help="run the compiled usb_input_linux observer on this board's keyboard and mouse")
    parser.add_argument("--timeout", type=int, default=60, help="seconds to wait for the fixed input sequence (1 to 900)")
    args = parser.parse_args()
    print(json.dumps(inspect(args.serial, args.expect_descriptor_sha256,
                             observer=args.observe_input, timeout=args.timeout), sort_keys=True))
