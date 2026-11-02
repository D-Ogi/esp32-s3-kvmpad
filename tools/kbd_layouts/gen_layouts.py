#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""TASK-071 keyboard layout generator (ADR-024 TECH-077..079, AT-EXT-22).

Reads the layout list from xkeyboard-config ``rules/base.xml``, compiles every ``layout(variant)`` with libxkbcommon
(through ctypes, ``rules=evdev``, ``model=pc105``, no options), reads the keysym of each key of the alphanumeric block
and of the IME keys at four levels (Base, Shift, AltGr, Shift+AltGr) by pressing the physical modifier keys in an
``xkb_state``, maps XKB key names to USB HID usages (page 0x07) through keycodemapdb ``data/keymaps.csv`` and writes:

- ``components/kbd_layouts/generated/kbd_layouts_data.c`` - the DEVICE tables (``include/kbd_layouts.h`` types);
- ``clients/windows/layouts/kbd_layouts.json`` - the CLIENT tables with the character -> keystroke index;
- ``components/kbd_layouts/generated/REPORT.md`` - shipped and rejected layouts, best-effort scripts, sizes;
- ``components/kbd_layouts/generated/legend_codepoints.txt`` - every code point of a shipped legend (TECH-078).

libxkbcommon is a build-time tool only; it is not readily available on Windows, so the generator runs on a Linux tooling
host (the CI workflow ``.github/workflows/kbd-layouts.yml``). The outputs are committed and never edited by hand; CI
regenerates them and requires an empty diff. The outputs depend only on the pinned inputs: no timestamps, sorted
iteration, ``\\n`` line ends.

Geometry family rule (no per-country code): a symbol on ``AE13`` (Yen) or a Japanese IME keysym -> JIS; a Hangul or
Hangul_Hanja keysym anywhere -> KR; otherwise a symbol on ``AB11`` -> ABNT2; otherwise a Base level other than
``backslash`` on ``BKSL`` (the key then sits in the ISO position next to Enter) -> ISO; otherwise ANSI.

Dead keys: the legend shows the accent; the CLIENT index adds ``dead key + letter`` for every letter whose NFC
composition with the accent's combining mark is one code point the layout cannot type directly (an approximation of
the HOST's dead-key tables for Latin scripts; anything else goes through the Unicode delivery mode, FR-749).

Usage::

    gen_layouts.py --print-pins
    gen_layouts.py --xkb-root <share/xkeyboard-config-2> --keymaps-csv <keycodemapdb/data/keymaps.csv> \\
                   --libxkbcommon <libxkbcommon.so> --repo <repo root> [--out-root <dir>]
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import re
import sys
import unicodedata
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

# ---- pinned inputs (TECH-077). A bump is a deliberate change with a reviewed regeneration diff. -----------------------
PINS = {
    "XKC_VERSION": "2.48",
    "XKC_URL": "https://xorg.freedesktop.org/archive/individual/data/xkeyboard-config/xkeyboard-config-2.48.tar.xz",
    "XKC_SHA256": "b77041324f0109f77161ee43743fe04baa485866af8460d31e476ad3f7648fd5",
    "XKBCOMMON_VERSION": "1.13.2",
    "XKBCOMMON_REPO": "https://github.com/xkbcommon/libxkbcommon.git",
    "XKBCOMMON_COMMIT": "d1442aaa6b635551182e83c3b55037f4c118c962",
    "KEYCODEMAPDB_REPO": "https://gitlab.com/keycodemap/keycodemapdb.git",
    "KEYCODEMAPDB_COMMIT": "54788039c0b3486a8883090d6736c2500177af29",
}

GENERATOR_VERSION = 1
RULES, MODEL = "evdev", "pc105"

# symbols/* files that are not Expat / Expat-Open-Group (TECH-079): excluded until a license review.
EXCLUDED_SYMBOL_FILES = {"lv": "HPND-sell-variant (ADR-024 TECH-079)"}

