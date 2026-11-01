# ADR-024 — Keyboard layouts generated from xkeyboard-config (host-layout mirroring, legends, geometry, type-text tables)
Status: ACCEPTED 2026-11-01 (owner); engine footprint PENDING-HW until the TASK-072 spike
Date: 2026-11-01
Acceptance notes: on-device IME is in scope of the main release (not a later milestone); HOST layouts come from xkeyboard-config, not from Android KCM files; AOSP is the source for the IME engines.
Approver: project owner (direction of 2026-11-01: "switching keyboards between countries — Japanese, Simplified Chinese, English variants; take the layouts from Linux and stay compatible with them; do not reinvent, reuse recognized, solid, permissively licensed solutions")

## Context

- **What the HOST sees.** A USB HID keyboard sends *usages* (page 0x07), never characters (FR-202, 03 §3 item 1). The HOST
  turns a usage into a character according to the layout selected **on the HOST**. The DEVICE cannot change that layout; it can
  only mirror it (and, as a convenience, send the OS's layout-switch shortcut). FR-508 already states this: "the host layout
  remains on the OS side".
- **What the DEVICE decides.** The on-screen keyboard's *geometry* (which physical keys exist: ANSI 101/104, ISO 102/105 with
  the `<>` key, JIS 106/109 with 無変換/変換/カタカナ/半角全角/¥/ろ, ABNT2, Korean 103/106 with 한/영 and 한자) and its *legends*
  (what is printed on each key at each level: Base / Shift / AltGr / Shift+AltGr). Today the spec fixes QWERTY (FR-201) and
  `UiKeyboardGrid` with "US/PL and Fn layers" (07 §7); the CLIENT needs per-layout character→keystroke tables for *type text*
  (ADR-019, FR-702: start with `US`, `PL-Programmer`).
- **CJK and other complex scripts — two separable problems.** (1) *Composition*: turning taps into Japanese / Chinese / Korean
  text (kana → kanji conversion, pinyin → hanzi candidates, jamo → syllables). (2) *Delivery*: getting Unicode text into the HOST
  through a USB HID keyboard, which by protocol carries no characters at all. Composition can be done entirely on the DEVICE
  (owner requirement of 2026-11-01: the on-screen keyboard is the primary input path; using the HOST's own IME must never be
  mandatory — this supersedes the v1 exclusion of "an on-screen keyboard with IME/Unicode" in 01 §1.2). Delivery has **no
  OS-independent method**: every operating system has its own Unicode entry sequence or needs its own IME driven by keystrokes.
  The recognized prior art is the Unicode input feature of the QMK keyboard firmware, which offers per-OS *input modes* selected
  by the user: Linux/IBus `Ctrl+Shift+U <hex> Space` (works out of the box in GNOME/KDE), macOS "Unicode Hex Input" source
  (`Option+<hex>`, enabled once on the HOST, surrogate pairs above U+FFFF), Windows HexNumpad (`Alt` held + Numpad `+` + hex;
  one-time registry value `EnableHexNumpad=1`), WinCompose (`Right Alt`, `u`, hex, `Enter`; user-installed), Emacs. The same
  approach is adopted here. The HOST's native IME remains available as one more delivery mode (readings sent as keystrokes,
  candidates on the HOST screen), using the IME keys of the geometry (JIS: `Zenkaku/Hankaku` on usage 0x35, `Henkan` 0x8A,
  `Muhenkan` 0x8B, `Katakana/Hiragana` 0x88, `¥` 0x89, `ろ` 0x87; Korean: `Hangul` 0x90, `Hanja` 0x91). All these usages fit the
  boot-protocol 1-byte key array, so 03 §5.1 descriptors do not change.
- **Embedded IME engines with permissive licenses** (verified 2026-11-01; footprint to be measured): **OpenWnn** (OMRON, Japanese,
  AOSP `packages/inputmethods/OpenWnn`, Apache-2.0, native dictionary engine designed for low-resource phones); **PinyinIME**
  (AOSP `packages/inputmethods/PinyinIME`, Apache-2.0, native engine + compact dictionary); Korean needs no dictionary — Hangul
  syllables are composed algorithmically (Unicode: `S = 0xAC00 + (L×21 + V)×28 + T`). GPL/LGPL engines and dictionaries (Anthy,
  libkkc, SKK-JISYO, libpinyin, librime schemas with incompatible terms) are excluded.
