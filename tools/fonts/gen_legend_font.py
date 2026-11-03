#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Generate the keyboard legend fonts of the HOST layouts (ADR-024 TECH-078, FR-742).

The theme's Montserrat set covers ASCII and the LVGL symbols, so a layout that prints Cyrillic, Greek or an
accented Latin letter on a cap had no glyph for it. This generator rasterizes the legend code points of
``components/kbd_layouts/generated/legend_codepoints.txt`` from Noto Sans (SIL Open Font License 1.1), pinned by
release tag and SHA256, into one LVGL font per text size the caps use, each with the Montserrat of that size as
its ``--lv-fallback``. Every output is therefore a *superset* of the theme font it replaces: ui_theme hands the
same three roles out as before and nothing else in the UI changes.

Subset (TECH-078 budget: the tables and the fonts together stay under 1.5 MiB):
  * every legend code point below U+0530 - ASCII, the Latin supplements and extensions, Greek and Cyrillic;
  * the punctuation, currency, arrow, math and symbol code points the legends use (U+1E00..U+2C00);
  * U+FFFD REPLACEMENT CHARACTER, which the KEYBOARD view draws in place of a legend it has no glyph for.
Left out: the complex scripts (Arabic, Hebrew, the Indic scripts, Thai, Lao, Khmer, Myanmar, Tibetan, Mongolian,
Ethiopic, Cherokee, the Canadian syllabics) and the CJK blocks. Those layouts are the ones the generator already
marks ``KBD_LAYOUT_BEST_EFFORT``: they need shaping, which LVGL does not do, and they would add roughly 2900 more
glyphs per size. Their caps show the replacement character and the key still sends its usage (FR-743).

Usage (run from anywhere; nothing is written outside --out-dir):
    python -I tools/fonts/gen_legend_font.py --font <NotoSans-Regular.otf> \\
        --lv-font-conv <node_modules/lv_font_conv/lv_font_conv.js> \\
        --codepoints components/kbd_layouts/generated/legend_codepoints.txt \\
        --out-dir components/ui_theme/fonts

Inputs (pinned):
    Noto Sans Regular, notofonts/latin-greek-cyrillic tag NotoSans-v2.015, NotoSans/unhinted/otf/NotoSans-Regular.otf
    lv_font_conv 1.5.3 (npm)
The output is reproducible byte for byte: the generator's command line comment is normalized so no local path
reaches the file.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile

FONT_SHA256 = "bfb44de2c2031c391ff856943fe8f105ebb5d725cb9a04d493d211d5bbd5c21b"
FONT_SOURCE = ("notofonts/latin-greek-cyrillic tag NotoSans-v2.015, "
               "NotoSans/unhinted/otf/NotoSans-Regular.otf")
LV_FONT_CONV_VERSION = "1.5.3"
BPP = 4

# One output per text size of ui_theme.h: UI_FONT_TITLE (the letter caps), UI_FONT_BODY (the special caps and the
# body text) and UI_FONT_LABEL (the Shift and AltGr corners).
SIZES = (24, 16, 14)

# The fallback glyph the KEYBOARD view shows for a legend this subset has no glyph for (TECH-078).
FALLBACK_CP = 0xFFFD

HEADER = (
    "// SPDX-License-Identifier: OFL-1.1\n"
    "// Derived font: glyphs of Noto Sans (Copyright 2022 The Noto Project Authors,\n"
    "// https://github.com/notofonts/latin-greek-cyrillic), rasterized to an LVGL subset of the keyboard legend\n"
    "// code points by tools/fonts/gen_legend_font.py. Licensed under the SIL Open Font License 1.1; see\n"
    "// components/ui_theme/fonts/OFL-NotoSans.txt and THIRD_PARTY_NOTICES.md.\n"
)


# Invisible formatting characters GCC rejects in a source file, comment included (-Werror=bidi-chars).
BIDI_CONTROLS = (0x061C, 0x200E, 0x200F, 0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069)

def wanted(path: str) -> list[int]:
    """The legend code points this subset covers, from the generated list."""
    out: list[int] = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line.startswith("U+"):
                continue
            cp = int(line[2:], 16)
            if cp < 0x530 or 0x1E00 <= cp < 0x2C00:
                out.append(cp)
    out.append(FALLBACK_CP)
    return sorted(set(out))