# Key slots, in the order of the tables (KBD_SLOT_* in kbd_layouts.h). Evdev key names.
SLOTS = (
    ["TLDE"] + [f"AE{i:02d}" for i in range(1, 14)]
    + [f"AD{i:02d}" for i in range(1, 13)] + ["BKSL"]
    + [f"AC{i:02d}" for i in range(1, 12)]
    + ["LSGT"] + [f"AB{i:02d}" for i in range(1, 12)]
    + ["SPCE", "MUHE", "HENK", "HKTG", "HNGL", "HJCV"]
)
# Every shipped layout must give these a Base-level symbol (the letter rows).
REQUIRED = [f"AD{i:02d}" for i in range(1, 11)] + [f"AC{i:02d}" for i in range(1, 10)] + [f"AB{i:02d}" for i in range(1, 8)]
IME_SLOTS = {"MUHE", "HENK", "HKTG", "HNGL", "HJCV"}
LEVELS = 4
MAX_LEGEND = 8  # bytes of UTF-8 (TECH-077)

GEOMETRIES = ["ANSI", "ISO", "JIS", "ABNT2", "KR"]
KEY_DEAD, KEY_IME = 0x01, 0x02  # kbd_key_t.flags; bit 4 + level: that level is a dead key
LAYOUT_ALTGR, LAYOUT_BEST_EFFORT = 0x01, 0x02  # kbd_layout_t.flags
USAGE_NON_US_HASH = 0x32  # ISO: the key next to Enter (HID usage tables)

KS_BACKSLASH, KS_LEVEL3 = 0x5C, 0xFE03
JP_IME_KEYSYMS = {0xFF22, 0xFF23, 0xFF27, 0xFF2A}  # Muhenkan, Henkan, Hiragana_Katakana, Zenkaku_Hankaku
KR_KEYSYMS = {0xFF31, 0xFF34}  # Hangul, Hangul_Hanja

# dead_* keysym name -> (legend, combining mark or None)
DEAD = {
    "dead_grave": ("`", 0x300), "dead_acute": ("´", 0x301), "dead_circumflex": ("^", 0x302),
    "dead_tilde": ("~", 0x303), "dead_macron": ("¯", 0x304), "dead_breve": ("˘", 0x306),
    "dead_abovedot": ("˙", 0x307), "dead_diaeresis": ("¨", 0x308), "dead_abovering": ("˚", 0x30A),
    "dead_doubleacute": ("˝", 0x30B), "dead_caron": ("ˇ", 0x30C), "dead_cedilla": ("¸", 0x327),
    "dead_ogonek": ("˛", 0x328), "dead_iota": ("ͺ", 0x345), "dead_belowdot": ("◌̣", 0x323),
    "dead_hook": ("◌̉", 0x309), "dead_horn": ("◌̛", 0x31B),
    "dead_doublegrave": ("◌̏", 0x30F), "dead_belowring": ("◌̥", 0x325),
    "dead_belowmacron": ("◌̱", 0x331), "dead_belowcircumflex": ("◌̭", 0x32D),
    "dead_belowtilde": ("◌̰", 0x330), "dead_belowbreve": ("◌̮", 0x32E),
    "dead_belowdiaeresis": ("◌̤", 0x324), "dead_invertedbreve": ("◌̑", 0x311),
    "dead_belowcomma": ("◌̦", 0x326), "dead_psili": ("◌̓", 0x313),
    "dead_dasia": ("◌̔", 0x314), "dead_stroke": ("/", None), "dead_currency": ("¤", None),
    "dead_greek": ("µ", None),
}

# Scripts drawn without shaping (TECH-078): such layouts ship as isolated glyphs and are marked best-effort.
SHAPING_RANGES = [
    (0x0600, 0x06FF), (0x0700, 0x074F), (0x0750, 0x077F), (0x0780, 0x07BF), (0x0900, 0x0DFF), (0x0E00, 0x0E7F),
    (0x0E80, 0x0EFF), (0x0F00, 0x0FFF), (0x1000, 0x109F), (0x1780, 0x17FF), (0x1800, 0x18AF), (0xA980, 0xA9DF),
]

