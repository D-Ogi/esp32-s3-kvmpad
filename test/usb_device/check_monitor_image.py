# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check normal HID and both USB monitor profiles in a linked ESP32-S3 monitor evaluation image."""
import argparse
from pathlib import Path
import sys

from elftools.elf.elffile import ELFFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host_unit"))
from check_hpe_descriptors import symbol, verify


def verify_controls(elf):
    original = symbol(elf, "s_monitor_device")
    device = symbol(elf, "s_controls_device")
    assert device[:10] == original[:10] and device[10:12] == bytes.fromhex("86 29")
    assert device[12:] == original[12:] and device[4:7] == bytes(3)
    config = symbol(elf, "s_controls_config")
    assert config[:9] == bytes.fromhex("09 02 52 00 03 01 00 80 fa") and len(config) == 82
    assert config[9:32] == symbol(elf, "s_monitor_config")[9:]
    for index, report in enumerate(("s_report_keyboard", "s_report_mouse")):
        number = index + 1
        start = 32 + 25*index
        assert config[start:start+9] == bytes((9, 4, number, 0, 1, 3, 1, number, 5+index))
        assert config[start+9:start+16] == bytes.fromhex("09 21 11 01 00 01 22")
        assert int.from_bytes(config[start+16:start+18], "little") == len(symbol(elf, report))
        assert config[start+18:start+25] == bytes((7, 5, 0x82+index, 3, 8, 0, 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    with args.elf.open("rb") as stream:
        elf = ELFFile(stream)
        verify(elf, 4)
        hid = symbol(elf, "s_device")
        monitor = symbol(elf, "s_monitor_device")
        assert len(monitor) == 18 and monitor[:10] == hid[:10]
        assert monitor[8:12] == bytes.fromhex("3a 30 87 29")
        assert monitor[12:18] == bytes.fromhex("01 01 01 02 03 01") and monitor[16] == hid[16]
        assert symbol(elf, "s_monitor_config") == bytes.fromhex(
            "09 02 20 00 01 01 00 80 fa 09 04 00 00 02 ff 00 00 04 "
            "07 05 01 02 40 00 00 07 05 81 02 40 00 00")
        verify_controls(elf)
        for label, geometry in (("s_monitor_caps", "640x480"), ("s_native_caps", "1024x600")):
            caps = symbol(elf, label)
            assert caps[:2] == bytes((len(caps) - 2, 3)) and caps[-2:] == bytes(2)
            assert caps[2:caps[0]].decode("utf-16-le") == f"esp32s3udisp0_R{geometry}_Ejpg4_Fps10_Bl96"
        for name in ("__wrap_tud_descriptor_device_cb", "__wrap_tud_descriptor_configuration_cb",
                     "__wrap_tud_descriptor_string_cb", "usb_hid_device_set_monitor_mode",
                     "tud_vendor_rx_cb", "usb_monitor_feed", "usb_monitor_view_open_mode"):
            assert symbol(elf, name), name
    print("PASS boot HID (4 interfaces), display-only 303A:2987 (vendor interface 0, 64-byte bulk OUT/IN), "
          "composite 303A:2986 (vendor MI_00, boot keyboard/mouse, three IN endpoints), "
          "serial index, full UTF-16 capability string and linked runtime switch")


if __name__ == "__main__":
    main()
