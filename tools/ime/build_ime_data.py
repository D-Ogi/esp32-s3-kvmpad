#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build the read-only `ime_data` partition image (ADR-024 TECH-081).

The image holds the dictionaries of the on-device IME engines and is flashed once with the firmware tooling; it is
never downloaded at run time. Format: components/ime_engine/include/ime_data.h.

Files in this version:
  dict_pinyin.dat     PinyinIME system dictionary, built by the vendored AOSP dictionary builder
                      (components/ime_engine/pinyinime/command) from the vendored raw dictionary
                      (pinyinime/data/rawdict_utf16_65105_freq.txt, valid_utf16.txt).
  font_hanzi_24.bin   with --hanzi-font: the hanzi of the pinyin candidate bar, a KVMFONT1 blob written by
                      tools/fonts/gen_hanzi_font_blob.py (derived from Noto Sans SC, OFL-1.1)
  font_kanji_24.bin   with --kanji-font: the kanji and kana of the Japanese candidate bar, the same kind of blob
                      from the same tool with --set ja (derived from Noto Sans JP, OFL-1.1)
  ofl_noto_cjk.txt    with either font: the OFL text that travels with them
                      (components/ui_theme/fonts/OFL-NotoSansKR.txt, the license of the whole Noto CJK family)
  wnn_ja.bin          the Japanese dictionaries of the vendored OpenWnn engine, extracted from
                      components/ime_engine/openwnn/data/WnnJpnDic.c and packed by
                      tools/ime/build_wnn_blob.py; left out with --no-openwnn

The pinyin dictionary builder writes `size_t` fields as 4 bytes, the layout of the ESP32-S3; it must be compiled for
a 32-bit target. The script checks that with a probe program and refuses a 64-bit compiler. The OpenWnn dictionaries
are plain byte arrays and need no compiler.

Usage:
    python -I tools/ime/build_ime_data.py --cxx <32-bit C++ compiler> --work <scratch dir outside the repo> \\
        --out <ime_data.bin> [--hanzi-font <font_hanzi_24.bin>] [--kanji-font <font_kanji_24.bin>] \\
        [--no-openwnn] [--partition-size 0x7B0000]
    e.g. --cxx ../.tools/llvm-mingw-*/bin/i686-w64-mingw32-clang++ (add --cxx-flag=-static on Windows)

Flash it (a human decision; it resets the board once):
    esptool --chip esp32s3 --port COM3 write-flash 0x820000 <ime_data.bin>
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# `python -I` keeps the script's own directory off sys.path, so add it before importing the sibling.
sys.path.insert(0, HERE)
import build_wnn_blob  # noqa: E402  (needs HERE on sys.path)

PINYIN = os.path.join(REPO, "components", "ime_engine", "pinyinime")
BUILDER_SRCS = ["dictbuilder.cpp", "dictlist.cpp", "dicttrie.cpp", "lpicache.cpp", "mystdlib.cpp", "ngram.cpp",
                "searchutility.cpp", "spellingtable.cpp", "spellingtrie.cpp", "splparser.cpp", "utf16char.cpp",
                "utf16reader.cpp"]
MAGIC = b"KVMIMEDT"
VERSION = 1
ALIGN = 4096
NAME_CAP = 24
MAX_ENTRIES = 8


def run(cmd: list[str], cwd: str | None = None) -> None:
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + r.stdout + r.stderr)
        raise SystemExit(1)


def build_pinyin_dict(cxx: str, flags: list[str], work: str) -> bytes:
    probe_c = os.path.join(work, "probe.cpp")
    with open(probe_c, "w", encoding="ascii") as f:
        f.write("#include <cstddef>\nint main() { return (int)sizeof(std::size_t); }\n")
    probe = os.path.join(work, "probe.exe" if os.name == "nt" else "probe")
    run([cxx, *flags, "-o", probe, probe_c])
    width = subprocess.run([probe]).returncode
    if width != 4:
        print(f"{cxx} builds a {width * 8}-bit size_t; the dictionary needs the 32-bit layout of the ESP32-S3",
              file=sys.stderr)
        raise SystemExit(1)

    exe = os.path.join(work, "pinyin_dictbuilder.exe" if os.name == "nt" else "pinyin_dictbuilder")
    srcs = [os.path.join(PINYIN, "command", "pinyinime_dictbuilder.cpp")]
    srcs += [os.path.join(PINYIN, "share", s) for s in BUILDER_SRCS]
    run([cxx, *flags, "-O2", "-std=c++17", "-w", "-D___BUILD_MODEL___", "-I" + os.path.join(PINYIN, "include"),
         "-o", exe, *srcs])
    # The builder writes to ../../res/raw/dict_pinyin.dat relative to its working directory (upstream layout).
    cwd = os.path.join(work, "run", "x")
    out_dir = os.path.join(work, "res", "raw")
    shutil.rmtree(os.path.join(work, "res"), ignore_errors=True)
    os.makedirs(cwd, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)
    run([exe, os.path.join(PINYIN, "data", "rawdict_utf16_65105_freq.txt"),
         os.path.join(PINYIN, "data", "valid_utf16.txt")], cwd=cwd)
    with open(os.path.join(out_dir, "dict_pinyin.dat"), "rb") as f:
        return f.read()


