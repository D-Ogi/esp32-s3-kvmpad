#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Host tests of gen_layouts.py without libxkbcommon (AT-EXT-22, the parts that run on any host).

A fake keymap compiler stands in for libxkbcommon with five small layouts (ANSI, ISO with dead keys and AltGr, JIS,
KR, ABNT2) and one broken one. Checks: geometry rule, usages, dead-key index, rejection, determinism, base.xml and
keymaps.csv parsing, the license include walk. ``--emit <dir>`` also writes the fake C tables for test/kbd_layouts.

    python -I tools/kbd_layouts/test_gen_layouts.py [--emit <dir>]
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

_spec = importlib.util.spec_from_file_location("gen_layouts", Path(__file__).with_name("gen_layouts.py"))
gl = importlib.util.module_from_spec(_spec)
sys.modules["gen_layouts"] = gl
_spec.loader.exec_module(gl)

# ---- fake libxkbcommon -------------------------------------------------------------------------------------------------
NAMES = {
    0xFE51: "dead_acute", 0xFE50: "dead_grave", 0xFE03: "ISO_Level3_Shift", 0xFFEA: "Alt_R", 0xFF22: "Muhenkan",
    0xFF23: "Henkan", 0xFF27: "Hiragana_Katakana", 0xFF2A: "Zenkaku_Hankaku", 0xFF31: "Hangul", 0xFF34: "Hangul_Hanja",
    0xFFE1: "Shift_L", 0xFFE4: "Control_R",
}
US_ROWS = {
    "TLDE": "`~", "AE01": "1!", "AE02": "2@", "AE03": "3#", "AE04": "4$", "AE05": "5%", "AE06": "6^", "AE07": "7&",
    "AE08": "8*", "AE09": "9(", "AE10": "0)", "AE11": "-_", "AE12": "=+", "AD11": "[{", "AD12": "]}", "BKSL": "\\|",
    "AC10": ";:", "AC11": "'\"", "LSGT": "<>", "AB08": ",<", "AB09": ".>", "AB10": "/?", "SPCE": "  ",
}
for row, letters in (("AD", "qwertyuiop"), ("AC", "asdfghjkl"), ("AB", "zxcvbnm")):
    for i, ch in enumerate(letters, 1):
        US_ROWS[f"{row}{i:02d}"] = ch + ch.upper()


def _syms(text: str) -> list[int]:
    return [ord(c) for c in text] + [0] * (4 - len(text))


def _us() -> dict[str, list[int]]:
    d = {k: _syms(v) for k, v in US_ROWS.items()}
    d.update({"RALT": [0xFFEA] * 4, "RCTL": [0xFFE4] * 4, "LFSH": [0xFFE1] * 4})
    return d


def _de() -> dict[str, list[int]]:
    d = _us()
    d["RALT"] = [0xFE03] * 4
    d["BKSL"] = _syms("#'")
    d["AE12"] = [0xFE51, 0xFE50, 0, 0]  # dead acute / dead grave
    d["AE02"] = [ord("2"), ord('"'), 0xB2, 0]
    d["AD01"] = [ord("q"), ord("Q"), ord("@"), 0]
    d["AD06"], d["AB01"] = _syms("zZ"), _syms("yY")
    d["AC10"], d["AC11"], d["AD11"] = _syms("öÖ"), _syms("äÄ"), _syms("üÜ")
    d["AD12"] = [ord("+"), ord("*"), ord("~"), 0]
    return d


def _jp() -> dict[str, list[int]]:
    d = _us()
    d["TLDE"] = [0xFF2A] * 2 + [0, 0]
    d["AE13"], d["AB11"] = _syms("¥|"), _syms("\\_")
    d["MUHE"], d["HENK"], d["HKTG"] = [0xFF22, 0, 0, 0], [0xFF23, 0, 0, 0], [0xFF27, 0, 0, 0]
    return d


