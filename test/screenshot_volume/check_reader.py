# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run the public screenshot reader on the USB FAT16 image produced by run.py.

Supply --ovmf pointing to its ovmf-* output directory (usb.img and seed/SOURCE.BMP).
Only fresh copies in --out are written. The production reader always opens them read-only.
"""
import argparse
from fractions import Fraction
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def make_bmp(w, h, bits):
    n = bits // 8
    stride = (w * n + 3) & ~3
    body = bytearray([0xD7]) * (stride * abs(h))
    for i in range(abs(h)):
        y = i if h < 0 else h - 1 - i
        for x in range(w):
            offset = i * stride + x * n
            body[offset:offset + 3] = bytes(((x + y * 29) & 255, (x * 3 ^ y * 17) & 255, (x * 13 + y) & 255))
            if n == 4:
                body[offset + 3] = (x + y) & 255
    start = 70
    return (b"BM" + struct.pack("<I4xI", start + len(body), start)
            + struct.pack("<IiiHHIIiiII", 40, w, h, 1, bits, 0, len(body), 0, 0, 0, 0)
            + b"\xaf" * (start - 54) + body)


def rgb565(blob):
    w, signed_h = struct.unpack_from("<ii", blob, 18)
    h = abs(signed_h)
    pixel_bytes = struct.unpack_from("<H", blob, 28)[0] // 8
    stride = (w * pixel_bytes + 3) & ~3
    start = struct.unpack_from("<I", blob, 10)[0]
    scale = min(Fraction(1), Fraction(960, w), Fraction(432, h))
    dw, dh = max(1, int(w * scale)), max(1, int(h * scale))
    result = bytearray(2 * dw * dh)
    for y in range(dh):
        sy = y * h // dh
        base = start + (sy if signed_h < 0 else h - 1 - sy) * stride
        for x in range(dw):
            i = base + (x * w // dw) * pixel_bytes
            blue, green, red = blob[i:i + 3]
            c = (red // 8) * 2048 + (green // 4) * 32 + blue // 8
            struct.pack_into("<H", result, (y * dw + x) * 2, c)
    return dw, dh, result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", required=True)
    for name in ("out", "fatfs", "sdkconfig", "ovmf"):
        p.add_argument("--" + name, required=True, type=Path)
    a = p.parse_args()
    a.out.mkdir(exist_ok=True, parents=True)
    work = Path(tempfile.mkdtemp(prefix="reader-", dir=a.out.resolve()))
    print("Reader evidence:", work, flush=True)
    test = Path(__file__).resolve().parent
    repo = test.parents[1]
    volume = repo / "components/screenshot_volume"
    fat = a.fatfs.resolve()
    disk = fat.parent / "diskio"
    platform = work / "platform"
    (platform / "freertos").mkdir(parents=True)
    (platform / "freertos/FreeRTOS.h").write_text(
        "#include <stddef.h>\n#define MIN(a,b) ((a)<(b)?(a):(b))\n#define MAX(a,b) ((a)>(b)?(a):(b))\n")
    (platform / "freertos/semphr.h").touch()
    shutil.copyfile(a.sdkconfig, platform / "sdkconfig.h")
    (platform / "esp_err.h").write_text(
        "#pragma once\n#include <stdbool.h>\ntypedef int esp_err_t;\n"
        "enum {ESP_OK, ESP_FAIL, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_SIZE, ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE};\n")
    (platform / "esp_heap_caps.h").write_text(
        "#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
        "void *heap_caps_malloc(size_t, unsigned);\nvoid heap_caps_free(void *);\n")
    (platform / "esp_random.h").write_text("#include <stdint.h>\nuint32_t esp_random(void);\n")
    (platform / "compat.h").write_text(
        "#include <assert.h>\n#include <time.h>\n#ifdef _WIN32\n#include <direct.h>\n#include <sys/stat.h>\n"
        "#define mkdir(path,mode) _mkdir(path)\n#define localtime_r(t,b) (localtime_s((b),(t)) ? NULL : (b))\n#endif\n")
    exe = work / "reader.exe"
    flags = ["-std=gnu11", "-O2", "-include", platform / "compat.h",
             "-I", platform, "-I", fat, "-I", disk, "-I", repo / "main", "-I", volume / "include",
             "-I", repo / "components/pack_runtime/include"]
    # IDF namespaces these five FatFs entry points. Link its real dispatcher,
    # including drive slot assignment, registration and VolToPart.
    flags += [f"-Ddisk_{f}=ff_disk_{f}" for f in ("initialize", "status", "read", "write", "ioctl")]
    subprocess.run([a.cc, *flags, repo / "main/preboot_capture.c", volume / "screenshot_volume.c",
                    volume / "screenshot_volume_file.c", disk / "diskio.c", fat / "ff.c", fat / "ffunicode.c",
                    test / "reader.c", "-o", exe], check=True)
    archive = a.ovmf / "usb.img"
    reference = (a.ovmf / "seed/SOURCE.BMP").read_bytes()
    copy = work / "private.img"
    shutil.copyfile(archive, copy)
    seed = work / "input.bmp"
    expected = work / "expected.rgb565"

    def check(name, blob, error=0, original=False):
        image = archive if original else copy
        if not original:
            seed.write_bytes(blob)
            subprocess.run([exe, "replace", image, seed], check=True)
        cmd = [exe, "read", image, str(error)]
        detail = f"expected error {error}"
        if not error:
            dw, dh, pixels = rgb565(blob)
            expected.write_bytes(pixels)
            cmd += [str(dw), str(dh), expected]
            detail = f"{dw}x{dh} {dw * dh} identical RGB565 pixels sha256={hashlib.sha256(pixels).hexdigest()}"
        subprocess.run(cmd, check=True)
        print(f"PASS {name}: {detail}; read twice and released drive 1", flush=True)

    check("original OVMF bottom-up BGR24", reference, original=True)
    check("top-down BGRA32 tall", make_bmp(509, -735, 32))
    check("bottom-up BGR24 width-limited, 3 padding bytes", make_bmp(1295, 377, 24))
    check("top-down BGR24, 1 padding byte", make_bmp(13, -7, 24))
    check("height-limited within one scaled scanline of both bounds", make_bmp(1920, -865, 32))
    check("bottom-up single row", make_bmp(1921, 1, 24))
    check("top-down single column", make_bmp(1, -721, 32))
    good = make_bmp(13, -7, 32)
    check("truncated fixed header", good[:31], 1)
    check("truncated final scanline", good[:-1], 3)
    for name, offset, fmt, value in [("non-BMP", 0, "2s", b"ZZ"), ("compressed BMP", 30, "I", 1),
                                     ("unsupported pixel format", 28, "H", 16), ("zero height", 22, "i", 0),
                                     ("excessive width", 18, "i", 8193), ("offset inside DIB", 10, "I", 28)]:
        invalid = bytearray(good)
        struct.pack_into("<" + fmt, invalid, offset, value)
        check(name, invalid, 2)
    check("remount after rejected BMP", good)


if __name__ == "__main__":
    main()
