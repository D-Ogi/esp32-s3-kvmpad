# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Read only the standard USB descriptor strings of the selected host bus/address."""

import ctypes as c
import ctypes.util
import os
import shutil
from pathlib import Path


def check_target(qemu, hostbus, hostaddr, vidpid, expected_serial):
    if os.name == "nt":
        exe = shutil.which(qemu) or qemu
        library = str(Path(exe).resolve().with_name("libusb-1.0.dll"))
    else:
        library = ctypes.util.find_library("usb-1.0")
        if not library:
            raise RuntimeError("Cannot find the host libusb library")
    usb = c.CDLL(library)
    ptr, byteptr = c.c_void_p, c.POINTER(c.c_ubyte)
    devlist = c.POINTER(ptr)
    calls = {
        "libusb_init": (c.c_int, c.POINTER(ptr)),
        "libusb_exit": (None, ptr),
        "libusb_get_device_list": (c.c_ssize_t, ptr, c.POINTER(devlist)),
        "libusb_free_device_list": (None, devlist, c.c_int),
        "libusb_get_bus_number": (c.c_ubyte, ptr),
        "libusb_get_device_address": (c.c_ubyte, ptr),
        "libusb_get_device_descriptor": (c.c_int, ptr, byteptr),
        "libusb_open": (c.c_int, ptr, c.POINTER(ptr)),
        "libusb_close": (None, ptr),
        "libusb_get_string_descriptor_ascii": (c.c_int, ptr, c.c_ubyte, byteptr, c.c_int),
    }
    for name, (result, *params) in calls.items():
        fn = getattr(usb, name)
        fn.restype, fn.argtypes = result, params
    ctx = ptr()
    if usb.libusb_init(c.byref(ctx)):
        raise OSError("libusb initialization failed")
    try:
        devices = devlist()
        count = usb.libusb_get_device_list(ctx, c.byref(devices))
        if count < 0:
            raise OSError(f"libusb device enumeration failed ({count})")
        try:
            for i in range(count):
                dev = devices[i]
                if (usb.libusb_get_bus_number(dev), usb.libusb_get_device_address(dev)) != (hostbus, hostaddr):
                    continue
                desc = (c.c_ubyte * 18)()
                if usb.libusb_get_device_descriptor(dev, desc):
                    raise OSError("Could not read the selected USB descriptor")
                # Native-endian fields from struct libusb_device_descriptor.
                vid = c.c_uint16.from_buffer_copy(bytes(desc[8:10])).value
                pid = c.c_uint16.from_buffer_copy(bytes(desc[10:12])).value
                if f"{vid:04x}:{pid:04x}" != vidpid:
                    raise ValueError("Selected USB address has different VID:PID")
                handle = ptr()
                if usb.libusb_open(dev, c.byref(handle)):
                    raise OSError("Cannot read the selected GUD interface using host libusb")
                try:
                    for index, expected in ((desc[15], "KVMpad GUD"), (desc[16], expected_serial)):
                        buf = (c.c_ubyte * 256)()
                        n = usb.libusb_get_string_descriptor_ascii(handle, index, buf, len(buf))
                        if n <= 0 or bytes(buf[:n]) != expected.encode("ascii"):
                            raise ValueError("Selected USB product or serial differs from expected")
                finally:
                    usb.libusb_close(handle)
                return
            raise ValueError("Selected USB bus/address does not exist")
        finally:
            usb.libusb_free_device_list(devices, 1)
    finally:
        usb.libusb_exit(ctx)