def font_cmap(path: str) -> set[int]:
    """
    The code points the font actually maps, read straight out of its `cmap` table (formats 4 and 12).

    lv_font_conv fails on a range the font does not cover at all, and the legend list spans blocks that no single
    Noto family carries, so the subset is intersected with this set. No third-party module is needed - and that
    keeps the generator runnable with `python -I` in CI, like the rest of tools/.
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 12:
        raise ValueError("not a font file")
    num_tables = int.from_bytes(data[4:6], "big")
    cmap_off = 0
    for i in range(num_tables):
        rec = 12 + 16 * i
        if data[rec:rec + 4] == b"cmap":
            cmap_off = int.from_bytes(data[rec + 8:rec + 12], "big")
            break
    if cmap_off == 0:
        raise ValueError("font has no cmap table")
    n_sub = int.from_bytes(data[cmap_off + 2:cmap_off + 4], "big")
    out: set[int] = set()
    for i in range(n_sub):
        rec = cmap_off + 4 + 8 * i
        sub = cmap_off + int.from_bytes(data[rec + 4:rec + 8], "big")
        fmt = int.from_bytes(data[sub:sub + 2], "big")
        if fmt == 4:
            seg_x2 = int.from_bytes(data[sub + 6:sub + 8], "big")
            segs = seg_x2 // 2
            ends = sub + 14
            starts = ends + seg_x2 + 2
            for s in range(segs):
                end = int.from_bytes(data[ends + 2 * s:ends + 2 * s + 2], "big")
                start = int.from_bytes(data[starts + 2 * s:starts + 2 * s + 2], "big")
                if start <= end and end != 0xFFFF:
                    out.update(range(start, end + 1))
        elif fmt == 12:
            n_groups = int.from_bytes(data[sub + 12:sub + 16], "big")
            for g in range(n_groups):
                rec_g = sub + 16 + 12 * g
                start = int.from_bytes(data[rec_g:rec_g + 4], "big")
                end = int.from_bytes(data[rec_g + 4:rec_g + 8], "big")
                if end - start < 0x20000:
                    out.update(range(start, end + 1))
    if len(out) < 500:
        raise ValueError(f"cmap gave only {len(out)} code points")
    return out


def ranges(cps: list[int]) -> list[str]:
    """Collapse sorted code points into lv_font_conv `-r` ranges."""
    out: list[str] = []
    i = 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1:
            j += 1
        out.append(f"0x{cps[i]:X}-0x{cps[j]:X}" if j > i else f"0x{cps[i]:X}")
        i = j + 1
    return out


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def one(args, cps: list[int], size: int) -> int:
    name = f"kvm_font_legend_{size}"
    out = os.path.join(args.out_dir, f"{name}.c")
    with tempfile.TemporaryDirectory(dir=os.path.abspath(args.out_dir)) as tmp:
        raw = os.path.join(tmp, "font.c")
        cmd = [args.node, args.lv_font_conv, "--size", str(size), "--bpp", str(BPP), "--format", "lvgl",
               "--no-compress", "--no-prefilter", "--font", args.font, "-r", ",".join(ranges(cps)),
               "--lv-include", "lvgl.h", "--lv-font-name", name,
               "--lv-fallback", f"lv_font_montserrat_{size}", "-o", raw]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr, file=sys.stderr)
            return 1
        with open(raw, encoding="utf-8") as f:
            text = f.read()

    # No local path in the output: the "Opts:" comment names the inputs by role.
    text, n = re.subn(
        r"^( \* Opts: ).*$",
        rf"\1--size {size} --bpp {BPP} --format lvgl --no-compress --no-prefilter --font NotoSans-Regular.otf "
        rf"({FONT_SOURCE}, sha256 {FONT_SHA256[:16]}...) -r <{len(cps)} keyboard legend code points> "
        rf"--lv-font-name {name} --lv-fallback lv_font_montserrat_{size}",
        text, count=1, flags=re.M)
    if n != 1:
        print("lv_font_conv output has no Opts line; check the version", file=sys.stderr)
        return 1
    # GCC refuses a source file that carries an unpaired bidirectional control character, even inside a
    # comment (-Werror=bidi-chars), and lv_font_conv prints every glyph's code point as a literal character.
    # Those code points are written out as <U+XXXX> in the comment instead; no glyph data changes.
    for cp in BIDI_CONTROLS:
        text = text.replace(chr(cp), f"<U+{cp:04X}>")
    text = text.replace("\r\n", "\n")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + text)
    print(f"{out}: {len(cps)} code points, {os.path.getsize(out)} bytes of source")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--font", required=True, help="NotoSans-Regular.otf (pinned by SHA256)")
    ap.add_argument("--lv-font-conv", required=True, help="lv_font_conv.js of the npm package 1.5.3")
    ap.add_argument("--codepoints", required=True, help="generated/legend_codepoints.txt")
    ap.add_argument("--out-dir", required=True, help="output directory (components/ui_theme/fonts)")
    ap.add_argument("--node", default="node", help="the Node.js executable")
    args = ap.parse_args(argv)

    got = sha256(args.font)
    if got != FONT_SHA256:
        print(f"font SHA256 mismatch: {got}, expected {FONT_SHA256}", file=sys.stderr)
        return 1
    ver = subprocess.run([args.node, args.lv_font_conv, "--version"], capture_output=True, text=True)
    if ver.stdout.strip() != LV_FONT_CONV_VERSION:
        print(f"lv_font_conv {ver.stdout.strip()!r}, expected {LV_FONT_CONV_VERSION}", file=sys.stderr)
        return 1

    cps = wanted(args.codepoints)
    if len(cps) < 200:
        print(f"only {len(cps)} legend code points: has the generator run?", file=sys.stderr)
        return 1
    have = font_cmap(args.font)
    missing = [c for c in cps if c not in have]
    cps = [c for c in cps if c in have]
    if FALLBACK_CP not in have:
        print(f"the font has no U+{FALLBACK_CP:04X}: the fallback glyph would be blank", file=sys.stderr)
        return 1
    print(f"{len(cps)} of {len(cps) + len(missing)} legend code points are in this font; "
          f"{len(missing)} are not and fall back to U+{FALLBACK_CP:04X}: "
          + ", ".join(f"U+{c:04X}" for c in missing[:12]) + (" ..." if len(missing) > 12 else ""))
    for size in SIZES:
        rc = one(args, cps, size)
        if rc != 0:
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