- **Established sources with permissive licenses** (verified 2026-11-01; per-file verification is part of the implementing task):
  - **xkeyboard-config** (freedesktop.org) — the layout database used by every Linux desktop: `rules/base.xml` (layout and
    variant list with English names), `symbols/*` (keysyms per key per level, dead keys), `geometry/*`. License: Expat (MIT) by
    default; vendor files Expat-Open-Group; `symbols/lv` and `geometry/sgi_vndr/*` HPND-sell-variant.
  - **libxkbcommon** — resolves rules + symbols into keycode → keysym per level (the reference implementation). MIT/X11-derivative
    licenses. Used only as a build-time tool, not shipped.
  - **keycodemapdb** (QEMU project) — `data/keymaps.csv` maps Linux keycode ↔ USB HID usage ↔ XKB key name ↔ Win32 VK ↔
    X11 keysym ↔ macOS keycode. Dual-licensed GPL-2.0-or-later **or** BSD-3-Clause; generated output may be distributed under
    either. This replaces the Linux kernel `hid-input.c` table (GPL-2.0), which is **not** copied.
  - **Noto Sans / Noto Sans CJK / Noto Sans Symbols** (SIL OFL 1.1) and **`lv_font_conv`** (MIT) for pre-rendered legend glyph
    subsets (FR-608).

## Options

| | Variant | Assessment |
|---|---|---|
| A | One fixed US layout, legends only (spec minimum) | REJECTED — no switching between countries |
| B | Hand-written layout tables per country | REJECTED — reinvents xkeyboard-config; AltGr/dead-key errors; unmaintainable |
| **C** | **Offline generator from xkeyboard-config + libxkbcommon + keycodemapdb → compact tables and font subsets compiled into the firmware and the CLIENT; a DEVICE setting "Host layout" that selects legends + geometry + IME keys** | **CHOSEN** — Linux-compatible by construction, reproducible, no runtime parser |
| D | Parsing xkb files at runtime on the ESP32-S3 | REJECTED — flash/RAM/complexity; 07 §1 forbids own parsers |
| E | Android `.kcm` key character maps (AOSP `InputDevices`, Apache-2.0) as the layout source | REJECTED for layouts — ~50 layouts, simplified level semantics, no dead-key/geometry model, not what the HOST actually runs; AOSP is used where it is the best permissive option: the IME engines (OpenWnn, PinyinIME) |

## Decision

### Behavior

- **FR-741 MUST — "Host layout" setting.** `Settings → Keyboard → Host layout` lists layouts generated from xkeyboard-config
  `rules/base.xml` (layout and variant), identified by the xkb string `layout(variant)` (e.g. `us`, `us(intl)`, `gb`, `de`, `pl`,
  `jp`, `kr`, `cn`, `br`), with the English display names from `base.xml`. Default `us`. Stored in NVS by `config_store`
  (`kbd.host_layout`, TECH-050 schema/validation). FR-508 "Host keymap label" is realized by this setting: the label shows the
  selected layout's name on the KEYBOARD screen.
- **FR-742 MUST — what changes on the DEVICE.** Selecting a layout changes only: (1) key legends per level (Base / Shift / AltGr /
  Shift+AltGr), rendered from the generated tables; (2) the geometry family (ANSI, ISO, JIS, ABNT2, KR) including the extra
  keys and their usages (ISO `<>` 0x64; JIS 0x87 / 0x88 / 0x89 / 0x8A / 0x8B and `Zenkaku/Hankaku` on 0x35; ABNT2 0x85 / 0x87;
  KR 0x90 / 0x91); (3) dead-key markers in legends (visual only). The DEVICE shows the hint "Set the same layout on the HOST"
  when the setting changes; a mismatch produces wrong characters on the HOST — a HID property, not a defect.