def pack(files: list[tuple[str, bytes]], part_size: int) -> bytes:
    if not 1 <= len(files) <= MAX_ENTRIES:
        raise SystemExit("bad file count")
    header = MAGIC + struct.pack("<IIII", VERSION, len(files), 0, 0)
    entries = b""
    body = b""
    offset = ALIGN
    for name, data in files:
        raw = name.encode("ascii")
        if not 0 < len(raw) < NAME_CAP:
            raise SystemExit(f"bad name {name}")
        entries += raw.ljust(NAME_CAP, b"\0") + struct.pack("<II", offset, len(data)) + hashlib.sha256(data).digest()
        pad = (-len(data)) % ALIGN
        body += data + b"\xff" * pad
        offset += len(data) + pad
    directory = header + entries
    image = directory + b"\xff" * (ALIGN - len(directory)) + body
    if len(image) > part_size:
        raise SystemExit(f"image {len(image)} B does not fit the {part_size} B partition")
    return image


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cxx", required=True, help="a C++ compiler that targets a 32-bit host")
    ap.add_argument("--cxx-flag", action="append", default=[], help="extra compiler flag (repeatable)")
    ap.add_argument("--work", required=True, help="scratch directory, outside the repository")
    ap.add_argument("--out", required=True, help="output image")
    ap.add_argument("--partition-size", default="0x7B0000", help="size of the ime_data partition (ADR-033)")
    ap.add_argument("--hanzi-font", help="KVMFONT1 blob of tools/fonts/gen_hanzi_font_blob.py (--set zh)")
    ap.add_argument("--kanji-font", help="KVMFONT1 blob of tools/fonts/gen_hanzi_font_blob.py --set ja")
    ap.add_argument("--openwnn-dict", default=build_wnn_blob.DEFAULT_SRC,
                    help="vendored WnnJpnDic.c the Japanese dictionaries come from")
    ap.add_argument("--no-openwnn", action="store_true", help="leave wnn_ja.bin out of the image")
    args = ap.parse_args(argv)

    work = os.path.abspath(args.work)
    if os.path.commonpath([work, REPO]) == REPO:
        print("--work must be outside the repository", file=sys.stderr)
        return 1
    os.makedirs(work, exist_ok=True)
    dict_pinyin = build_pinyin_dict(args.cxx, args.cxx_flag, work)
    files = [("dict_pinyin.dat", dict_pinyin)]
    if args.hanzi_font:
        with open(args.hanzi_font, "rb") as f:
            font = f.read()
        if font[:8] != b"KVMFONT1":
            print(f"{args.hanzi_font}: not a KVMFONT1 blob", file=sys.stderr)
            return 1
        with open(os.path.join(REPO, "components", "ui_theme", "fonts", "OFL-NotoSansKR.txt"), "rb") as f:
            ofl = f.read()
        files += [("font_hanzi_24.bin", font), ("ofl_noto_cjk.txt", ofl)]
    if args.kanji_font:
        with open(args.kanji_font, "rb") as f:
            kanji = f.read()
        if kanji[:8] != b"KVMFONT1":
            print(f"{args.kanji_font}: not a KVMFONT1 blob", file=sys.stderr)
            return 1
        files.append(("font_kanji_24.bin", kanji))
        if not args.hanzi_font:  # the OFL text travels with either Noto subset
            with open(os.path.join(REPO, "components", "ui_theme", "fonts", "OFL-NotoSansKR.txt"), "rb") as f:
                files.append(("ofl_noto_cjk.txt", f.read()))
    if not args.no_openwnn:
        files.append(("wnn_ja.bin", build_wnn_blob.build(args.openwnn_dict)))
    image = pack(files, int(args.partition_size, 0))
    with open(args.out, "wb") as f:
        f.write(image)
    for name, data in files:
        print(f"{name} {len(data)} B sha256 {hashlib.sha256(data).hexdigest()}")
    print(f"{args.out}: {len(image)} B sha256 {hashlib.sha256(image).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
