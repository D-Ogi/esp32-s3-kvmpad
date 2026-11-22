# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise the private FAT16 image through OVMF USB mass storage and the pinned IDF FatFs.

Example:
    python run.py --out <workspace-evidence> --cc <i686-mingw-clang> --fatfs <fatfs/src>
                  --sdkconfig <config/sdkconfig.h> --qemu <qemu-system-x86_64>
                  --code <edk2-x86_64-code.fd> --vars <edk2-i386-vars.fd>
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


def bmp():
    width, height = 320, 240
    pixels = bytes((x * 7 + y * 3 + c * 53) & 255
                   for y in range(height) for x in range(width) for c in range(3))
    header = b"BM" + struct.pack("<I4xI", len(pixels) + 54, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    return header + dib + pixels


def fat_copies(image, empty):
    with image.open("rb") as src:
        boot = src.read(512)
        assert boot[510:] == b"\x55\xaa" and boot[54:62] == b"FAT16   "
        assert struct.unpack_from("<HBHBHHBH", boot, 11) == (512, 4, 1, 2, 512, 0, 248, 64)
        assert struct.unpack_from("<II", boot, 28) == (0, 65536)
        table = src.read(64 * 512)
        assert src.read(64 * 512) == table and table[:4] == b"\xf8\xff\xff\xff"
        root = src.read(32 * 512)
        assert root[:12] == b"KVMPADSHOT \x08"
        if empty:
            assert not any(table[4:]) and not any(root[12:]) and not any(src.read())
        else:
            assert b"11221234BMP" in root and any(table[4:])
    print("PASS byte-level BPB, root and identical FAT16 mirrors", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("out", "fatfs", "sdkconfig", "code", "vars"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--cc", required=True)
    p.add_argument("--qemu", default="qemu-system-x86_64")
    a = p.parse_args()
    a.out.mkdir(exist_ok=True, parents=True)
    work = Path(tempfile.mkdtemp(prefix="ovmf-", dir=a.out.resolve()))
    print("Private test files:", work, flush=True)
    test = Path(__file__).resolve().parent
    component = test.parents[1] / "components/screenshot_volume"
    platform = work / "platform"
    rt = platform / "freertos"
    rt.mkdir(parents=True)
    (rt / "FreeRTOS.h").write_text(
        "#include <stddef.h>\n#define MIN(a,b) ((a)<(b)?(a):(b))\n#define MAX(a,b) ((a)>(b)?(a):(b))\n", encoding="ascii")
    (rt / "semphr.h").write_bytes(b"")
    shutil.copyfile(a.sdkconfig, platform / "sdkconfig.h")
    exe = work / "fixture.exe"
    subprocess.run([a.cc, "-O2", "-std=gnu11", "-I", platform, "-I", a.fatfs, "-I", component / "include",
                    component / "screenshot_volume.c", component / "screenshot_volume_file.c", test / "fixture.c",
                    a.fatfs / "ff.c", a.fatfs / "ffunicode.c", "-o", exe], check=True)
    image = work / "usb.img"
    image.touch(exist_ok=False)
    seed = work / "seed"
    seed.mkdir()
    reference = seed / "SOURCE.BMP"
    reference.write_bytes(bmp())
    subprocess.run([exe, "create", image, reference], check=True)
    assert image.stat().st_size == 32 << 20
    fat_copies(image, True)
    # Only this generated two-file directory is mapped read-only to the guest.
    # Depending on the order of the two disks, the USB LUN can map to fs0 or fs1.
    script = [
        "@echo -off", "map -r", "map -v", "if exist fs0:\\SOURCE.BMP then",
        "  cp fs0:\\SOURCE.BMP fs1:\\11221234.BMP", "  ls fs1:\\11221234.BMP", "else",
        "  cp fs1:\\SOURCE.BMP fs0:\\11221234.BMP", "  ls fs0:\\11221234.BMP", "endif", "reset -s",
    ]
    (seed / "startup.nsh").write_bytes(("\r\n".join(script) + "\r\n").encode("ascii"))
    shutil.copyfile(a.vars, work / "vars.fd")
    shutil.copyfile(a.code, work / "code.fd")
    cmd = [
        a.qemu, "-machine", "q35,accel=tcg", "-m", "192", "-nodefaults", "-display", "none",
        "-nic", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
        "-drive", "if=pflash,format=raw,readonly=on,file=code.fd",
        "-drive", "if=pflash,format=raw,file=vars.fd",
        "-device", "qemu-xhci,id=xhci", "-drive", "id=shot,if=none,format=raw,file=usb.img",
        "-device", "usb-storage,bus=xhci.0,drive=shot,removable=on",
        "-drive", "id=seed,if=none,format=raw,readonly=on,file=fat:seed",
        "-device", "virtio-blk-pci,drive=seed",
    ]
    log = work / "ovmf.log"
    with log.open("wb") as f:
        subprocess.run(cmd, cwd=work, stdout=f, stderr=subprocess.STDOUT, timeout=70, check=True)
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", log.read_bytes().decode(errors="replace"))
    assert "UEFI Interactive Shell" in text and "USB(" in text and "11221234.BMP" in text, text[-1500:]
    fat_copies(image, False)
    subprocess.run([exe, "verify", image, reference], check=True)
    print(f"PASS OVMF USB mass-storage BMP sha256={hashlib.sha256(reference.read_bytes()).hexdigest()}", flush=True)


if __name__ == "__main__":
    main()