C_HEADER = """\
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""


# ---- inputs ------------------------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class LayoutRef:
    layout: str
    variant: str
    name: str

    @property
    def id(self) -> str:
        return f"{self.layout}({self.variant})" if self.variant else self.layout


def read_base_xml(path: Path) -> list[LayoutRef]:
    """Every layout and variant of base.xml, with the English description, sorted by id (byte order)."""
    root = ET.parse(path).getroot()
    out: list[LayoutRef] = []
    for lay in root.iter("layout"):
        ci = lay.find("configItem")
        lname, ldesc = ci.findtext("name"), ci.findtext("description")
        out.append(LayoutRef(lname, "", ldesc))
        vl = lay.find("variantList")
        if vl is None:
            continue
        for var in vl.iter("variant"):
            vci = var.find("configItem")
            out.append(LayoutRef(lname, vci.findtext("name"), vci.findtext("description")))
    ids = [r.id for r in out]
    if len(set(ids)) != len(ids):
        dup = sorted({i for i in ids if ids.count(i) > 1})
        raise SystemExit(f"base.xml: duplicate ids {dup}")
    return sorted(out, key=lambda r: r.id.encode())


def read_keymaps_csv(path: Path) -> dict[str, int]:
    """XKB key name -> USB HID usage (page 0x07). A name with several usages keeps the lowest."""
    usages: dict[str, int] = {}
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            name, usb = (row.get("XKB key name") or "").strip(), (row.get("USB Keycodes") or "").strip()
            if not name or not usb:
                continue
            u = int(usb, 0)
            if 0 < u <= 0xFF:
                usages[name] = min(u, usages.get(name, 0x100))
    return usages


def reachable_symbol_files(symbols_dir: Path, first: str) -> set[str]:
    """The symbols files a layout can include, over-approximated: every include of every variant, recursively."""
    seen: set[str] = set()
    todo = [first]
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        p = symbols_dir / name
        if not p.is_file():
            continue
        for m in re.finditer(r'(?:include|augment|override|replace)\s+"([^"]+)"', p.read_text(encoding="utf-8")):
            for part in re.split(r"[+|^]", m.group(1)):
                f = part.split("(")[0].split(":")[0].strip()
                if f and f not in seen:
                    todo.append(f)
    return seen


# ---- libxkbcommon ------------------------------------------------------------------------------------------------------
class _RuleNames(ctypes.Structure):
    _fields_ = [(n, ctypes.c_char_p) for n in ("rules", "model", "layout", "variant", "options")]


class Xkb:
    """The few libxkbcommon calls the generator needs (xkbcommon.h, >= 1.13)."""

    def __init__(self, lib: str, xkb_root: str):
        L = self.L = ctypes.CDLL(lib)
        vp, u32, cp = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p
        L.xkb_context_new.restype, L.xkb_context_new.argtypes = vp, [ctypes.c_int]
        L.xkb_context_include_path_append.argtypes = [vp, cp]
        L.xkb_context_set_log_level.argtypes = [vp, ctypes.c_int]
        L.xkb_context_unref.argtypes = [vp]
        L.xkb_keymap_new_from_names.restype = vp
        L.xkb_keymap_new_from_names.argtypes = [vp, ctypes.POINTER(_RuleNames), ctypes.c_int]
        L.xkb_keymap_unref.argtypes = [vp]
        L.xkb_keymap_key_by_name.restype, L.xkb_keymap_key_by_name.argtypes = u32, [vp, cp]
        L.xkb_state_new.restype, L.xkb_state_new.argtypes = vp, [vp]
        L.xkb_state_unref.argtypes = [vp]
        L.xkb_state_update_key.argtypes = [vp, u32, ctypes.c_int]
        L.xkb_state_key_get_one_sym.restype, L.xkb_state_key_get_one_sym.argtypes = u32, [vp, u32]
        L.xkb_keysym_to_utf8.argtypes = [u32, ctypes.c_char_p, ctypes.c_size_t]
        L.xkb_keysym_get_name.argtypes = [u32, ctypes.c_char_p, ctypes.c_size_t]
        self.ctx = L.xkb_context_new(1 | 2)  # NO_DEFAULT_INCLUDES | NO_ENVIRONMENT_NAMES
        if not self.ctx or L.xkb_context_include_path_append(self.ctx, xkb_root.encode()) != 1:
            raise SystemExit(f"libxkbcommon: cannot use the include path {xkb_root}")
        L.xkb_context_set_log_level(self.ctx, 10)  # critical only

    def keysym_name(self, ks: int) -> str:
        buf = ctypes.create_string_buffer(64)
        n = self.L.xkb_keysym_get_name(ks, buf, 64)
        return buf.value.decode() if n > 0 else ""

    def keysym_utf8(self, ks: int) -> str:
        buf = ctypes.create_string_buffer(16)
        n = self.L.xkb_keysym_to_utf8(ks, buf, 16)
        return buf.value.decode("utf-8") if n > 1 else ""

    def compile(self, ref: LayoutRef, names: list[str]) -> dict[str, list[int]] | None:
        """{key name: [keysym per level]} for the keys that exist; RALT included; None when it does not compile."""
        L = self.L
        rn = _RuleNames(RULES.encode(), MODEL.encode(), ref.layout.encode(), ref.variant.encode(), None)
        km = L.xkb_keymap_new_from_names(self.ctx, ctypes.byref(rn), 0)
        if not km:
            return None
        try:
            codes = {n: L.xkb_keymap_key_by_name(km, n.encode()) for n in names + ["RALT", "RCTL", "LFSH"]}
            codes = {n: c for n, c in codes.items() if c != 0xFFFFFFFF}
            out: dict[str, list[int]] = {n: [0] * LEVELS for n in codes}
            for level, press in enumerate(([], ["LFSH"], ["RALT"], ["LFSH", "RALT"])):
                st = L.xkb_state_new(km)
                try:
                    for p in press:
                        if p in codes:
                            L.xkb_state_update_key(st, codes[p], 1)
                    for n, c in codes.items():
                        out[n][level] = L.xkb_state_key_get_one_sym(st, c)
                finally:
                    L.xkb_state_unref(st)
            return out
        finally:
            L.xkb_keymap_unref(km)


# ---- model -------------------------------------------------------------------------------------------------------------
@dataclass
class Key:
    slot: str
    usage: int
    legends: list[str]
    flags: int
    dead_marks: list[int | None] = field(default_factory=lambda: [None] * LEVELS)


@dataclass
class Layout:
    ref: LayoutRef
    geometry: str
    flags: int
    keys: dict[str, Key]
    chars: dict[str, list[list[int]]]


# Keys whose definition by the layout's own symbols (not by the pc / inet sections the rules add to every layout)
# selects the geometry family. Names as written in symbols/* (evdev aliases included).
JIS_KEYS = {"HZTG", "AE13", "HENK", "XFER", "MUHE", "NFER", "HKTG"}
KR_KEYS = {"HNGL", "HJCV"}
# Slots that exist only in some geometry families; every other slot exists in all of them.
GEOMETRY_SLOTS = {
    "LSGT": {"ISO", "ABNT2"},
    "AE13": {"JIS"},
    "AB11": {"JIS", "ABNT2"},
    "MUHE": {"JIS"}, "HENK": {"JIS"}, "HKTG": {"JIS"},
    "HNGL": {"KR"}, "HJCV": {"KR"},
}


class SymbolsIndex:
    """The key names each ``xkb_symbols`` section of symbols/* defines, following its includes (no keysyms)."""

    _BLOCK = re.compile(r'((?:\w+\s+)*)xkb_symbols\s+"([^"]+)"\s*\{')
    _INCLUDE = re.compile(r'\b(?:include|augment|override|replace)\s+"([^"]+)"')
    _KEY = re.compile(r"\bkey\s*<([A-Za-z0-9_+\-]+)>")

    def __init__(self, symbols_dir: Path):
        self.dir = symbols_dir
        self._files: dict[str, tuple[dict[str, str], str | None]] = {}
        self._memo: dict[tuple[str, str], frozenset[str]] = {}

    def _blocks(self, name: str) -> tuple[dict[str, str], str | None]:
        if name not in self._files:
            p = self.dir / name
            text = p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""
            text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
            text = re.sub(r"//[^\n]*", " ", text)
            blocks: dict[str, str] = {}
            default = None
            for m in self._BLOCK.finditer(text):
                depth, i = 1, m.end()
                while i < len(text) and depth:
                    depth += {"{": 1, "}": -1}.get(text[i], 0)
                    i += 1
                blocks.setdefault(m.group(2), text[m.end():i])
                if default is None and "default" in m.group(1).split():
                    default = m.group(2)
            if default is None and blocks:
                default = next(iter(blocks))
            self._files[name] = (blocks, default)
        return self._files[name]

    def defined_keys(self, file: str, variant: str, _stack: frozenset = frozenset()) -> frozenset[str]:
        blocks, default = self._blocks(file)
        section = variant or default or ""
        memo_key = (file, section)
        if memo_key in self._memo:
            return self._memo[memo_key]
        if memo_key in _stack or section not in blocks:
            return frozenset()
        body = blocks[section]
        keys = set(self._KEY.findall(body))
        for m in self._INCLUDE.finditer(body):
            for part in re.split(r"[+|^]", m.group(1)):
                part = part.split(":")[0].strip()
                if not part:
                    continue
                f, _, v = part.partition("(")
                keys |= self.defined_keys(f.strip(), v.rstrip(")").strip(), _stack | {memo_key})
        out = frozenset(keys)
        self._memo[memo_key] = out
        return out


def classify(syms: dict[str, list[int]], has: dict[str, bool], defined: frozenset[str] | None) -> str:
    """Geometry family. `defined`: the keys the layout's own symbols define (None: judge by keysyms only)."""
    if defined is not None:
        own_kr = any(k in defined and k in syms and syms[k][0] in KR_KEYSYMS for k in ("RALT", "RCTL"))
        if defined & JIS_KEYS:
            return "JIS"
        if defined & KR_KEYS or own_kr:
            return "KR"
        if "AB11" in defined:
            return "ABNT2"
    else:
        every = {s for v in syms.values() for s in v}
        if has.get("AE13") or every & JP_IME_KEYSYMS:
            return "JIS"
        if every & KR_KEYSYMS:
            return "KR"
        if has.get("AB11"):
            return "ABNT2"
    if "BKSL" in syms and syms["BKSL"][0] not in (0, KS_BACKSLASH):
        return "ISO"
    return "ANSI"


def build_layout(ref: LayoutRef, syms: dict[str, list[int]], usages: dict[str, int], xkb,
                 defined: frozenset[str] | None = None) -> tuple[Layout | None, str]:
    altgr = "RALT" in syms and syms["RALT"][0] == KS_LEVEL3
    levels = LEVELS if altgr else 2
    has = {s: s in syms and syms[s][0] != 0 for s in SLOTS}
    for s in REQUIRED:
        if not has[s]:
            return None, f"no Base symbol on {s}"
    geometry = classify({s: v for s, v in syms.items() if s in SLOTS or s in ("RALT", "RCTL")}, has, defined)
    keys: dict[str, Key] = {}
    best_effort = False
    for s in SLOTS:
        if not has[s] or s not in usages or geometry not in GEOMETRY_SLOTS.get(s, {geometry}):
            continue
        usage = usages[s]
        if s == "BKSL" and geometry == "ISO":
            usage = USAGE_NON_US_HASH
        legends, flags, marks = [""] * LEVELS, 0, [None] * LEVELS
        for lv in range(levels):
            ks = syms[s][lv]
            if ks == 0:
                continue
            name = xkb.keysym_name(ks)
            if name.startswith("dead_"):
                legend, mark = DEAD.get(name, ("◌", None))
                legends[lv], marks[lv] = legend, mark
                flags |= KEY_DEAD | (0x10 << lv)
                continue
            if ks in JP_IME_KEYSYMS or ks in KR_KEYSYMS:
                flags |= KEY_IME
            text = xkb.keysym_utf8(ks)
            if text and all(unicodedata.category(c) not in ("Cc", "Cs", "Co") for c in text):
                legends[lv] = text
        if s in IME_SLOTS and not flags & KEY_IME:
            continue  # an IME position with no IME keysym is not part of this layout
        for t in legends:
            if len(t.encode()) > MAX_LEGEND:
                return None, f"legend longer than {MAX_LEGEND} bytes on {s}"
            best_effort |= any(lo <= ord(c) <= hi for c in t for lo, hi in SHAPING_RANGES)
        keys[s] = Key(s, usage, legends, flags, marks)
    flags = (LAYOUT_ALTGR if altgr else 0) | (LAYOUT_BEST_EFFORT if best_effort else 0)
    return Layout(ref, geometry, flags, keys, char_index(keys)), ""


def char_index(keys: dict[str, Key]) -> dict[str, list[list[int]]]:
    """Character -> keystrokes [[usage, mods], ...] (mods: 1 Shift, 2 AltGr). Lower level and earlier slot win."""
    direct: dict[str, list[list[int]]] = {}
    for lv in range(LEVELS):
        for s in SLOTS:
            k = keys.get(s)
            if k is None or k.dead_marks[lv] is not None:
                continue
            t = k.legends[lv]
            if len(t) == 1 and t not in direct and (ord(t) >= 0x20 and ord(t) != 0x7F):
                direct[t] = [[k.usage, lv]]
    out = dict(direct)
    for lv in range(LEVELS):
        for s in SLOTS:
            k = keys.get(s)
            if k is None or k.dead_marks[lv] is None:
                continue
            for base, strokes in sorted(direct.items()):
                if not unicodedata.category(base).startswith("L"):
                    continue
                r = unicodedata.normalize("NFC", base + chr(k.dead_marks[lv]))
                if len(r) == 1 and r not in out:
                    out[r] = [[k.usage, lv]] + strokes
    return dict(sorted(out.items()))


# ---- outputs -----------------------------------------------------------------------------------------------------------
def c_str(s: str) -> str:
    out = []
    for b in s.encode():
        out.append(chr(b) if 0x20 <= b < 0x7F and chr(b) not in '"\\?' else f"\\{b:03o}")
    return '"' + "".join(out) + '"'


class Pool:
    def __init__(self):
        self.data = bytearray(b"\0")  # offset 0 = empty string
        self.index = {"": 0}

    def add(self, s: str) -> int:
        if s not in self.index:
            self.index[s] = len(self.data)
            self.data += s.encode() + b"\0"
        return self.index[s]


def emit_c(layouts: list[Layout], usages: dict[str, int], provenance: list[str]) -> str:
    legends, names = Pool(), Pool()
    recs: list[tuple] = []
    rec_index: dict[tuple, int] = {}
    slot_rows: list[list[int]] = []
    for lay in layouts:
        row = []
        for s in SLOTS:
            k = lay.keys.get(s)
            if k is None:
                row.append(0xFFFF)
                continue
            r = (k.usage, k.flags, tuple(legends.add(t) for t in k.legends))
            if r not in rec_index:
                rec_index[r] = len(recs)
                recs.append(r)
            row.append(rec_index[r])
        slot_rows.append(row)
    if len(legends.data) > 0xFFFF or len(recs) >= 0xFFFF:
        raise SystemExit("kbd_layouts: legend pool or key records exceed 16-bit indices")
    lines = [C_HEADER, "//"] + [f"// {p}" for p in provenance] + [
        "// GENERATED by tools/kbd_layouts/gen_layouts.py - do not edit (TECH-077).",
        '#include "kbd_layouts.h"',
        "",
        f'_Static_assert(KBD_SLOT_COUNT == {len(SLOTS)}, "kbd_layouts.h and the generated tables disagree on the key slots");',
        "",
        f"const uint16_t kbd_layouts_count = {len(layouts)}u;",
        "",
        f"const char kbd_layouts_legend_pool[{len(legends.data)}] =",
    ]
    lines += chunk_bytes(legends.data)
    lines += ["", f"const kbd_key_t kbd_layouts_keys[{len(recs)}] = {{"]
    for u, f, lg in recs:
        lines.append(f"    {{0x{u:02x}u, 0x{f:02x}u, {{{', '.join(f'{o}u' for o in lg)}}}}},")
    lines += ["};", "", f"const uint16_t kbd_layouts_slots[{len(layouts)}][KBD_SLOT_COUNT] = {{"]
    for lay, row in zip(layouts, slot_rows):
        lines.append(f"    /* {lay.ref.id} */ {{{', '.join(f'0x{v:04x}u' for v in row)}}},")
    lines += ["};", "", f"const kbd_layout_t kbd_layouts_table[{len(layouts)}] = {{"]
    for i, lay in enumerate(layouts):
        lines.append(f"    {{{c_str(lay.ref.id)}, {c_str(lay.ref.name)}, KBD_GEOM_{lay.geometry}, 0x{lay.flags:02x}u, {i}u}},")
    lines += ["};", ""]
    return "\n".join(lines)