- **FR-743 MUST — keys stay usages.** A key sends the same usage regardless of the layout, except for the geometry-specific keys
  above. Legends are decoration; no keymap is derived from key text (07 §7). The DEVICE emits no Unicode and runs no IME
  (01 §1.2 unchanged); CJK and complex-script text comes from the HOST's IME driven by these keys.
- **FR-744 SHOULD — "Switch HOST layout" action** in SHORTCUTS: sends the HOST OS's layout toggle for the host OS selected in
  Settings (Windows `Win+Space`, macOS `Ctrl+Space`, GNOME `Super+Space`). A convenience without feedback; it goes through the
  ordinary shortcut path (typed intent → `InputArbiter`).
- **FR-745 MUST — the CLIENT uses the same tables.** ADR-019 FR-702 character → (usage, modifiers, dead-key sequence) tables
  come from the same generator, for every shipped layout. The CLIENT reads the active layout from the DEVICE status snapshot
  (new read-only field `host_layout`, 03 §6.6 — additive; value validated against the generated id list, never free text) so the
  CLIENT and the DEVICE cannot disagree. Characters outside the table are skipped and counted (FR-702 unchanged).

### On-device composition (IME) and Unicode delivery

- **FR-746 MUST — text is composed on the DEVICE (in scope of the main release).** For the selected language the KEYBOARD screen offers a composition mode in
  which the text is built and shown on the DEVICE (composition preview + candidate bar, conversion keys) before anything is
  sent: Japanese — 12-key flick kana (フリック入力) and romaji QWERTY with kana → kanji conversion; Simplified Chinese — pinyin
  QWERTY with candidates (9-key pinyin MAY follow); Korean — 2-set (두벌식) with algorithmic jamo → syllable composition.
  Traditional Chinese (Zhuyin/Cangjie) and further languages follow the same architecture later. Using the HOST's IME is never
  required for these languages. This changes the v1 scope statement of 01 §1.2 by owner decision.
- **FR-747 MUST — Unicode delivery modes.** Composed text leaves the DEVICE as HID keystrokes through one user-selected delivery
  mode (`Settings → Keyboard → Unicode input on HOST`): `linux` (Ctrl+Shift+U, hex, Space), `macos` (Option+hex, Unicode Hex
  Input source), `windows-hex` (Alt held, Numpad `+`, hex — requires `EnableHexNumpad=1` once), `wincompose` (Right Alt, `u`, hex,
  Enter), `host-ime` (readings as keystrokes; the HOST converts). The DEVICE shows the active mode and a one-screen setup hint per
  mode. Characters that the selected layout can type directly are always sent as plain keystrokes from the layout table; the
  Unicode sequence is used only for the rest. Code points above U+FFFF are sent as surrogate pairs where the mode requires it.
- **FR-748 MUST — one path, no new HID source.** Composed text → character queue → `kbd_layouts` (character → keystrokes or
  Unicode sequence) → typed intents → `InputArbiter` → `HidScheduler`. The source is `LOCAL` (a person on the LCD), so 03 §9 item
  4 is not triggered, but the arbiter must treat the sequence as one cancellable burst: paced within the 03 §6.5 budget (as in
  ADR-019), aborted and followed by `RELEASE_ALL` on cancel, mode switch, loss of USB or loss of ownership; never more than one
  Unicode sequence in flight; a partially sent sequence is terminated with the mode's cancel key where one exists (Linux `Esc`,
  WinCompose `Esc`) before `RELEASE_ALL`.
- **FR-749 SHOULD — the CLIENT reuses delivery.** ADR-019 type text sends characters outside the layout table through the
  active delivery mode (unless it is `host-ime`) instead of skipping them; the skip counter remains for characters no mode can
  carry.
- **TECH-080 MUST — engines, licenses, footprint.** First candidates: OpenWnn (Japanese) and PinyinIME (Simplified Chinese), both
  Apache-2.0, as ESP-IDF components with their own license headers and entries in `THIRD_PARTY_NOTICES.md`; Korean composer
  written in-house (≤ 200 lines, algorithmic). A spike task measures flash, PSRAM and conversion latency on board #1 before the
  choice is final; the fallback is a pruned dictionary, never a GPL engine. The engines run in the UI owner task's budget without
  per-keystroke heap allocation outside their own arena; conversion latency target ≤ 50 ms per keystroke (to be confirmed by
  measurement — `NOT_RUN_HW` until then).
