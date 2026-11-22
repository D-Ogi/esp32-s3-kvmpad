# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

"""Parse the actual USB tables in an Xtensa usb_hid_device object or firmware ELF."""
import argparse
import hashlib
from collections import defaultdict
from pathlib import Path

from elftools.elf.elffile import ELFFile


def symbol(elf, name):
    matches = elf.get_section_by_name(".symtab").get_symbol_by_name(name)
    assert matches and len(matches) == 1, name
    sym = matches[0]
    section = elf.get_section(sym["st_shndx"])
    offset = sym["st_value"] - section["sh_addr"]
    return section.data()[offset:offset + sym["st_size"]]


def layout(data, byte_fields=None):
    sizes = defaultdict(int)
    state = {}
    stack = []
    depth = 0
    offset = 0
    usage = None
    found_fields = defaultdict(int)
    while offset < len(data):
        tag = data[offset]
        size = (0, 1, 2, 4)[tag & 3]
        assert tag != 0xfe and offset + size < len(data)
        group, item = (tag >> 2) & 3, tag >> 4
        value = int.from_bytes(data[offset + 1:offset + size + 1], "little", signed=group == 1 and item in (1, 2))
        offset += size + 1
        if group == 2 and item == 0:
            usage = value
        if group == 1:
            if item == 10:
                stack.append(state.copy())
            elif item == 11:
                state = stack.pop()
            else:
                state[item] = value
        if group == 0 and item in (8, 9, 11):
            sizes[(state.get(8, 0), item)] += state[7] * state[9]
            field = (state.get(8), state.get(0), usage)
            if byte_fields and field in byte_fields:
                assert (state[1], state[2], state[7]) == (0, 255, 8), (field, state[1], state[2], state[7])
                found_fields[field] += 1
        if group == 0 and item in (10, 12):
            depth += 1 if item == 10 else -1
            assert depth >= 0
        if group == 0:
            usage = None
    assert not stack and not depth
    if byte_fields:
        assert found_fields == byte_fields
    return dict(sizes)


def verify(elf, count):
    cfg = symbol(elf, "s_config_fs")
    dev = symbol(elf, "s_device")
    assert len(dev) == 18 and dev[4] == 0 and dev[7] == 64 and dev[17] == 1
    assert int.from_bytes(dev[12:14], "little") == {2: 0x0102, 3: 0x0100, 4: 0x0101}[count]
    assert cfg[4] == count and int.from_bytes(cfg[2:4], "little") == len(cfg)
    names = ["keyboard", "mouse"] + {2: [], 3: ["consumer"], 4: ["ptp", "control"]}[count]
    reports = [symbol(elf, "s_report_" + name) for name in names]
    interfaces, endpoints, hids = [], [], []
    off = cfg[0]
    while off < len(cfg):
        n = cfg[off]
        assert 2 <= n <= len(cfg) - off
        item = cfg[off:off + n]
        if item[1] == 4:
            interfaces.append(tuple(item[2:8]))
        elif item[1] == 5:
            endpoints.append((interfaces[-1][0], item[2], item[3], int.from_bytes(item[4:6], "little"), item[6]))
        elif item[1] == 0x21:
            assert item[2:8] == bytes([0x11, 1, 0, 1, 0x22, len(reports[len(hids)]) & 255])
            hids.append(int.from_bytes(item[7:9], "little"))
        off += n
    assert hids == list(map(len, reports))
    assert interfaces[:2] == [(0, 0, 1, 3, 1, 1), (1, 0, 1, 3, 1, 2)]
    assert interfaces[2:] == [(i, 0, 2 if i == 3 else 1, 3, 0, 0) for i in range(2, count)]
    assert endpoints[:2] == [(0, 0x81, 3, 8, 1), (1, 0x82, 3, 8, 1)]
    assert layout(reports[0]) == {(0, 8): 64, (0, 9): 8}
    assert layout(reports[1]) == {(0, 8): 40}
    if count == 4:
        assert endpoints[2:] == [(2, 0x83, 3, 64, 1), (3, 0x05, 3, 64, 4), (3, 0x84, 3, 64, 4)]
        ptp_bytes = {(0x10, 0x0d, 0x51): 5, (0x12, 0xff00, 0xc5): 1}
        studio_bytes = {(0x22, 0xff61, 2): 1, (0x23, 0xff61, 3): 1, (0x24, 0xff61, 4): 1}
        assert layout(reports[2], ptp_bytes) == {(0x10, 8): 264, (0x11, 11): 8, (0x12, 11): 2048, (0x13, 11): 8,
                                      (0x14, 11): 8, (0x15, 11): 8}
        assert layout(reports[3], studio_bytes) == {(0x20, 8): 16, (0x21, 8): 16, (0x22, 8): 504, (0x23, 9): 504,
                                      (0x24, 11): 120, (0x30, 8): 88}
        cert = symbol(elf, "s_ptp_cert_report")
        assert len(cert) == 257 and cert[:5] == bytes.fromhex("12 fc 28 fe 84") and cert[-1] == 0xc2
    elif count == 3:
        assert endpoints[2:] == [(2, 0x83, 3, 8, 10)]
        assert layout(reports[2]) == {(0, 8): 16}
    else:
        assert len(endpoints) == 2


