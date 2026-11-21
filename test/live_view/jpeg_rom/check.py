# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run the firmware's JPEG path and local ESP32-S3 rev0 mask-ROM bytes on a QEMU Xtensa CPU."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import time
from PIL import Image

HERE = Path(__file__).resolve().parent
MEDIA = HERE.parents[2] / "components/live_view"
REV0_SHA256 = "c0ce0f338d1de1bdc6efbef1591779a2a42c1ab7d759d3c6ae8ae63a7dd34cfd"
BLACK_FIXTURES = ("black", "near-black")
FIT_MODES = ((240, 240), (280, 240), (320, 240), (480, 272), (480, 320), (480, 480),
             (640, 480), (800, 480), (800, 600), (960, 960), (1024, 600), (1024, 768),
             (1280, 600), (1280, 720), (1280, 800), (1600, 1200), (1920, 1080))


def fit_inputs(work):
    # Switch geometry on the same canvas/decoder for every case. Add an all-black
    # frame and one with a nonblack source pixel omitted by nearest-neighbor shrink.
    cases = [(*mode, "color") for mode in FIT_MODES] + [(480, 480, "black"), (1920, 1080, "last-pixel")]
    asm = ['.section .data.fit_inputs,"aw"']
    header = ["typedef struct { unsigned w,h; const uint8_t *start,*end; bool visible_color; } fixture_t;"]
    records = []
    tile = Image.frombytes("RGB", (53, 49), bytes((x*7 + y*13 + c*73) % 256
                           for y in range(49) for x in range(53) for c in range(3)))
    for i, (w, h, kind) in enumerate(cases):
        name = f"fit_{i}"
        path = work / (name + ".jpg")
        im = Image.new("RGB", (w, h))
        if kind == "color":
            im.paste((250, 30, 12), (0, 0, w//2, h//2))
            im.paste((1, 233, 42), (w//2, 0, w, h//2))
            im.paste((8, 13, 239), (0, h//2, w//2, h))
            im.paste((244, 243, 244), (w//2, h//2, w, h))
            for x, y in ((0, 0), (11, 81), (w//2-21, h//2-19), (w-53, h-49)):
                im.paste(tile, (x, y))
        elif kind == "last-pixel":
            im.putpixel((w-1, h-1), (7, 7, 7))
        im.save(path, quality=100 if kind == "last-pixel" else 82, subsampling=2 if kind != "color" else i % 3)
        assert path.stat().st_size <= 96 * 1024
        asm += [f".global {name},{name}_end", f"{name}:", f'.incbin "{path.name}"', f"{name}_end:"]
        header += [f"extern const uint8_t {name}[], {name}_end[];"]
        records += [f"{{{w},{h},{name},{name}_end,{'true' if kind == 'color' else 'false'}}}"]
    header += ["static const fixture_t fixtures[] = {" + ",\n".join(records) + "};"]
    (work / "fit_inputs.h").write_text("\n".join(header))
    return "\n".join(asm) + "\n", cases


def chart(directory, subsampling, width=512, height=512, fixture="chart"):
    # Original synthetic chart, freely reproducible. Constant 128x256 cells
    # line up with MCU boundaries. Final two YCbCr triplets require clipping.
    colors = [
        (76, 85, 255),
        (149, 43, 21),
        (29, 255, 107),
        (255, 128, 128),
        (0, 128, 128),
        (226, 0, 149),
        (0, 0, 255),
        (255, 255, 0),
    ]
    cell_w, cell_h = width // 4, height // 2
    im = Image.new("YCbCr", (width, height))
    for i, color in enumerate(colors):
        x, y = (i % 4) * cell_w, (i // 4) * cell_h
        im.paste(color, (x, y, x + cell_w, y + cell_h))
    if fixture in (*BLACK_FIXTURES, "last-pixel"):
        im.paste((0, 128, 128), (0, 0, width, height))
        if fixture == "near-black":
            # Nonuniform RGB888 data, all below the first RGB565 green level.
            ramp = bytes((x // 4) % 3 for x in range(width)) * height
            im = Image.merge("YCbCr", (Image.frombytes("L", im.size, ramp), *im.split()[1:]))
        elif fixture == "last-pixel":
            im.putpixel((width - 1, height - 1), (7, 128, 128))
    elif fixture != "chart":
        first = height // 2 if fixture == "mixed" else 0
        detail = bytes(16 + ((3 * x + 2 * y + ((x ^ y) & 31)) % 224)
                       for y in range(first, height) for x in range(width))
        luma = Image.frombytes("L", (width, height - first), detail)
        if fixture == "color-detail":
            cb = Image.frombytes("L", luma.size, bytes(96 + (x // 2 + y) % 64
                                 for y in range(height) for x in range(width)))
            cr = Image.frombytes("L", luma.size, bytes(96 + (x + y // 2) % 64
                                 for y in range(height) for x in range(width)))
        else:
            cb = cr = Image.new("L", luma.size, 128)
        im.paste(Image.merge("YCbCr", (luma, cb, cr)), (0, first))
    path = directory / "chart.jpg"
    im.save(path, quality=65 if fixture in ("mixed", "color-detail", "detail") else 100, subsampling=subsampling,
            optimize=False, progressive=False)
    with Image.open(path) as decoded:
        rgb = decoded.convert("RGB")
    if fixture == "near-black":
        assert rgb.getextrema() == ((0, 2),) * 3
    return [rgb.getpixel(((i % 4) * cell_w + cell_w // 2, (i // 4) * cell_h + cell_h // 2)) for i in range(8)]


def rom_segments(elf, directory):
    # Load the text and data ROM windows at their *target virtual addresses*.
    # No substitution, translation of the JPEG algorithm or SoC boot code.
    data = elf.read_bytes()
    if hashlib.sha256(data).hexdigest() != REV0_SHA256:
        raise ValueError(
            "Expected the pinned ESP32-S3 rev0 ROM from esp-rom-elfs 20241011"
        )
    if data[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", data, 18)[0] != 94:
        raise ValueError("Expected ELF32 little-endian Xtensa ROM")
    phoff = struct.unpack_from("<I", data, 28)[0]
    size, count = struct.unpack_from("<HH", data, 42)
    loaders = []
    for base, end in ((0x40000000, 0x40060000), (0x3FF00000, 0x3FF20000)):
        chunk = bytearray(end - base)
        loaded = 0
        for i in range(count):
            kind, off, va, _, length, _, _, _ = struct.unpack_from(
                "<8I", data, phoff + i * size
            )
            if kind == 1 and length and base <= va and va + length <= end:
                chunk[va - base : va - base + length] = data[off : off + length]
                loaded += length
        if not loaded:
            raise ValueError("ROM image is missing a loadable window")
        part = directory / f"rom-{base:x}.bin"
        part.write_bytes(chunk)
        loaders += ["-device", f"loader,file={part.name},addr=0x{base:x},force-raw=on"]
    return loaders


def run(args):
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    fit = getattr(args, "usb_fit", False)
    width, height = (640, 480) if args.usb_monitor or args.usb_frame else (512, 512)
    phase = getattr(args, "magnifier_phase", None)
    expected_pixels = None
    if fit:
        if phase is not None or args.usb_frame or args.usb_monitor or args.fixture != "chart":
            raise ValueError("--usb-fit has its own fixtures")
    elif phase is not None:
        if args.usb_frame or args.usb_monitor or args.fixture != "chart" or args.subsampling != 2:
            raise ValueError("The magnifier uses its own 512x512 4:2:0 fixtures.")
        sys.path.insert(0, str(HERE.parent))
        from generate_synthetic_jpeg import picture
        _, expected_pixels = picture(phase)
        header = (MEDIA.parents[1] / "main/media_synthetic_jpeg.h").read_text()
        encoded = re.search(rf"synthetic_jpeg_{phase}\[\] = \{{([^}}]+)\}}", header)[1]
        path = work / "chart.jpg"
        path.write_bytes(bytes(int(v, 16) for v in re.findall("0x([0-9a-f]{2})", encoded)))
        with Image.open(path) as image:
            assert image.size == (width, height)
            rgb = image.convert("RGB")
        colors = [rgb.getpixel(((i % 4) * 128 + 64, (i // 4) * 256 + 128)) for i in range(8)]
    elif args.usb_frame:
        packet = args.usb_frame.read_bytes()
        crc, kind, cmd, x, y, w, h, packed = struct.unpack_from("<HBBHHHHI", packet)
        assert (kind, x, y, w, h, packed >> 10) == (3, 0, 0, width, height, len(packet) - 16)
        path = work / "chart.jpg"
        path.write_bytes(packet[16:])
        with Image.open(path) as src:
            assert src.size == (width, height)
            rgb = src.convert("RGB")
            colors = [rgb.getpixel(((i % 4) * width // 4 + width // 8, (i // 4) * height // 2 + height // 4))
                      for i in range(8)]
    else:
        colors = chart(work, args.subsampling, width, height, args.fixture)
    source = args.decoder_source.resolve() if args.decoder_source else MEDIA / "media_decode.c"
    loaders = rom_segments(args.rom, work)
    asm = '.section .data.chart,"aw"\n.global chart,chart_end\nchart:\n.incbin "chart.jpg"\nchart_end:\n'
    if fit:
        asm, cases = fit_inputs(work)
    check_monitor_color = (args.usb_monitor or args.usb_frame) and not args.decoder_source
    if check_monitor_color:
        level = 255 if args.fixture in BLACK_FIXTURES else 0
        Image.new("RGB", (width, height), (level,) * 3).save(work / "solid.jpg", quality=100)
        asm += '.global solid,solid_end\nsolid:\n.incbin "solid.jpg"\nsolid_end:\n'
    (work / "chart.S").write_text(asm)
    (work / "esp_heap_caps.h").write_text(
        "#include <stddef.h>\n#define MALLOC_CAP_8BIT 1\n#define MALLOC_CAP_SPIRAM 2\nvoid *heap_caps_malloc(size_t, unsigned);\n"
    )
    (work / "image.ld").write_text("""
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x40100000;
  .text : { *(.literal*) *(.text.start) *(.text*) } :code
  .rodata : { *(.rodata*) } :code
  .data ALIGN(0x1000) : { *(.data*) } :data
  .bss : { *(.bss*) *(COMMON) } :data
}
jd_prepare = 0x40000858;
jd_decomp = 0x40000864;
memcpy = 0x400011f4;
memset = 0x400011e8;
memcmp = 0x4000120c;
""")
    prefix = str(args.toolchain.resolve() / "xtensa-esp32s3-elf-")
    decoder = source
    if args.firmware_build:
        build = args.firmware_build.resolve()
        database = json.loads((build / "compile_commands.json").read_text())
        entry = next(e for e in database if Path(e["file"]).resolve() == (MEDIA / "media_decode.c").resolve())
        flags = entry["command"].rsplit(" -o ", 1)[0]
        decoder = work / "media_decode.o"
        output = ["-o", str(decoder), "-c", str(source)]
        command = flags + " " + subprocess.list2cmdline(output) if os.name == "nt" else shlex.split(flags) + output
        subprocess.run(command, cwd=entry["directory"], check=True)
    subprocess.run(
        [
            prefix + "gcc",
            "-mtext-section-literals",
            "-mlongcalls",
            "-Os",
            "-fno-builtin",
            "-ffunction-sections",
            "-fdata-sections",
            "-DESP_PLATFORM",
            *(["-DUSB_FIT_TEST"] if fit else []),
            *(["-DTEST_FIRMWARE_OBJECT"] if args.firmware_build else []),
            *(["-DUSB_MONITOR_TEST"] if args.usb_monitor or args.usb_frame else []),
            *(["-DCHECK_MONITOR_COLOR"] if check_monitor_color else []),
            "-I.",
            "-I",
            MEDIA / "include",
            "-I",
            args.idf.resolve() / "components/esp_rom/esp32s3/include",
            "-nostdlib",
            "-Wl,--gc-sections",
            "-T",
            "image.ld",
            HERE / "start.S",
            HERE / "chart.c",
            "chart.S",
            decoder,
            MEDIA / "cinepak_tile.c",
            "-lc",
            "-lgcc",
            "-o",
            "chart.elf",
        ],
        cwd=work,
        check=True,
    )
    symbols = subprocess.check_output(
        [prefix + "nm", "-n", "chart.elf"], cwd=work, text=True
    )
    result_symbol = "fit_result" if fit else "result"
    addr = int(re.search(rf"^(\w+) [Bb] {result_symbol}$", symbols, re.M)[1], 16)
    frame_addr = int(re.search(r"^(\w+) [Bb] frame$", symbols, re.M)[1], 16)
    qemu = [
        args.qemu,
        "-M",
        "sim",
        "-cpu",
        args.cpu,
        "-m",
        "1040M" if fit else "1030M",
        "-display",
        "none",
        "-serial",
        "none",
        "-nic",
        "none",
        "-qmp",
        "stdio",
        "-icount",
        "shift=0,align=off,sleep=off",
        "-S",
        "-d",
        "in_asm",
        "-D",
        "rom-trace.log",
        *loaders,
        "-device",
        "loader,file=chart.elf,cpu-num=0",
    ]
    with subprocess.Popen(
        qemu,
        cwd=work,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ) as proc:

        def req(op, **kwargs):
            cmd = {"execute": op, "arguments": kwargs}
            proc.stdin.write((json.dumps(cmd) + "\n").encode())
            proc.stdin.flush()
            while line := proc.stdout.readline():
                resp = json.loads(line)
                if "return" in resp:
                    return resp["return"]
                if "error" in resp:
                    raise RuntimeError(resp)
            raise RuntimeError(proc.stderr.read().decode(errors="replace"))

        try:
            hello = json.loads(proc.stdout.readline())
            req("qmp_capabilities")
            req("cont")
            deadline = time.monotonic() + (120 if fit else 8)
            while time.monotonic() < deadline:
                dump = req(
                    "human-monitor-command", **{"command-line": f"xp /27wx 0x{addr:x}"}
                )
                words = [
                    int(v, 16)
                    for line in dump.splitlines()
                    for v in re.findall(r"0x[0-9a-fA-F]+", line.partition(":")[2])
                ]
                if words and words[0] == 0x4A504547:
                    break
                time.sleep(0.05)
            else:
                req("stop")
                regs = req(
                    "human-monitor-command", **{"command-line": "info registers"}
                )
                raise RuntimeError(f"Results {words}; {regs}")
            if not fit:
                req("pmemsave", val=frame_addr, size=(64 + height * (width + 7)) * 2, filename="frame.rgb565")
        finally:
            proc.kill()
    if fit:
        if words[1] != len(cases) or words[3] != 1:
            raise AssertionError(f"Fit case {cases[words[2]]}, results {words}")
        print(json.dumps({"cpu": args.cpu, "mask_rom_sha256": REV0_SHA256,
                          "decoder_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                          "source_modes": FIT_MODES, "cases_with_transitions": len(cases),
                          "additional_cases": cases[len(FIT_MODES):],
                          "canvas_pixels_verified_per_case": 1024*600, "guards_per_case": 600*7+64,
                          "metadata_and_buffer_bounds_rejected": True, "cancellation_and_reuse": True},
                         indent=2))
        return
    if words[1] != 1 or any(words[11:16]) or words[16] != 1 or (args.fixture == "chart" and words[10]):
        raise AssertionError(words)
    assert words[17] > 0 and words[18:21] == [1, 1, 128], words
    raw = (work / "frame.rgb565").read_bytes()
    if args.reference_frame and raw != args.reference_frame.read_bytes():
        raise AssertionError("RGB565 frame or guard words differ from the reference decode")
    visible = b"".join(raw[(32 + y * (width + 7)) * 2:(32 + y * (width + 7) + width) * 2] for y in range(height))
    if args.fixture in BLACK_FIXTURES:
        assert not any(visible), "Expected every RGB565 pixel to be zero"
    elif args.fixture == "last-pixel":
        assert not any(visible[:-2]) and visible[-2:] == b"\x20\x00", "Expected only green bit 0 at the last pixel"
    if check_monitor_color:
        nonblack = any(visible)
        assert words[22:27] == [nonblack, 1, not nonblack, not nonblack, nonblack], words
    if expected_pixels is not None and visible != expected_pixels:
        raise AssertionError("Magnifier ROM output differs from the independent RGB565 quadrant raster")
    recovered_hash = 2166136261
    for (pixel,) in struct.iter_unpack("<H", visible):
        recovered_hash = ((recovered_hash ^ pixel) * 16777619) & 0xffffffff
    assert words[21] == recovered_hash, "Pixels after decoder reuse differ from the measured decode"
    for ref, v in zip(colors, words[2:10], strict=True):
        c = (v >> 11, (v >> 5) & 63, v & 31)
        expected = (ref[0] >> 3, ref[1] >> 2, ref[2] >> 3)
        if args.fixture == "chart" and c != expected:
            raise AssertionError((c, expected))
    if args.usb_frame:
        assert words[2:10] == [0xF800] * 4 + [0x001F] * 4
    elif args.fixture == "chart" and phase is None:
        assert words[5] == 0xFFFF and words[6] == 0
        assert words[8] & 0x7FF == 0 and words[9] & 0x7FF == 0x7FF
    trace = (work / "rom-trace.log").read_text(errors="replace")
    addresses = ["40000858", "40045f64", "40000864", "400463b0"]
    if not all(re.search("0x" + address + r"\s*:", trace) for address in addresses):
        raise AssertionError(
            "Missing S3 rev0 JPEG entry points in QEMU execution trace"
        )
    evidence = {
        "qemu": hello["QMP"]["version"],
        "cpu": args.cpu,
        "subsampling": ["4:4:4", "4:2:2", "4:2:0"][args.subsampling],
        "production_media_decode_sha256": hashlib.sha256(
            source.read_bytes()
        ).hexdigest(),
        "mask_rom_sha256": hashlib.sha256(args.rom.read_bytes()).hexdigest(),
        "rom_entries_executed": addresses,
        "jpeg_sha256": hashlib.sha256((work / "chart.jpg").read_bytes()).hexdigest(),
        "oracle_rgb888": colors,
        "actual_rgb565": [f"{v:04x}" for v in words[2:10]],
        "decoded_pixels": width * height,
        "verified_pixels": width * height if args.fixture in ("chart", *BLACK_FIXTURES, "last-pixel") or args.reference_frame else 0,
        "verified_guard_words": 7 * height + 64,
        "missing_eoi_refused": True,
        "short_entropy_scan_refused": True,
        "short_stride_refused": True,
        "decoder_reuse_after_refusal": True,
        "cancelled_during_decode_after_callbacks": words[20],
        "decode_ccount_qemu_icount": words[17],
        "full_rgb565_fnv1a_words": f"{words[21]:08x}",
        "full_rgb565_sha256": hashlib.sha256(visible).hexdigest(),
        "fixture": args.fixture,
        "monitor_nonblack_first_solid_after_failure_recovered": [bool(words[i]) for i in (22, 24, 25, 26)] if check_monitor_color else None,
        "magnifier_phase": phase,
        "reference_frame": str(args.reference_frame) if args.reference_frame else None,
        "firmware_compile_commands": str(args.firmware_build) if args.firmware_build else None,
    }
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ("idf", "toolchain", "rom", "work"):
        p.add_argument("--" + arg, type=Path, required=True)
    p.add_argument("--qemu", default="qemu-system-xtensa")
    p.add_argument("--cpu", default="de233_fpu")
    p.add_argument("--subsampling", type=int, choices=(0, 1, 2), default=2)
    p.add_argument("--usb-monitor", action="store_true", help="test the 640x480 USB display JPEG path")
    p.add_argument("--usb-fit", action="store_true", help="compare all 17 Windows source modes against full ROM decodes")
    p.add_argument("--fixture", choices=("chart", "detail", "mixed", "color-detail", *BLACK_FIXTURES, "last-pixel"), default="chart")
    p.add_argument("--magnifier-phase", type=int, choices=(0, 1), help="test an embedded synthetic magnifier JPEG and its RGB565 geometry")
    p.add_argument("--decoder-source", type=Path, help="compare an earlier implementation on the identical ROM")
    p.add_argument("--reference-frame", type=Path, help="compare every recovered pixel and guard against the saved frame")
    p.add_argument("--firmware-build", type=Path, help="compile the decoder separately with the existing IDF build flags")
    p.add_argument("--usb-frame", type=Path, help="check the exact red/blue diagnostic wire record emitted by check_view.py")
    run(p.parse_args())
