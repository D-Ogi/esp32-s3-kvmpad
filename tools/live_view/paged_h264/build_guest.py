# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build independent code pages from the staged handoff FFmpeg v2 sources."""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

HERE = Path(__file__).resolve().parent


def pack(elf, direct=()):
    phoff, = struct.unpack_from("<I", elf, 28)
    entries, = struct.unpack_from("<H", elf, 44)
    entry, = struct.unpack_from("<I", elf, 24)
    segments = {}
    for i in range(entries):
        typ, offset, addr, _, size, _, flags, _ = struct.unpack_from("<8I", elf, phoff + 32 * i)
        if typ == 1:
            segments[addr, flags] = elf[offset:offset + size]
    rom = segments[0x10000, 5]
    data = segments[0x2000000, 6]
    block = 8192
    chunks = [(rom[p:p + block] if p // block in direct else zlib.compress(rom[p:p + block], 9))
              for p in range(0, len(rom), block)]
    pos = 24 + 4 * (len(chunks) + 1)
    offsets = [pos]
    for page, chunk in enumerate(chunks):
        if page in direct:
            offsets[-1] |= 1 << 31
        pos += len(chunk)
        offsets.append(pos)
    header = struct.pack("<4s5I", b"KVR1", len(rom), entry, len(data), len(chunks), block)
    return header + struct.pack(f"<{len(offsets)}I", *offsets) + b"".join(chunks) + data


def build(src, out, sh, make, jobs, direct):
    src = src.resolve()
    if "kvm_media_rr_mode" not in (src / "libavcodec/h264dec.c").read_text():
        raise ValueError("Expected staged handoff v2 source from build_ffmpeg.py")
    out.mkdir(exist_ok=True, parents=True)
    out = out.resolve()
    flags = ["-march=rv32i_zicsr_zifencei", "-mabi=ilp32", "-ffunction-sections", "-fdata-sections"]
    sh = Path(shutil.which(sh) or sh).resolve().as_posix()
    if not (out / "ffbuild/config.mak").is_file():
        subprocess.run([sh, (src / "configure").as_posix(), "--disable-all", "--enable-avcodec",
                        "--enable-decoder=h264", "--enable-parser=h264", "--disable-asm",
                        "--disable-network", "--disable-autodetect", "--disable-pthreads",
                        "--disable-debug", "--enable-small", "--enable-cross-compile",
                        "--cross-prefix=riscv32-esp-elf-", "--target-os=none", "--arch=riscv32",
                        "--extra-cflags=" + " ".join(flags),
                        "--extra-ldflags=" + " ".join(flags[:2] + ["-nostartfiles", "-Wl,-e,main", "-specs=nosys.specs"])],
                       cwd=out, check=True, env={**os.environ, "TMPDIR": out.as_posix()})
    libs = ["libavcodec/libavcodec.a", "libavutil/libavutil.a"]
    subprocess.run([make, f"-j{jobs}", f"SHELL={sh}", *libs], cwd=out, check=True)
    subprocess.run(["riscv32-esp-elf-gcc", *flags, "-std=gnu11", "-O2", "-I.", "-I" + src.as_posix(),
                    HERE / "guest.c", "-nostartfiles", "-Wl,--gc-sections,--no-relax", "-Wl,-T," + (HERE / "guest.ld").as_posix(),
                    *libs, "-lm", "-lc", "-lgcc", "-lnosys", "-o", "decoder.elf"], cwd=out, check=True)
    image = pack((out / "decoder.elf").read_bytes(), direct)
    (out / "decoder.kvr").write_bytes(image)
    print(f"{len(image)} packed decoder bytes in {out / 'decoder.kvr'}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--shell", default="sh")
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--direct-pages", type=int, nargs="*", default=[],
                        help="Store selected frequent ROM pages literally in flash to save inflate calls")
    args = parser.parse_args()
    build(args.src, args.out, args.shell, args.make, args.jobs, args.direct_pages)
