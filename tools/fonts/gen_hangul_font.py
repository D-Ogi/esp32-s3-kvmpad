#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Generate the 24 px Hangul legend font of the KO composition strip (ADR-024 TECH-078, FR-746).

The glyphs come from Noto Sans KR (SIL Open Font License 1.1), pinned by release tag and SHA256, and are
rasterized by lv_font_conv (MIT). The output is a derived font and keeps the OFL: it carries an OFL-1.1 SPDX
line instead of the KVMpad header and is listed in THIRD_PARTY_NOTICES.md (ADR-025).

Uncompressed bitmaps (LV_USE_FONT_COMPRESSED stays off; no decompression at render time).

Subset (2401 code points):
  * U+3131..U+3163  Hangul compatibility jamo (a lone consonant or vowel in the preedit);
  * the 2350 precomposed syllables of KS X 1001 (row 0xB0..0xC8 of EUC-KR), which cover everyday Korean text.
Latin and digits fall back to Montserrat 24 (`--lv-fallback`), so the strip keeps the theme's Latin face.
A syllable outside KS X 1001 still composes and is still sent to the HOST; only its preview glyph is missing.

Usage (run from anywhere; nothing is written outside --out):
    python -I tools/fonts/gen_hangul_font.py --font <NotoSansKR-Regular.otf> \\
        --lv-font-conv <node_modules/lv_font_conv/lv_font_conv.js> --out components/ui_theme/fonts/kvm_font_hangul_24.c

Inputs (pinned):
    Noto Sans KR Regular, notofonts/noto-cjk tag Sans2.004, Sans/SubsetOTF/KR/NotoSansKR-Regular.otf
    lv_font_conv 1.5.3 (npm)
The output is reproducible byte for byte: the generator's command line comment is normalized so no local
path reaches the file.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile

FONT_SHA256 = "69975a0ac8472717870aefeab0a4d52739308d90856b9955313b2ad5e0148d68"
FONT_SOURCE = "notofonts/noto-cjk tag Sans2.004, Sans/SubsetOTF/KR/NotoSansKR-Regular.otf"
LV_FONT_CONV_VERSION = "1.5.3"
SIZE = 24
BPP = 4
NAME = "kvm_font_hangul_24"

HEADER = (
    "// SPDX-License-Identifier: OFL-1.1\n"
    "// Derived font: glyphs of Noto Sans KR (Copyright 2014-2021 Adobe (http://www.adobe.com/); Noto is a\n"
    "// trademark of Google Inc.), rasterized to a 24 px 4 bpp LVGL subset by tools/fonts/gen_hangul_font.py.\n"
    "// Licensed under the SIL Open Font License 1.1; see components/ui_theme/fonts/OFL-NotoSansKR.txt and\n"
    "// THIRD_PARTY_NOTICES.md.\n"
)


def ksx1001_syllables() -> list[int]:
    """The 2350 precomposed syllables of KS X 1001, in code order (EUC-KR rows 0xB0..0xC8)."""
    out: list[int] = []
    for lead in range(0xB0, 0xC9):
        for trail in range(0xA1, 0xFF):
            try:
                ch = bytes((lead, trail)).decode("euc_kr")
            except UnicodeDecodeError:
                continue
            cp = ord(ch)
            if 0xAC00 <= cp <= 0xD7A3:
                out.append(cp)
    return out


def ranges(cps: list[int]) -> list[str]:
    """Collapse sorted code points into lv_font_conv `-r` ranges."""
    out: list[str] = []
    cps = sorted(set(cps))
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


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--font", required=True, help="NotoSansKR-Regular.otf (pinned by SHA256)")
    ap.add_argument("--lv-font-conv", required=True, help="lv_font_conv.js of the npm package 1.5.3")
    ap.add_argument("--out", required=True, help="output .c file")
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

    syllables = ksx1001_syllables()
    if len(syllables) != 2350:
        print(f"KS X 1001 table gave {len(syllables)} syllables, expected 2350", file=sys.stderr)
        return 1
    cps = list(range(0x3131, 0x3164)) + syllables

    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(args.out))) as tmp:
        raw = os.path.join(tmp, "font.c")
        cmd = [args.node, args.lv_font_conv, "--size", str(SIZE), "--bpp", str(BPP), "--format", "lvgl", "--no-compress", "--no-prefilter",
               "--font", args.font, "-r", ",".join(ranges(cps)), "--lv-include", "lvgl.h",
               "--lv-font-name", NAME, "--lv-fallback", "lv_font_montserrat_24", "-o", raw]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr, file=sys.stderr)
            return 1
        with open(raw, encoding="utf-8") as f:
            text = f.read()

    # No local path in the output: the "Opts:" comment names the inputs by role.
    text, n = re.subn(r"^( \* Opts: ).*$",
                      rf"\1--size {SIZE} --bpp {BPP} --format lvgl --no-compress --no-prefilter --font NotoSansKR-Regular.otf "
                      rf"({FONT_SOURCE}, sha256 {FONT_SHA256[:16]}...) -r <U+3131..U+3163 + {len(syllables)} KS X "
                      rf"1001 syllables> --lv-font-name {NAME} --lv-fallback lv_font_montserrat_24",
                      text, count=1, flags=re.M)
    if n != 1:
        print("lv_font_conv output has no Opts line; check the version", file=sys.stderr)
        return 1
    text = text.replace("\r\n", "\n")
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + text)
    print(f"{args.out}: {len(cps)} code points, {os.path.getsize(args.out)} bytes of source")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
