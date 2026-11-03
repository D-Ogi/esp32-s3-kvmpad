# `kbd_layouts`

> **Status:** `IN PROGRESS` — the Unicode delivery sequences (`include/kbd_unicode.h`, FR-747: `linux`, `macos`,
> `windows-hex`, `wincompose`, cancel keys, UTF-16 surrogates) are implemented and host-tested (`test/ime`). The layout
> accessors (`include/kbd_layouts.h`: list, find by xkb id, key per slot with usage / four legends / dead and IME flags,
> geometry family, one-keystroke character lookup and a `kvm_uni_key_fn` adapter) and the generator
> (`tools/kbd_layouts/gen_layouts.py`) are host-tested with fake tables (`test/kbd_layouts`). `generated/` comes from the
> CI workflow `kbd-layouts` (libxkbcommon needs Linux); until it is committed the component builds with zero layouts and
> `kvm_uni_key_us` stays the only resolver. The legend font subsets (TECH-078,
> `components/ui_theme/fonts/kvm_font_legend_{24,16,14}.c`), the `host_layout` field of the Settings snapshot and the
> KEYBOARD view and Settings integration (FR-741/742) are in place: `Settings → Keyboard → Host layout` picks an entry
> of these tables and the KEYBOARD view draws its legends and geometry family from it, with the HID usage of each key
> unchanged (FR-743).

**Role:** generated keyboard layout data (ADR-024): for every `layout(variant)` from xkeyboard-config the key → HID usage map,
legends per level, geometry family (ANSI / ISO / JIS / ABNT2 / KR), IME keys, the character → keystroke reverse index and the
Unicode delivery sequences (`linux` / `macos` / `windows-hex` / `wincompose`); a small accessor API over tables in flash.

| | |
|---|---|
| Tasks | TASK-071 (generator + component + legend fonts), TASK-070B (keyboard view integration), TASK-105 (CLIENT tables) |
| Specification | 01 §4 (FR-201/202/508), 03 §5.2, 07 §7; ADR-024 (FR-741..745, FR-747, TECH-077..079) |
| Tests | AT-EXT-22..25, AT-KEY-01..10 |

**Rules (MUST):**

- `generated/` is produced by `tools/kbd_layouts/gen_layouts.py` from a pinned xkeyboard-config release and a pinned keycodemapdb
  commit; it carries a provenance header and is never edited by hand; CI regenerates it and requires an empty diff (TECH-077).
- A key is a usage; legends are decoration (FR-743). No keymap is derived from key text (07 §7).
- Zero allocation at runtime: tables are read in place from flash by index.
- Only Expat / Expat-Open-Group `symbols/*` sources; keycodemapdb under its BSD-3-Clause option; every source listed in
  `THIRD_PARTY_NOTICES.md` (TECH-079).
- No `lvgl.h`, no `tusb.h`; pure C, testable on the host.

Governing documents: `AGENTS.md`.