def _kr() -> dict[str, list[int]]:
    d = _us()
    d["RALT"], d["RCTL"] = [0xFF31] * 4, [0xFF34] * 4
    d["HNGL"], d["HJCV"] = [0xFF31, 0, 0, 0], [0xFF34, 0, 0, 0]
    return d


def _br() -> dict[str, list[int]]:
    d = _us()
    d["AB11"] = _syms("/?")
    return d


def _broken() -> dict[str, list[int]]:
    d = _us()
    del d["AD05"]
    return d


FAKE = {"br": _br, "de": _de, "jp": _jp, "kr": _kr, "us": _us, "zz": _broken}


class FakeXkb:
    def keysym_name(self, ks: int) -> str:
        return NAMES.get(ks, f"U{ks:04X}")

    def keysym_utf8(self, ks: int) -> str:
        if 0x20 <= ks <= 0xFF:
            return chr(ks)
        if ks & 0xFF000000 == 0x01000000:
            return chr(ks & 0xFFFFFF)
        return ""

    def compile(self, ref, names):
        f = FAKE.get(ref.layout)
        return None if f is None else {k: v for k, v in f().items() if k in names or k in ("RALT", "RCTL", "LFSH")}


USAGES = {"TLDE": 0x35, "AE13": 0x89, "AE11": 0x2D, "AE12": 0x2E, "AD11": 0x2F, "AD12": 0x30, "BKSL": 0x31,
          "AC10": 0x33, "AC11": 0x34, "LSGT": 0x64, "AB08": 0x36, "AB09": 0x37, "AB10": 0x38, "AB11": 0x87,
          "SPCE": 0x2C, "MUHE": 0x8B, "HENK": 0x8A, "HKTG": 0x88, "HNGL": 0x90, "HJCV": 0x91}
USAGES.update({f"AE{i:02d}": 0x1E + i - 1 for i in range(1, 11)})
for _row, _letters in (("AD", "qwertyuiop"), ("AC", "asdfghjkl"), ("AB", "zxcvbnm")):
    USAGES.update({f"{_row}{i:02d}": 0x04 + ord(c) - ord("a") for i, c in enumerate(_letters, 1)})

REFS = [gl.LayoutRef(n, "", f"Fake {n}") for n in sorted(FAKE)] + [gl.LayoutRef("xx", "", "Does not compile")]
PROV = ["fake inputs (test_gen_layouts.py)"]


def run_fake() -> dict[str, str]:
    return gl.generate(sorted(REFS, key=lambda r: r.id.encode()), USAGES, FakeXkb(), None, PROV)


class GeneratorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = run_fake()
        import json
        cls.doc = json.loads(cls.files["clients/windows/layouts/kbd_layouts.json"])
        cls.by_id = {x["id"]: x for x in cls.doc["layouts"]}

    def test_geometry_rule(self):
        got = {i: x["geometry"] for i, x in self.by_id.items()}
        self.assertEqual(got, {"br": "ABNT2", "de": "ISO", "jp": "JIS", "kr": "KR", "us": "ANSI"})

    def test_rejected(self):
        rep = self.files["components/kbd_layouts/generated/REPORT.md"]
        self.assertIn("| `zz` | no Base symbol on AD05 |", rep)
        self.assertIn("| `xx` | does not compile |", rep)

    def test_usages(self):
        self.assertEqual(self.by_id["us"]["keys"]["BKSL"]["usage"], 0x31)
        self.assertEqual(self.by_id["de"]["keys"]["BKSL"]["usage"], 0x32)  # ISO: Non-US #
        self.assertEqual(self.by_id["jp"]["keys"]["MUHE"]["usage"], 0x8B)
        self.assertNotIn("MUHE", self.by_id["us"]["keys"])  # an IME position without an IME keysym is dropped
        self.assertEqual(self.by_id["kr"]["keys"]["HNGL"]["usage"], 0x90)

    def test_levels_and_dead_keys(self):
        de = self.by_id["de"]
        self.assertTrue(de["altgr"])
        self.assertFalse(self.by_id["us"]["altgr"])
        self.assertEqual(de["keys"]["AE12"]["legends"][:2], ["´", "`"])
        self.assertEqual(de["keys"]["AE12"]["dead"], [True, True, False, False])
        chars = de["chars"]
        self.assertEqual(chars["@"], [[0x14, 2]])  # AltGr+Q
        self.assertEqual(chars["é"], [[0x2E, 0], [0x08, 0]])  # dead acute, e
        self.assertEqual(chars["È"], [[0x2E, 1], [0x08, 1]])  # Shift+dead grave, Shift+E
        self.assertNotIn("´", chars)  # the accent itself is not one keystroke
        self.assertEqual(self.by_id["us"]["chars"]["A"], [[0x04, 1]])
        self.assertNotIn("²", self.by_id["us"]["chars"])

    def test_c_tables(self):
        c = self.files["components/kbd_layouts/generated/kbd_layouts_data.c"]
        self.assertTrue(c.startswith("// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0\n"))
        self.assertIn("const uint16_t kbd_layouts_count = 5u;", c)
        self.assertIn("_Static_assert(KBD_SLOT_COUNT == 56,", c)
        self.assertIn('{"de", "Fake de", KBD_GEOM_ISO, 0x01u, 1u}', c)
        self.assertNotIn("\r", c)

    def test_deterministic(self):
        self.assertEqual(run_fake(), self.files)

    def test_codepoints(self):
        cps = self.files["components/kbd_layouts/generated/legend_codepoints.txt"].split()
        self.assertIn("U+00A5", cps)
        self.assertIn("U+0041", cps)
        self.assertEqual(cps, sorted(cps, key=lambda s: int(s[2:], 16)))


