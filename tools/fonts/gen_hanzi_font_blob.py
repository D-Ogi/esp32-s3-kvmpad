# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Generate the hanzi font of the pinyin candidate bar as a KVMFONT1 blob (ADR-024 FR-746, TECH-081).

The hanzi a candidate can contain are the characters of the vendored PinyinIME dictionary (16 463 code points) plus
CJK punctuation and the full-width forms. At 24 px 4 bpp that is ~4.5 MB of plain glyphs, ~3 MB in LVGL's RLE
compression with the line prefilter (blob version 2): too large for the application image (TECH-078 budget), so the
font ships in the read-only ime_data partition next to the dictionaries and the firmware reads the glyphs in place
from flash (components/ui_theme/include/ui_font_blob.h describes the format).

The glyphs come from Noto Sans SC (SIL Open Font License 1.1), pinned by release tag and SHA256, rasterized by
lv_font_conv 1.5.3 (MIT) in parts of at most 1 MiB of bitmap each (the 20-bit bitmap index of an LVGL font). The
output is a derived font and keeps the OFL; build_ime_data.py packs the OFL text into the same image.

    python -I tools/fonts/gen_hanzi_font_blob.py --font <NotoSansSC-Regular.otf> \\
        --lv-font-conv <node_modules/lv_font_conv/lv_font_conv.js> --work ../build-ime-data/font \\
        --out ../build-ime-data/font_hanzi_24.bin

Pinned inputs:
    Noto Sans SC Regular, notofonts/noto-cjk tag Sans2.004, Sans/SubsetOTF/SC/NotoSansSC-Regular.otf
    lv_font_conv 1.5.3 (npm)
The output is byte for byte reproducible from these inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys

# --set zh: Noto Sans SC, the PinyinIME characters; --set ja: Noto Sans JP, the kana and kanji of JIS X 0208.
FONT_SHA256 = {"zh": "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9",
               "ja": "dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073"}
LV_FONT_CONV_VERSION = "1.5.3"
SIZE = 24
BPP = 4
MAX_BITMAP = (1 << 20) - 1  # bitmap_index is a 20-bit field (LV_FONT_FMT_TXT_LARGE == 0)
FIRST_PART = 5000  # code points per lv_font_conv run (~0.9 MB of compressed 24 px glyphs); a run over the limit is halved

MAGIC = b"KVMFONT1"
VERSION = 2  # RLE-compressed glyphs with the line prefilter (LV_FONT_FMT_TXT_COMPRESSED)
HEADER_LEN = 32
PART_LEN = 48
CMAP_LEN = 16
# lv_font_fmt_txt_cmap_type_t values. FORMAT0_FULL carries uint8 glyph id offsets (0 past the first entry = no glyph),
# SPARSE_TINY uint16 code point offsets.
CMAP_TYPES = {"LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": 0, "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": 2,
              "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": 3}

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DICT = os.path.join(REPO, "components", "ime_engine", "pinyinime", "data", "rawdict_utf16_65105_freq.txt")


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def code_points(which: str) -> list[int]:
    return code_points_zh() if which == "zh" else code_points_ja()


def code_points_ja() -> list[int]:
    """JIS X 0208 from U+3000 on (CJK symbols, kana, the 6355 kanji, full-width forms) - what Japanese text and the
    OpenWnn dictionaries use - plus the general punctuation of Japanese text. The Latin, Greek, Cyrillic and box
    drawing rows of JIS X 0208 are left to the fallback font."""
    cps = {0x2010, 0x2015, 0x2018, 0x2019, 0x201C, 0x201D, 0x2025, 0x2026}
    for hi in range(0xA1, 0xFF):
        for lo in range(0xA1, 0xFF):
            try:
                s = bytes([hi, lo]).decode("euc_jp")
            except UnicodeDecodeError:
                continue
            if len(s) == 1 and ord(s) >= 0x3000:
                cps.add(ord(s))
    return sorted(cps)


def code_points_zh() -> list[int]:
    """Every character of the dictionary plus the punctuation of Chinese text (in the em box of the hanzi)."""
    with open(DICT, "rb") as f:
        text = f.read().decode("utf-16")
    # 、。〈〉《》「」『』【】〔〕〖〗, the general punctuation of Chinese text (‘’“”—…·) and the full-width
    # ASCII forms ！..～
    cps = set(range(0x3001, 0x3003)) | set(range(0x3008, 0x3012)) | set(range(0x3014, 0x3018))
    cps |= {0x2018, 0x2019, 0x201C, 0x201D, 0x2014, 0x2026, 0x00B7}
    cps |= set(range(0xFF01, 0xFF5F))
    for line in text.splitlines():
        word = line.split(" ", 1)[0]
        cps.update(ord(c) for c in word if ord(c) > 0x7F)
    return sorted(cps)


