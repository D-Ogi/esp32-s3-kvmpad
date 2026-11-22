# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Prepare the handoff's LGPL FFmpeg decoder outside the source tree. No downloads."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ARCHIVE_SHA = "670069f52aacb43b683d349adbae88ca51be26924b391b7c3be786eec1438730"
PREFIX = "KVMpad_H264_complete/upstream/ffmpeg-6.1.1/"


def stage_reconstruction(archive, src):
    """Adapt the supplied v2 reconstruction hooks to the single decoder owner and flash weights."""
    directory = src / "libavcodec"
    root = "KVMpad_H264_complete/"
    for name in ("h264_picture.c", "h264_slice.c", "h264dec.c", "kvm_rr.h", "kvm_rr_hint.h"):
        code = archive.read(root + "source-overlays/v2/libavcodec/" + name).decode()
        if 'getenv("KVM_RR")' in code:
            declaration = 'extern const char *kvm_media_rr_mode(void);\n'
            code = code.replace('#include ', declaration + '#include ', 1)
            code = code.replace('getenv("KVM_RR")', 'kvm_media_rr_mode()')
        if name == "kvm_rr.h":
            start = code.index("static int16_t rr_kernels")
            end = code.index("static inline const int16_t *rr_filter", start)
            code = code[:start] + '#include "model_arrays.h"\n' + code[end:]
            code = code.replace("rr_kernels[range>=16][phase]", "kv_small_weights+16*(64*(range>=16)+phase)")
            code = code.replace("rr_large_kernels[expert][phase]", "kv_large_weights+16*(64*expert+phase)")
        if name == "h264_slice.c":
            code = code.replace("if(bank && rr_load_bank(bank==2)<0)return AVERROR_INVALIDDATA;", "")
        (directory / name).write_bytes(code.encode())
    (directory / "model_arrays.h").write_bytes(archive.read(root + "portable/model_arrays.h"))
    patch = Path(__file__).resolve().with_name("ffmpeg-workspace.patch")
    subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=src, check=True)


def xtensa_specs(override, output):
    if override is not None:
        return override.resolve(strict=True)
    # ESP-IDF 6.1 uses the toolchain specs with --gc-sections removed.
    # Derive the same file before the firmware's first configure.
    path = subprocess.check_output(["xtensa-esp32s3-elf-gcc", "--print-file-name=picolibc.specs"],
                                   text=True).strip()
    original = Path(path).resolve(strict=True)
    output.write_text(original.read_text(encoding="ascii").replace("--gc-sections", ""), encoding="ascii")
    return output


def build(args):
    with args.archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != ARCHIVE_SHA:
            raise ValueError("unexpected handoff SHA-256")
    root = args.out.resolve()
    src = root / "ffmpeg"
    dest = root / args.target
    with zipfile.ZipFile(args.archive) as archive:
        for info in archive.infolist():
            if info.filename.startswith(PREFIX) and not info.is_dir():
                path = src / info.filename[len(PREFIX):]
                path.parent.mkdir(parents=True, exist_ok=True)
                contents = archive.read(info)
                if not path.exists() or path.read_bytes() != contents:
                    path.write_bytes(contents)
        stage_reconstruction(archive, src)
    dest.mkdir(parents=True, exist_ok=True)
    sh = Path(shutil.which(args.shell) or args.shell).resolve().as_posix()
    common = ["--disable-all", "--enable-avcodec", "--enable-avformat", "--enable-decoder=h264",
              "--enable-parser=h264", "--enable-demuxer=mov,h264", "--disable-asm", "--disable-network",
              "--disable-autodetect", "--disable-pthreads", "--disable-debug", "--enable-small"]
    if args.target == "xtensa":
        specs = xtensa_specs(args.specs, dest / "picolibc.specs")
        opts = ["--enable-cross-compile", "--cross-prefix=xtensa-esp32s3-elf-", "--target-os=none",
                "--arch=xtensa", "--malloc-prefix=kvm_av_",
                "--extra-cflags=-mlongcalls -mdisable-hardware-atomics -ffunction-sections -fdata-sections "
                + "-specs=" + specs.as_posix(),
                "--extra-ldflags=-nostartfiles -Wl,-e,main -specs=nosys.specs"]
    elif sys.platform == "win32":
        opts = ["--enable-cross-compile", "--arch=x86_64", "--target-os=mingw32", "--cc=clang",
                "--ar=llvm-ar", "--nm=llvm-nm", "--ranlib=llvm-ranlib", "--disable-w32threads"]
    else:
        opts = []
    env = {**os.environ, "TMPDIR": root.as_posix()}
    subprocess.run([sh, (src / "configure").as_posix(), *common, *opts], cwd=dest, env=env, check=True)
    subprocess.run([args.make, "-j" + str(args.jobs), "SHELL=" + sh, "libavformat/libavformat.a",
                    "libavcodec/libavcodec.a", "libavutil/libavutil.a"], cwd=dest, env=env, check=True)
    print(dest)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=["xtensa", "host"])
    parser.add_argument("--specs", type=Path, help="use existing ESP-IDF picolibc.specs for Xtensa")
    parser.add_argument("--shell", default="sh")
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", default=8, type=int)
    build(parser.parse_args())