- **TECH-081 MUST — dictionary storage.** Dictionaries live in a dedicated read-only `ime_data` partition shared by both OTA slots
  (TECH-051), outside the app image, updated by the same signed OTA mechanism. Order of magnitude: pinyin ~1–2 MiB, Japanese
  several MiB; if the 16 MiB flash cannot hold a language next to two OTA slots, that language's dictionary is loaded from the
  board's microSD (TF) slot (presence and wiring to be verified on board #1 in TASK-010). Never downloaded at runtime from the
  Internet (02 §11 assets rule).

### Implementation rules

- **TECH-077 MUST — generator, not hand-written tables.** `tools/kbd_layouts/gen_layouts.py` runs on a Linux tooling host
  (CI runner, WSL or a container — libxkbcommon is not readily available on Windows; the outputs are committed, so the Windows
  workstation never needs it), using `xkbcli compile-keymap` / the `xkbcommon` Python bindings with libxkbcommon ≥ 1.13, pinned to
  one xkeyboard-config release (version + SHA256 of the tarball from `xorg.freedesktop.org/archive/individual/data/xkeyboard-config/`,
  signature verified) and one keycodemapdb commit. The pin is the **newest stable release at the time the task starts** — as of
  2026-11-01 that is 2.48 (2026-06-21; 2.49 appeared in distributions on 2026-11-01 and is taken once its tarball is on the
  archive) — never an older release such as 2.44; the release cadence is roughly every four months and a version bump is a
  deliberate change with a reviewed regeneration diff. Outputs are committed: `components/kbd_layouts/generated/*.c|*.h` for the DEVICE and
  `clients/windows/**/layouts/*.json` for the CLIENT, each with a provenance header (source versions, hashes, generator command).
  Generated files are never edited by hand; CI regenerates and requires an empty diff (the same rule as generated UI code, 07 §8).
  Table format per layout: XKB key name → HID usage (via keycodemapdb), up to 4 levels of UTF-8 legend (≤ 8 bytes each), flags
  (dead key, IME key, geometry family); target ≤ 2 KiB per layout; the character → keystroke reverse index is built at generation
  time. Zero allocation at runtime (tables in flash, accessed by index).
- **TECH-078 MUST — fonts.** Legend glyphs are pre-rendered subsets (FR-608) produced by `lv_font_conv` from Noto fonts for
  exactly the code points present in the shipped legends plus ASCII. Scripts that need shaping (Arabic, Devanagari, Thai, …)
  ship as isolated glyphs and are marked `best-effort` in the layout list. Budget: all layout tables + legend fonts ≤ 1.5 MiB of
  flash (16 MiB, ADR-009), measured in TASK-120.
- **TECH-079 MUST — which layouts ship and under which license.** Every `layout(variant)` from `base.xml` whose keys map onto one
  of the five geometry families; the generator lists what it rejects; no per-country exceptions in code. Only `symbols/*` files
  under Expat / Expat-Open-Group are consumed; `symbols/lv` (HPND-sell-variant) is excluded until a license review. keycodemapdb
  is used under its BSD-3-Clause option. `THIRD_PARTY_NOTICES.md` lists xkeyboard-config, keycodemapdb, Noto (OFL-1.1, shipped
  as glyph subsets) and the build-time tools (libxkbcommon, lv_font_conv).
- **03 §9 compliance.** New snapshot field → item 3 (whitelist, validation, fuzz test: AT-EXT-25). No new HID source → item 4 not
  triggered. Report descriptors unchanged → item 2 not triggered. BIOS/UEFI boot protocol ignores layouts (US assumed) — unchanged.

## Consequences

