# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build the LGPL FFmpeg decoder from the release source ZIP or the original handoff."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

ARCHIVE_SHA = "670069f52aacb43b683d349adbae88ca51be26924b391b7c3be786eec1438730"
HANDOFF = "KVMpad_H264_complete/"
TARBALL = "upstream/ffmpeg-6.1.1.tar.xz"
OVERLAYS = "source-overlays/v2/libavcodec/"
MODEL = "portable/model_arrays.h"
DECODER_INPUTS = {
    TARBALL: "8684f4b00f94b85461884c3719382f1261f0d9eb3d59640a1f4ac0873616f968",
    MODEL: "138b9454260c63cfa5ce4edda11bd481af0c32255a2e9487436a39699e88331b",
    OVERLAYS + "h264_picture.c": "9f0077c3a2e271c25afa7c8bab865c6d3296cc19639a7b08c41de89fe1d68e92",
    OVERLAYS + "h264_slice.c": "ed28514af96cf6f48a8a1233523def893f26ce458e60d17c0549a4791b126006",
    OVERLAYS + "h264dec.c": "ddea68576041350691ee67ca4b76d6d3c2fc9665f57cbdd6552a7a9bd707f77d",
    OVERLAYS + "kvm_rr.h": "6a5edb809f9812c6247aed45e98ac6064a1cb287aa15758284963ccbd55022a2",
    OVERLAYS + "kvm_rr_hint.h": "f1cfef70b453337f7d925e06a671b77025d9ac64c30f5551e037c65cbf945716",
}


def decoder_inputs(path):
    with zipfile.ZipFile(path) as archive:
        prefix = "" if TARBALL in archive.namelist() else HANDOFF
        if prefix:
            with path.open("rb") as stream:
                if hashlib.file_digest(stream, "sha256").hexdigest() != ARCHIVE_SHA:
                    raise ValueError("unexpected handoff SHA-256")
        files = {}
        for name, digest in DECODER_INPUTS.items():
            data = archive.read(prefix + name)
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError(f"unexpected SHA-256 for {name}")
            files[name] = data
        return files


def stage_reconstruction(files, src):
    """Adapt the supplied v2 reconstruction hooks to the single decoder owner and flash weights."""
    directory = src / "libavcodec"
    for name in ("h264_picture.c", "h264_slice.c", "h264dec.c", "kvm_rr.h", "kvm_rr_hint.h"):
        code = files[OVERLAYS + name].decode()
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
    (directory / "model_arrays.h").write_bytes(files[MODEL])
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


def stage_source(archive, src):
    files = decoder_inputs(archive)
    # The digest authenticates the complete upstream tar, including names and file modes.
    with tarfile.open(fileobj=io.BytesIO(files[TARBALL]), mode="r|xz") as upstream:
        for member in upstream:
            if member.isfile():
                path = src / Path(member.name).relative_to("ffmpeg-6.1.1")
                path.parent.mkdir(parents=True, exist_ok=True)
                contents = upstream.extractfile(member).read()
                if not path.exists() or path.read_bytes() != contents:
                    path.write_bytes(contents)
                if os.name != "nt" and member.mode & 0o111:
                    path.chmod(member.mode & 0o777)
    stage_reconstruction(files, src)


def build(args):
    root = args.out.resolve()
    src = args.source.resolve() if args.source else root / "ffmpeg"
    if args.archive:
        stage_source(args.archive, src)
    if args.prepare_only:
        print(src)
        return
    dest = root / args.target
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
    parser.add_argument("archive", type=Path, nargs="?", help="kvmpad-decoder-source.zip or original handoff")
    parser.add_argument("--prepare-only", action="store_true", help="stage the matching editable decoder source")
    parser.add_argument("--source", type=Path, help="build an already staged tree, preserving local changes")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=["xtensa", "host"])
    parser.add_argument("--specs", type=Path, help="use existing ESP-IDF picolibc.specs for Xtensa")
    parser.add_argument("--shell", default="sh")
    parser.add_argument("--make", default="make")
    parser.add_argument("--jobs", default=8, type=int)
    args = parser.parse_args()
    if bool(args.archive) == bool(args.source) or (args.prepare_only and not args.archive):
        parser.error("use either a source ZIP (optionally --prepare-only) or --source DIRECTORY")
    build(args)