class InputTests(unittest.TestCase):
    def test_base_xml(self):
        xml = """<?xml version="1.0"?><xkbConfigRegistry><layoutList>
<layout><configItem><name>us</name><description>English (US)</description></configItem>
<variantList><variant><configItem><name>intl</name><description>English (US, intl.)</description></configItem></variant>
</variantList></layout>
<layout><configItem><name>de</name><description>German</description></configItem></layout>
</layoutList></xkbConfigRegistry>"""
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "base.xml"
            p.write_text(xml, encoding="utf-8")
            refs = gl.read_base_xml(p)
        self.assertEqual([r.id for r in refs], ["de", "us", "us(intl)"])
        self.assertEqual(refs[2].name, "English (US, intl.)")

    def test_keymaps_csv(self):
        text = ('"Linux Name","USB Keycodes","XKB key name"\n'
                "KEY_BACKSLASH,49,BKSL\nKEY_BACKSLASH,50,BKSL\nKEY_1,30,AE01\nKEY_X,,AE02\nKEY_Y,300,AE03\n")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "k.csv"
            p.write_text(text, encoding="utf-8")
            self.assertEqual(gl.read_keymaps_csv(p), {"BKSL": 49, "AE01": 30})

    def test_license_walk(self):
        with tempfile.TemporaryDirectory() as d:
            s = Path(d)
            (s / "aa").write_text('xkb_symbols "basic" { include "bb(x)" };', encoding="utf-8")
            (s / "bb").write_text('xkb_symbols "x" { include "lv(basic)+cc" };', encoding="utf-8")
            (s / "lv").write_text("", encoding="utf-8")
            (s / "cc").write_text("", encoding="utf-8")
            (s / "dd").write_text('xkb_symbols "basic" { include "cc" };', encoding="utf-8")
            self.assertEqual(gl.reachable_symbol_files(s, "aa"), {"aa", "bb", "lv", "cc"})
            self.assertEqual(gl.reachable_symbol_files(s, "dd"), {"dd", "cc"})

    def test_symbols_index(self):
        with tempfile.TemporaryDirectory() as d:
            s = Path(d)
            (s / "us").write_text('// c\ndefault partial alphanumeric_keys\nxkb_symbols "basic" {\n'
                                  '  key <AE01> { [ 1, exclam ] }; key <BKSL> { [ backslash, bar ] };\n};\n'
                                  'partial xkb_symbols "intl" { include "us(basic)" key <AC01> { [ a ] }; };\n'
                                  '/* key <MUHE> in a comment */\n', encoding="utf-8")
            (s / "jp").write_text('default xkb_symbols "106" { include "jp(common)" key <AE13> { [ yen, bar ] }; };\n'
                                  'xkb_symbols "common" { key <HZTG> { [ Zenkaku_Hankaku ] }; '
                                  'include "jp(hztg_escape)+kpdl(dot)" };\n'
                                  'xkb_symbols "hztg_escape" { key <HZTG> { [ Escape ] }; };\n', encoding="utf-8")
            (s / "kr").write_text('default xkb_symbols "kr106" { include "us" include "kr(hw_keys)" };\n'
                                  'xkb_symbols "hw_keys" { key <HNGL> { [ Hangul ] }; key <HJCV> { [ Hangul_Hanja ] }; };\n',
                                  encoding="utf-8")
            (s / "br").write_text('xkb_symbols "abnt2" { include "latin" key <AB11> { [ slash, question ] }; };\n',
                                  encoding="utf-8")
            (s / "loop").write_text('xkb_symbols "a" { include "loop(b)" key <AE02> {[2]}; };\n'
                                    'xkb_symbols "b" { include "loop(a)" };\n', encoding="utf-8")
            ix = gl.SymbolsIndex(s)
            self.assertEqual(ix.defined_keys("us", ""), {"AE01", "BKSL"})
            self.assertEqual(ix.defined_keys("us", "intl"), {"AE01", "BKSL", "AC01"})
            self.assertEqual(ix.defined_keys("jp", ""), {"AE13", "HZTG"})
            self.assertEqual(ix.defined_keys("kr", ""), {"AE01", "BKSL", "HNGL", "HJCV"})
            self.assertEqual(ix.defined_keys("br", ""), {"AB11"})
            self.assertEqual(ix.defined_keys("loop", "a"), {"AE02"})
            self.assertEqual(ix.defined_keys("missing", ""), frozenset())

    def test_geometry_from_defined_keys(self):
        # inet(evdev) puts IME keysyms on every layout: the keys the layout itself defines decide, not the keysyms.
        us = {k: v for k, v in _us().items()}
        us.update({"MUHE": [0xFF22, 0, 0, 0], "HNGL": [0xFF31, 0, 0, 0]})
        has = {s: s in us and us[s][0] != 0 for s in gl.SLOTS}
        self.assertEqual(gl.classify(us, has, frozenset({"AE01", "BKSL"})), "ANSI")
        self.assertEqual(gl.classify(us, has, frozenset({"AE01", "HNGL"})), "KR")
        self.assertEqual(gl.classify(us, has, frozenset({"HZTG"})), "JIS")
        self.assertEqual(gl.classify(us, has, frozenset({"AB11"})), "ABNT2")
        lay, _ = gl.build_layout(gl.LayoutRef("us", "", "US"), us, USAGES, FakeXkb(), frozenset({"AE01"}))
        self.assertEqual(lay.geometry, "ANSI")
        self.assertNotIn("MUHE", lay.keys)
        self.assertNotIn("HNGL", lay.keys)
        self.assertNotIn("LSGT", lay.keys)

    def test_pins(self):
        self.assertEqual(len(gl.PINS["XKC_SHA256"]), 64)
        self.assertEqual(len(gl.PINS["XKBCOMMON_COMMIT"]), 40)
        self.assertEqual(len(gl.PINS["KEYCODEMAPDB_COMMIT"]), 40)
        self.assertEqual(len(gl.SLOTS), 56)


def main() -> int:
    args = sys.argv[1:]
    if args[:1] == ["--emit"]:
        out = Path(args[1])
        out.mkdir(parents=True, exist_ok=True)
        (out / "kbd_layouts_data.c").write_bytes(run_fake()["components/kbd_layouts/generated/kbd_layouts_data.c"].encode())
        return 0
    prog = unittest.main(argv=[sys.argv[0], "-v"], exit=False)
    return 0 if prog.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