def chunk_bytes(data: bytes) -> list[str]:
    out, cur = [], ""
    for b in data:
        piece = "\\000" if b == 0 else (chr(b) if 0x20 <= b < 0x7F and chr(b) not in '"\\?' else f"\\{b:03o}")
        if len(cur) + len(piece) > 100:
            out.append(f'    "{cur}"')
            cur = ""
        cur += piece
    out.append(f'    "{cur}";')
    return out


def emit_json(layouts: list[Layout], provenance: list[str]) -> str:
    doc = {
        "provenance": provenance,
        "mods": {"1": "Shift", "2": "AltGr"},
        "layouts": [
            {
                "id": lay.ref.id,
                "name": lay.ref.name,
                "geometry": lay.geometry,
                "altgr": bool(lay.flags & LAYOUT_ALTGR),
                "best_effort": bool(lay.flags & LAYOUT_BEST_EFFORT),
                "keys": {s: {"usage": k.usage, "legends": k.legends, "dead": [m is not None for m in k.dead_marks]}
                         for s, k in lay.keys.items()},
                "chars": lay.chars,
            }
            for lay in layouts
        ],
    }
    return json.dumps(doc, ensure_ascii=False, sort_keys=False, separators=(",", ":")) + "\n"


def emit_report(layouts: list[Layout], rejected: list[tuple[str, str]], c_size: int, provenance: list[str]) -> str:
    by_geom = {g: sum(1 for x in layouts if x.geometry == g) for g in GEOMETRIES}
    lines = ["# Generated keyboard layouts", "", "Generated by `tools/kbd_layouts/gen_layouts.py`; do not edit.", ""]
    lines += [f"- {p}" for p in provenance]
    lines += ["", f"Shipped: {len(layouts)} layouts ("
              + ", ".join(f"{g} {n}" for g, n in by_geom.items()) + f"); rejected: {len(rejected)}.",
              f"DEVICE table source: {c_size} bytes of C.", "",
              "## Best-effort (no shaping, TECH-078)", ""]
    be = [x.ref.id for x in layouts if x.flags & LAYOUT_BEST_EFFORT]
    lines += [", ".join(f"`{i}`" for i in be) if be else "None.", "", "## Rejected", "", "| Layout | Reason |", "|---|---|"]
    lines += [f"| `{i}` | {r} |" for i, r in rejected]
    lines += ["", "## Shipped", "", "| Layout | Name | Geometry | AltGr |", "|---|---|---|---|"]
    lines += [f"| `{x.ref.id}` | {x.ref.name} | {x.geometry} | {'yes' if x.flags & LAYOUT_ALTGR else 'no'} |" for x in layouts]
    return "\n".join(lines) + "\n"