- New component `kbd_layouts` (generated tables + a small accessor API; no `lvgl.h`, no `tusb.h`). `ui_views` renders the keyboard
  from it; TASK-070 `kbd_map` becomes "key → usage from the geometry table". `config_store`: key `kbd.host_layout`.
  `remote_protocol`: snapshot field `host_layout`. The CLIENT (TASK-105) ships the JSON tables.
- Risks: DEVICE/HOST layout mismatch (mitigated by the hint and the visible label, FR-742); glyph coverage for exotic scripts
  (the generator prints a coverage report; `best-effort` marking); xkeyboard-config drift between releases (pinned version,
  regeneration diff in CI).
- The CLIENT's "type text" grows from 2 to all shipped layouts at zero hand-written cost.
- New component `ime_engine` (composition: OpenWnn / PinyinIME wrappers, Hangul composer; no `lvgl.h`, no `tusb.h`) and the
  `ime_data` partition; `ui_views` gains the composition preview, candidate bar and flick keyboard; `kbd_layouts` gains the Unicode
  delivery sequences. Flash and PSRAM budgets are only known after the spike — this ADR commits to the architecture, not to a
  number.
- Honest limitation to document for users: arbitrary Unicode on Windows needs a one-time HOST setting (HexNumpad) or WinCompose,
  exactly as for every hardware keyboard with Unicode input; Linux desktops need nothing; macOS needs the input source enabled once.
- Tasks: **TASK-071** — layout generator (`tools/kbd_layouts/`), `kbd_layouts` component, legend font subsets, `host_layout`
  snapshot field, CLIENT tables (with TASK-105); runs within M5 next to TASK-070B. **TASK-072** — IME: footprint spike on board #1,
  `ime_engine` component (OpenWnn, PinyinIME, Hangul composer), `ime_data` partition (with TASK-000's partition table), flick
  keyboard and candidate bar in `ui_views`, Unicode delivery modes; part of the `local-only` release gate (05 §2), not deferred.

## Tests

| ID | Level | Check | Expected |
|---|---|---|---|
| AT-EXT-22 | unit (host) | generator determinism and validity: same inputs → byte-identical outputs; every key has exactly one page-0x07 usage; legends ≤ 8 bytes; coverage report | PASS; a changed input changes the provenance header |
| AT-EXT-23 | HIL | `us`, `de` (QWERTZ, ISO), `jp` (JIS incl. Henkan / Muhenkan / Kana / Zenkaku), `kr` (Hangul / Hanja), `br` (ABNT2) against a Windows HOST with the matching layout; a sample text typed on the DEVICE | the HOST receives the expected characters; on `jp` the IME toggles with Zenkaku/Hankaku |
| AT-EXT-24 | unit | CLIENT type-text with `pl`, `de`, `us(intl)` (dead keys), `jp` | expected keystroke sequences; out-of-table characters counted, none sent |
| AT-EXT-25 | SEC + fuzz | `host_layout` snapshot field: unknown id, overlong string, `settings.*` attempt from the CLIENT | rejected / `ERR_BAD_FRAME`; the field is read-only (SEC-107) |
| AT-KEY-01 / AT-UI-02 (extended) | unit + HIL | layout switched in Settings while a key is `DOWN` or a modifier is `LOCKED` | `RELEASE_ALL` first, no sticky state; legends refresh without touching the HID path |
| AT-EXT-26 | unit (host) | composers: Hangul algorithm vectors; pinyin candidates and kana → kanji conversion against fixed dictionaries; memory bounded (arena only), latency measured on the host as a proxy | deterministic output; zero heap growth over 10 000 keystrokes |
| AT-EXT-27 | HIL | the same Japanese / Chinese / Korean sample text (incl. one non-BMP character) composed on the DEVICE, delivered in `linux` (GNOME + IBus), `macos` (Unicode Hex Input) and `windows-hex` / `wincompose` modes | byte-identical text on the HOST; cancel mid-sequence and USB unplug mid-burst → `RELEASE_ALL`, no stuck key; a wrong mode produces wrong characters but never a stuck key |

## Migration

None — no keyboard implementation exists. FR-508 is reinterpreted (label = selected layout). ADR-019 FR-702 changes source:
generated tables instead of hand-written `US` / `PL-Programmer`.
