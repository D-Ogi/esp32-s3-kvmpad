# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Measure production Xtensa pack_video_read objects on the same KVC1 bytes with a fixed QEMU instruction clock."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
TEST = HERE.parents[1]
ROOT = TEST.parent
sys.path.insert(0, str(TEST / "live_view"))
from jpeg_rom.check import rom_segments


def header_checks(source, data):
    fmt, w, h, mode, count, length, digest, rows = struct.unpack_from("<4sHHIIQ32sI", data)
    with source.open("rb") as original:
        if hashlib.file_digest(original, "sha256").digest() != digest or source.stat().st_size != length:
            raise ValueError("Companion source binding failed")
    if fmt != b"KVC1" or rows != 8 or not (0 < w <= 960 and 0 < h <= 540 and count):
        raise ValueError("Invalid source dimensions")
    pos, crc_hash, pts_hash = 64, 0, 0
    for _ in range(count):
        pts, duration, crc = struct.unpack_from("<QII", data, pos)
        crc_hash = (((crc_hash << 1) | (crc_hash >> 31)) ^ crc) & 0xffffffff
        pts_hash = (((pts_hash << 1) | (pts_hash >> 31)) ^ pts) & 0xffffffff
        pos += 16
        for _ in range(0, h, rows):
            pos += 4 + struct.unpack_from("<H", data, pos)[0]
    if pos != len(data):
        raise ValueError("Companion size differs from its records")
    return count, crc_hash, pts_hash


def measure(args):
    work = args.work.resolve()
    work.mkdir(exist_ok=True, parents=True)
    data = args.companion.read_bytes()
    count, crc, pts = header_checks(args.source, data)
    (work / "clip.bin").write_bytes(data)
    (work / "clip.S").write_text('.section .rodata.clip,"a"\n.global clip,clip_end\n'
                                 'clip: .incbin "clip.bin"\nclip_end:\n')
    (work / "esp_err.h").write_text("typedef int esp_err_t;\n")
    cc = args.compiler.resolve()
    libc = cc.parent.parent / "picolibc/xtensa-esp-elf/lib/esp32s3/libc.a"
    zlib = args.build.resolve() / "esp-idf/espressif__zlib/zlib/libz.a"
    objects = [args.video_object.resolve(), args.port_object.resolve()]
    subprocess.run([
        cc, "-Os", "-mtext-section-literals", "-mlongcalls", "-ffunction-sections", "-fdata-sections",
        "-nostdlib", "-Wl,--gc-sections,--wrap=fread,--wrap=fseek,--wrap=ftell,--wrap=fgetc,--wrap=ferror",
        "-I", work, "-I", ROOT / "components/pack_runtime/include", "-T", HERE / "image.ld",
        TEST / "live_view/jpeg_rom/start.S", HERE / "reader.c", "clip.S", *objects,
        "-Wl,--start-group", zlib, libc, "-lgcc", "-Wl,--end-group", "-o", "reader.elf",
    ], cwd=work, check=True)
    nm = cc.with_name(cc.name.replace("gcc", "nm"))
    symbols = subprocess.check_output([nm, "-n", "reader.elf"], cwd=work, text=True)
    addr = int(re.search(r"^(\w+) B result$", symbols, re.M)[1], 16)
    loaders = rom_segments(args.rom, work)
    cmd = [args.qemu, "-M", "sim", "-cpu", "de233_fpu", "-m", "1030M",
           "-icount", "shift=0,align=off,sleep=off", "-display", "none", "-serial", "none", "-nic", "none",
           "-qmp", "stdio", "-S", *loaders, "-device", "loader,file=reader.elf,cpu-num=0"]
    with subprocess.Popen(cmd, cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as child:
        def request(command, **kwargs):
            child.stdin.write((json.dumps({"execute": command, "arguments": kwargs}) + "\n").encode())
            child.stdin.flush()
            for line in child.stdout:
                message = json.loads(line)
                if "return" in message:
                    return message["return"]
                if "error" in message:
                    raise RuntimeError(message)
            raise RuntimeError("QEMU exited")

        try:
            version = json.loads(child.stdout.readline())["QMP"]["version"]
            request("qmp_capabilities")
            request("cont")
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                dump = request("human-monitor-command", **{"command-line": f"xp /8wx 0x{addr:x}"})
                words = [int(v, 16) for line in dump.splitlines()
                         for v in re.findall(r"0x[0-9a-f]+", line.partition(":")[2])]
                if words and words[0] == 0x1234:
                    break
                time.sleep(0.1)
            else:
                raise TimeoutError("Xtensa reader did not finish")
        finally:
            child.kill()
    if (words[2], words[4], words[5]) != (count, crc, pts) or words[7]:
        raise AssertionError(words)
    return {"qemu": version, "companion": args.companion.name, "frames": count,
            "sha256": hashlib.sha256(data).hexdigest(), "ccount": words[1], "reader_stack_bytes": words[3],
            "reader_heap_bytes": words[6], "zlib_default_allocations": words[7],
            "ordered_crc_fold": f"{crc:08x}", "ordered_pts_fold": f"{pts:08x}"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("compiler", "build", "rom", "work", "source", "companion", "video-object", "port-object"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-xtensa")
    print(json.dumps(measure(parser.parse_args()), indent=2))