def emit_codepoints(layouts: list[Layout]) -> str:
    cps = set(range(0x20, 0x7F))
    for lay in layouts:
        for k in lay.keys.values():
            for t in k.legends:
                cps.update(ord(c) for c in t)
    return "".join(f"U+{c:04X}\n" for c in sorted(cps))


# ---- driver ------------------------------------------------------------------------------------------------------------
def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(refs: list[LayoutRef], usages: dict[str, int], xkb, symbols_dir: Path | None,
             provenance: list[str]) -> dict[str, str]:
    missing = [s for s in SLOTS if s not in usages]
    if missing:
        raise SystemExit(f"keymaps.csv has no usage for {missing}")
    layouts: list[Layout] = []
    rejected: list[tuple[str, str]] = []
    index = SymbolsIndex(symbols_dir) if symbols_dir is not None else None
    for ref in refs:
        if symbols_dir is not None:
            bad = sorted(reachable_symbol_files(symbols_dir, ref.layout) & EXCLUDED_SYMBOL_FILES.keys())
            if bad:
                rejected.append((ref.id, "license: " + ", ".join(f"symbols/{b} {EXCLUDED_SYMBOL_FILES[b]}" for b in bad)))
                continue
        syms = xkb.compile(ref, SLOTS)
        if syms is None:
            rejected.append((ref.id, "does not compile"))
            continue
        defined = index.defined_keys(ref.layout, ref.variant) if index is not None else None
        lay, why = build_layout(ref, syms, usages, xkb, defined)
        if lay is None:
            rejected.append((ref.id, why))
        else:
            layouts.append(lay)
    c = emit_c(layouts, usages, provenance)
    return {
        "components/kbd_layouts/generated/kbd_layouts_data.c": c,
        "components/kbd_layouts/generated/REPORT.md": emit_report(layouts, rejected, len(c.encode()), provenance),
        "components/kbd_layouts/generated/legend_codepoints.txt": emit_codepoints(layouts),
        "clients/windows/layouts/kbd_layouts.json": emit_json(layouts, provenance),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--print-pins", action="store_true", help="print the pinned inputs as KEY=value lines and exit")
    ap.add_argument("--xkb-root", type=Path, help="installed xkeyboard-config data (rules/, symbols/, ...)")
    ap.add_argument("--xkc-tarball", type=Path, help="the xkeyboard-config tarball, to record its SHA256")
    ap.add_argument("--keymaps-csv", type=Path, help="keycodemapdb data/keymaps.csv")
    ap.add_argument("--libxkbcommon", help="path of the libxkbcommon shared library")
    ap.add_argument("--out-root", type=Path, help="where the output tree is written (default: the repository root)")
    a = ap.parse_args(argv)
    if a.print_pins:
        for k, v in PINS.items():
            print(f"{k}={v}")
        return 0
    if not (a.xkb_root and a.keymaps_csv and a.libxkbcommon):
        ap.error("--xkb-root, --keymaps-csv and --libxkbcommon are required")
    if a.xkc_tarball is not None and sha256(a.xkc_tarball) != PINS["XKC_SHA256"]:
        raise SystemExit("the xkeyboard-config tarball does not match the pinned SHA256")
    me = Path(__file__).resolve()
    out_root = a.out_root or me.parents[2]
    gen_sha = hashlib.sha256(me.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    provenance = [
        f"xkeyboard-config {PINS['XKC_VERSION']} sha256 {PINS['XKC_SHA256']} (rules/base.xml, rules={RULES}, model={MODEL})",
        f"libxkbcommon {PINS['XKBCOMMON_VERSION']} commit {PINS['XKBCOMMON_COMMIT']} (build-time tool, not shipped)",
        f"keycodemapdb commit {PINS['KEYCODEMAPDB_COMMIT']} data/keymaps.csv sha256 {sha256(a.keymaps_csv)} (BSD-3-Clause option)",
        f"generator v{GENERATOR_VERSION} sha256 {gen_sha}",
        "command: tools/kbd_layouts/gen_layouts.py --xkb-root <xkeyboard-config> --keymaps-csv <keymaps.csv> --libxkbcommon <lib>",
    ]
    refs = read_base_xml(a.xkb_root / "rules" / "base.xml")
    usages = read_keymaps_csv(a.keymaps_csv)
    xkb = Xkb(a.libxkbcommon, str(a.xkb_root))
    files = generate(refs, usages, xkb, a.xkb_root / "symbols", provenance)
    for rel, text in files.items():
        p = out_root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(text.encode("utf-8"))
        print(f"wrote {rel} ({len(text.encode())} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