def verify_media_ssh_image(elf, elf_path, image_path):
    """Check the combined build before selecting its binary for an OTA trial."""
    from esptool.bin_image import LoadFirmwareImage

    table = elf.get_section_by_name(".symtab")
    for name in ("ff_h264_decoder", "usb_hid_device_set_monitor_mode", "usb_hid_device_set_monitor_controls",
                 "kvm_ssh_listener_open", "kvm_ssh_serve_session"):
        if not any(item["st_size"] for item in table.get_symbol_by_name(name) or []):
            raise ValueError(f"Combined firmware is missing {name}")

    # These four tables are the ones enumerated at startup in the hardware qualification run.
    tables = [symbol(elf, name) for name in ("s_device", "s_config_fs", "s_report_keyboard", "s_report_mouse")]
    if hashlib.sha256(b"".join(tables)).hexdigest() != "eb105de93f7db657a76416cfd056b928de66de017a9414f371abccffededb220":
        raise ValueError("Startup USB descriptors differ from the qualified keyboard and mouse")

    image = image_path.read_bytes()
    # esptool's elf2image places the application descriptor's ELF hash at 0xb0 on the ESP32-S3.
    if image[0xb0:0xd0] != hashlib.sha256(elf_path.read_bytes()).digest():
        raise ValueError("Application binary and ELF do not belong to the same build")
    if len(image) > 4 * 1024 * 1024:
        raise ValueError("Application binary exceeds the 7B OTA slot")
    firmware = LoadFirmwareImage("esp32s3", image)
    if not firmware.append_digest or firmware.calc_digest != firmware.stored_digest:
        raise ValueError("Application payload digest mismatch")
    print(f"Combined media and SSH image: {len(image)} bytes, SHA-256 {hashlib.sha256(image).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--interfaces", type=int, choices=(2, 3, 4), required=True)
    parser.add_argument("--media-ssh-image", type=Path, metavar="BIN",
                        help="also check the qualified USB bytes, linked media and SSH, and the BIN's ELF hash")
    args = parser.parse_args()
    if args.media_ssh_image and args.interfaces != 2:
        parser.error("the media and SSH profile requires --interfaces 2")
    with open(args.elf, "rb") as fp:
        elf = ELFFile(fp)
        verify(elf, args.interfaces)
        if args.media_ssh_image:
            try:
                verify_media_ssh_image(elf, args.elf, args.media_ssh_image)
            except ValueError as exc:
                parser.exit(1, f"FAIL: {exc}\n")
    print(f"PASS {args.interfaces} interfaces, endpoint map, descriptor lengths and report layouts")