def ranges(cps: list[int]) -> str:
    out = []
    start = prev = cps[0]
    for cp in cps[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        out.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
        start = prev = cp
    out.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
    return ",".join(out)


def array(src: str, decl: str) -> str:
    m = re.search(re.escape(decl) + r"\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise ValueError(f"array {decl!r} not found in the lv_font_conv output")
    return m.group(1)


def parse_c(src: str) -> dict:
    """Read the arrays of an lv_font_conv `--format lvgl` file (compressed + prefiltered bitmaps, no kerning)."""
    if "kern_dsc = NULL" not in src or ".bitmap_format = 1" not in src:
        raise ValueError("expected a compressed, prefiltered font without kerning")
    body = re.sub(r"/\*.*?\*/", "", array(src, "glyph_bitmap[]"), flags=re.S)
    bitmap = bytes(int(t, 16) for t in re.findall(r"0x[0-9a-fA-F]{1,2}\b", body))
    dsc = [tuple(int(v) for v in m.groups()) for m in re.finditer(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), "
        r"\.ofs_y = (-?\d+)\}", array(src, "glyph_dsc[]"))]
    lists = {m.group(1): [int(t, 16) for t in re.findall(r"0x[0-9a-fA-F]+", m.group(2))] for m in re.finditer(
        r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", src, re.S)}
    ofs_lists = {m.group(1): [int(t) for t in re.findall(r"\d+", m.group(2))] for m in re.finditer(
        r"static const uint8_t (glyph_id_ofs_list_\d+)\[\] = \{(.*?)\};", src, re.S)}
    cmaps = []
    for m in re.finditer(
            r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
            r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = (\d+), \.type = (\w+)", src):
        start, length, gid, ulist, ofs, n, kind = m.groups()
        if kind not in CMAP_TYPES:
            raise ValueError(f"unsupported cmap {kind}")
        if kind == "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL":
            if ulist != "NULL" or ofs == "NULL":
                raise ValueError("FORMAT0_FULL without its offset list")
            entries = ofs_lists[ofs]
        elif ofs != "NULL":
            raise ValueError(f"{kind} with a glyph id offset list")
        else:
            entries = [] if ulist == "NULL" else lists[ulist]
        if len(entries) != int(n):
            raise ValueError(f"{ulist}: {len(entries)} entries, list_length {n}")
        cmaps.append((int(start), int(length), int(gid), entries, CMAP_TYPES[kind]))
    metrics = {}
    for key in ("line_height", "base_line", "underline_position", "underline_thickness"):
        m = re.search(r"\." + key + r" = (-?\d+),", src)
        if not m:
            raise ValueError(f"{key} not found")
        metrics[key] = int(m.group(1))
    if not dsc or dsc[0] != (0, 0, 0, 0, 0, 0) or not cmaps:
        raise ValueError("unexpected glyph table")
    return {"bitmap": bitmap, "dsc": dsc, "cmaps": cmaps, **metrics}


def convert(args, cps: list[int], name: str) -> dict:
    out = os.path.join(args.work, name + ".c")
    cmd = [args.node, args.lv_font_conv, "--size", str(SIZE), "--bpp", str(BPP), "--format", "lvgl",
           "--no-kerning", "--font", args.font, "-r", ranges(cps), "--lv-include", "lvgl.h",
           "--lv-font-name", name, "-o", out]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    with open(out, encoding="utf-8") as f:
        part = parse_c(f.read())
    # LVGL's bit reader loads the byte after the last one a glyph uses when its bits end on a byte boundary: one zero
    # byte of slack keeps the last glyph's read inside the part (ui_font_blob.c checks every glyph against it).
    part["bitmap"] += b"\0"
    part["cps"] = len(cps)
    return part


def parts_for(args, cps: list[int], name: str) -> list[dict]:
    """Rasterize `cps`, halving any run whose bitmap does not fit a 20-bit bitmap index."""
    part = convert(args, cps, name)
    if len(part["bitmap"]) <= MAX_BITMAP:
        return [part]
    if len(cps) < 2:
        raise ValueError("a single glyph exceeds the bitmap limit")
    half = len(cps) // 2
    return parts_for(args, cps[:half], name + "a") + parts_for(args, cps[half:], name + "b")


def align4(buf: bytearray) -> None:
    while len(buf) % 4:
        buf.append(0)


def pack(parts: list[dict]) -> bytes:
    blob = bytearray(HEADER_LEN + PART_LEN * len(parts))
    table = []
    for p in parts:
        # unicode lists first (2-byte entries), then the cmap records, glyph descriptors and bitmap (4-aligned)
        list_off = []
        for _, _, _, entries, kind in p["cmaps"]:
            align4(blob)
            list_off.append(len(blob) if entries else 0)
            for e in entries:
                blob += struct.pack("<B" if kind == 0 else "<H", e)
        align4(blob)
        cmap_off = len(blob)
        for (start, length, gid, entries, kind), off in zip(p["cmaps"], list_off):
            blob += struct.pack("<IHHIHBB", start, length, gid, off, len(entries), kind, 0)
        dsc_off = len(blob)
        for index, adv, w, h, x, y in p["dsc"]:
            if index >= (1 << 20) or adv >= (1 << 12) or not (0 <= w < 256 and 0 <= h < 256):
                raise ValueError("glyph field out of range")
            blob += struct.pack("<IBBbb", index | (adv << 20), w, h, x, y)
        bitmap_off = len(blob)
        blob += p["bitmap"]
        align4(blob)
        table.append((p["line_height"], p["base_line"], p["underline_position"], p["underline_thickness"], cmap_off,
                      len(p["cmaps"]), dsc_off, len(p["dsc"]), bitmap_off, len(p["bitmap"]), 0, 0))
    struct.pack_into("<8sIIII8x", blob, 0, MAGIC, VERSION, len(parts), BPP, len(blob))
    for i, row in enumerate(table):
        struct.pack_into("<iiiiIIIIIIII", blob, HEADER_LEN + PART_LEN * i, *row)
    return bytes(blob)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", required=True, help="NotoSansSC-Regular.otf (--set zh) or NotoSansJP-Regular.otf (--set ja)")
    ap.add_argument("--set", choices=("zh", "ja"), default="zh", help="character set: pinyin dictionary or JIS X 0208")
    ap.add_argument("--lv-font-conv", required=True, help="lv_font_conv.js of the npm package 1.5.3")
    ap.add_argument("--node", default="node", help="the Node.js executable")
    ap.add_argument("--work", required=True, help="scratch directory outside the repository")
    ap.add_argument("--out", required=True, help="the blob to write, outside the repository")
    args = ap.parse_args(argv)

    for path in (args.work, args.out):
        if os.path.abspath(path).startswith(REPO + os.sep):
            print(f"{path}: must be outside the repository", file=sys.stderr)
            return 2
    got = sha256(args.font)
    if got != FONT_SHA256[args.set]:
        print(f"font SHA256 mismatch: {got}, expected {FONT_SHA256[args.set]}", file=sys.stderr)
        return 2
    ver = subprocess.run([args.node, args.lv_font_conv, "--version"], capture_output=True, text=True)
    if ver.stdout.strip() != LV_FONT_CONV_VERSION:
        print(f"lv_font_conv {ver.stdout.strip()!r}, expected {LV_FONT_CONV_VERSION}", file=sys.stderr)
        return 2
    os.makedirs(args.work, exist_ok=True)

    cps = code_points(args.set)
    parts = []
    for i in range(0, len(cps), FIRST_PART):
        parts += parts_for(args, cps[i:i + FIRST_PART], f"hanzi_part{i // FIRST_PART}")
    blob = pack(parts)
    with open(args.out, "wb") as f:
        f.write(blob)
    glyphs = sum(len(p["dsc"]) - 1 for p in parts)
    print(f"{args.out}: {len(blob)} bytes, {len(parts)} parts, {glyphs} glyphs of {len(cps)} code points, "
          f"sha256 {hashlib.sha256(blob).hexdigest()}")
    for i, p in enumerate(parts):
        print(f"  part {i}: {p['cps']} code points, {len(p['dsc']) - 1} glyphs, {len(p['cmaps'])} cmaps, "
              f"bitmap {len(p['bitmap'])} B, line_height {p['line_height']}, base_line {p['base_line']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
