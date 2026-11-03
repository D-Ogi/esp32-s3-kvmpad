#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build `wnn_ja.bin`, the Japanese dictionary container of the `ime_data` image (ADR-024 TECH-081).

The vendored OpenWnn dictionaries ship as C byte arrays in
`components/ime_engine/openwnn/data/WnnJpnDic.c`. The firmware reads them in place from the
`ime_data` partition instead of linking them into the application image, so this script extracts
the arrays the Japanese conversion needs and packs them into one container
(`components/ime_engine/include/ime_wnn.h` documents the layout):

  dic_05_data  single-kanji dictionary   (engine index 4, NJ_DIC_TYPE_TANKANJI)
  dic_06_data  independent words         (engine index 5, NJ_DIC_TYPE_JIRITSU)
  dic_07_data  ancillary words           (engine index 6, NJ_DIC_TYPE_FZK)
  con_01_data  connection matrix         (the rule dictionary, NJ_DIC_TYPE_RULE)

The prediction dictionaries of the same file (`dic_01`..`dic_04`) are left out: the clause
conversion never sets them, and keeping them would add 446 KB to the image for nothing.

The output is reproducible: the same source gives the same bytes.

Usage:
    python -I tools/ime/build_wnn_blob.py --out <wnn_ja.bin> [--src <WnnJpnDic.c>]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_SRC = os.path.join(REPO, "components", "ime_engine", "openwnn", "data", "WnnJpnDic.c")

MAGIC = b"KVMWNNJA"
VERSION = 1
HEADER = 64
ALIGN = 16
# The arrays to pack, in the order the container holds them, with the dictionary type the engine
# must report for each (NJ_GET_DIC_TYPE, nj_dic.h).
PARTS = [
    ("dic_05_data", 0x00000002),  # NJ_DIC_TYPE_TANKANJI
    ("dic_06_data", 0x00000000),  # NJ_DIC_TYPE_JIRITSU
    ("dic_07_data", 0x00000001),  # NJ_DIC_TYPE_FZK
    ("con_01_data", 0x000F0000),  # NJ_DIC_TYPE_RULE
]
ARRAY_RE = re.compile(r"static\s+NJ_UINT8\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", re.S)
BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")


def extract(src_path: str) -> dict[str, bytes]:
    """Read the byte arrays of WnnJpnDic.c and check them against its own dic_size[] table."""
    with open(src_path, "r", encoding="ascii") as f:
        text = f.read()
    arrays: dict[str, bytes] = {}
    for m in ARRAY_RE.finditer(text):
        arrays[m.group(1)] = bytes(int(h, 16) for h in BYTE_RE.findall(m.group(2)))
    sizes_m = re.search(r"NJ_UINT32\s+dic_size\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if sizes_m is None:
        raise SystemExit(f"{src_path}: no dic_size[] table")
    sizes = [int(x) for x in re.findall(r"\d+", sizes_m.group(1))]
    for i, size in enumerate(sizes):
        name = "dic_%02d_data" % (i + 1)
        if size == 0:
            continue
        if name not in arrays:
            raise SystemExit(f"{src_path}: {name} missing")
        if len(arrays[name]) != size:
            raise SystemExit(f"{src_path}: {name} is {len(arrays[name])} B, dic_size says {size}")
    return arrays


def dic_type(data: bytes) -> int:
    """NJ_GET_DIC_TYPE: a big-endian u32 at offset 8 of a dictionary header."""
    return int.from_bytes(data[8:12], "big")


def pack(arrays: dict[str, bytes]) -> tuple[bytes, list[tuple[str, int, int]]]:
    parts = []
    for name, want in PARTS:
        if name not in arrays:
            raise SystemExit(f"{name} missing")
        data = arrays[name]
        if len(data) <= 0x1C or data[:4] != b"NJDC":
            raise SystemExit(f"{name}: not an NJDC dictionary")
        got = dic_type(data)
        if got != want:
            raise SystemExit(f"{name}: dictionary type {got:#010x}, expected {want:#010x}")
        parts.append((name, data))

    offset = HEADER
    entries = b""
    body = b""
    table = []
    for name, data in parts:
        pad = (-offset) % ALIGN
        body += b"\0" * pad
        offset += pad
        entries += struct.pack("<II", offset, len(data))
        table.append((name, offset, len(data)))
        body += data
        offset += len(data)
    header = MAGIC + struct.pack("<II", VERSION, len(parts)) + entries
    if len(header) > HEADER:
        raise SystemExit("header overflow")
    return header + b"\0" * (HEADER - len(header)) + body, table


def build(src_path: str) -> bytes:
    blob, _ = pack(extract(src_path))
    return blob


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="output container")
    ap.add_argument("--src", default=DEFAULT_SRC, help="vendored WnnJpnDic.c")
    args = ap.parse_args(argv)

    blob, table = pack(extract(args.src))
    with open(args.out, "wb") as f:
        f.write(blob)
    for name, offset, size in table:
        print(f"{name} offset {offset} size {size}")
    print(f"{args.out}: {len(blob)} B sha256 {hashlib.sha256(blob).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
