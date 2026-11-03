# `simulator/`

Desktop simulator of the KVMpad UI — level **L1** of the UI tests (`docs/spec/07-…` §10), TASK-070D.

It builds the **real product shell** — the same `ui_app_init()` composition that `main/ui_shell.c` builds — on the
real LVGL 9.6 pinned by `dependencies.lock`, drives it from fixture files through the real domain publishers and
the real LVGL hit test, renders every fixture at 1024×600 and compares the result with the committed golden PNG.

| Directory | Contents |
|---|---|
| `lvgl_sdl/` | the `kvm_sim` CMake project: LVGL 9.6 + `ui_core` + `ui_theme` + `ui_components` + `ui_views`, with a fake platform |
| `fixtures/` | one plain-text file per deterministic state, with optional touch playback |
| `golden/` | 1024×600 PNG dumps from native LVGL (not from an HTML render) plus `MANIFEST.md` |
| `tools/` | `golden_manifest.py` — regenerates `golden/MANIFEST.md` |

The simulator proves nothing about the MCU: no panel, no GT911, no USB HOST. Hardware results stay `NOT_RUN_HW`
and come from HIL separately.

## What is real and what is fake

Real: LVGL 9.6, `ui_core` (mailbox, store, router, overlay stack, action gateway), `ui_theme`, `ui_components`,
`ui_views` including `ui_app.c`, and the compiled chord allowlist of `hid_core/src/hid_keymap.c`.

Fake: the clock (a counter, so a render is reproducible), the domain sink (a recorder — the simulator links no
USB stack and can emit no HID report), the domain producers (a fixture is published through
`ui_mailbox_publish_hid/net/touch/gesture`, so the UI still derives every state from an authoritative snapshot),
`cancel_input` / `pad_changed`, which are recorded so the ordering of a mode switch is observable,
`remote_action` — there is no pairing state machine here, so a tap on `Accept` or `Allow` is recorded and
nothing else happens — and `settings_change`, for the same reason: there is no `config_store` here, so a
Settings tap is recorded and no record is written or republished.

## Build and run

```sh
export PATH=.../llvm-mingw-.../bin:.../ninja:.../cmake/bin:$PATH
cmake -S simulator/lvgl_sdl -B ../build-sim -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-sim
ctest --test-dir ../build-sim --output-on-failure
```

`kvm_sim` is always built and needs no arguments: the fixture and golden directories are baked in. The
sanitizer build links the toolchain runtime dynamically, so the compiler's `bin/` directory must stay on `PATH`
when the binary (or `ctest`) runs; without it the process exits 127 before `main()`.

| Option | Meaning |
|---|---|
| `--update-golden` | regenerate `simulator/golden/` instead of comparing |
| `--only <name>` | one fixture (the acceptance checks are then skipped) |
| `--list` | print the fixture names and exit |
| `--tolerance <n>` | per-channel difference a pixel may have (default 2) |
| `--budget <n>` | how many pixels may exceed it (default 64) |
| `--switches <n>` | AT-UIFW-02 view switches (default 2000) |
| `--modal-cycles <n>` | AT-UIFW-10 open/close cycles (default 200) |
| `--fixtures` / `--golden` / `--out` | override the directories |
| `--hanzi <blob>` | the hanzi font blob of the `ime_data` image (`tools/fonts/gen_hanzi_font_blob.py`; also the CMake cache variable `KVM_HANZI_BLOB`). Fixtures with `zh 1` (a canned pinyin decoder, `lvgl_sdl/sim_zh.c`) compare their golden only with it; without it they still run their checks and the golden row reads `skipped`. A blob that is given but fails validation stops the run |
| `--kanji <blob>` | the same for the kanji font blob (`gen_hanzi_font_blob.py --set ja`; CMake `KVM_KANJI_BLOB`) and the fixtures with `ja 1` (a canned romaji / kana-kanji decoder, `lvgl_sdl/sim_ja.c`) |

The rendered image of every fixture is written to `<build>/rendered/`, so a mismatch can be compared with the
golden by eye. After `--update-golden`, regenerate the record in the same step:

```sh
python -I -X utf8 simulator/tools/golden_manifest.py --lvgl 9.6.0
```

## Determinism

The run uses LVGL's virtual clock (`lv_test_wait()` advances `lv_tick_inc` by one millisecond per iteration), so
no wall clock reaches the renderer. The views use no `lv_anim`; the renderer waits for any future animation to
finish before it snapshots. Fixtures are rendered in byte order of their file names. The snapshot is taken in
RGB565 — the pixel format of the panel (ADR-026, TECH-087) — and expanded to 24-bit RGB for the PNG, so a golden
shows the quantization the panel shows.

PNG is written and read with the lodepng that LVGL already vendors (`LV_USE_LODEPNG` in
`lvgl_sdl/lv_conf.h`): no new dependency, nothing installed.

## Fixture format

One directive per line, `#` starts a comment, an unknown key is an error. Defaults: HOST mounted, CLIENT
listening, owner local, Wi-Fi up, touch healthy, CapsLock LED known and off, MOUSE view.

```
title   <free text>                       caption in the summary table
view    mouse | keyboard | shortcuts      navigated to through the mode switcher, like a finger does
host    uninitialized | detached | enumerating | mounted | suspended | error
client  disabled | network_down | listening | authenticating | connected | control_granted | stale
owner   none | local | remote             epoch <u32>
wifi / touch / drag / maint   0 | 1
mods / armed / locked  <hex>              modifier ledger, ADR-016 armed and locked bits
caps    unknown | on | off                error <u16>
confirm 0 | 1                             the FR-734 policy the shell reports to the SHORTCUTS view
layout  win | ctrl                        the FR-744 chord of the HOST OS
step    <step>                            touch playback, in order
```

Pairing and control approval (TASK-100), published exactly as the `secure_remote` adapter publishes them,
through `ui_app_set_pairing()` / `ui_app_set_approval()`:

```
pair      0 | 1                           a pairing window is open (FR-722)
code      <6 digits>                      shown as "ddd ddd"; anything else is refused by the card
code_ms / win_ms      <u32>               the two countdowns, default 120000 / 180000
accept    0 | 1                           a client proved the code: Accept / Reject (FR-725)
accept_ms <u32>                           default 30000
approval  0 | 1                           a paired client requests control (FR-733)
appr_ms   <u32>       session <u32>       default 30000; the session an Allow / Deny names
peer      <name>                          client name of the prompts and of the outcome toast
result    none | paired | rejected | failed | locked | expired
```

Settings (TASK-070C), published exactly as the `config_store` adapter publishes the record, through
`ui_app_set_settings()`. The card is opened through the gear and the section through its tab, so both go
through the real hit test:

```
settings  0 | 1                           open the Settings card
section   remote | clients | shortcuts | input | keyboard | about
cfg_seq   <u32>                           0 = nothing published yet: every value Unknown, every control off
remote / req_accept / altf4 / awake  0 | 1    FR-506, FR-732, FR-734, FR-711
approval_policy  always | trusted | auto  control.approval (FR-733)
awake_s   <10..300>                       keepawake.interval_s (ADR-020)
layout_id <id>                            host_layout, e.g. us or de(neo) (FR-741)
unicode   host_ime | linux | macos | windows_hex | wincompose   (FR-747)
lang_ko / lang_zh / lang_ja  0 | 1        the input languages turned on in Settings (ADR-037)
fw        <version>                       the firmware version of the About pane (FR-751)
client / tclient  <name>                  append a paired client, with or without the trusted flag (FR-726)
```

Steps: `tap_nav <0..2>`, `tap_host`, `tap_client`, `tap_key <legend>`, `hold_key <legend>`,
`tap_mouse left|right|drag|help`, `tap_shortcut <0..5>`, `tap_media <0..5>`, `tap_layout`, `tap_pair`,
`tap_settings`, `tap_section <0..5>`, `tap_toggle <setting id>`, `tap_approval <0..2>`,
`tap_trusted <index>`, `tap_forget <index>`, `tap_confirm 0|1`, `tap_lang`, `hold_lang`,
`pick_lang en|ko|zh|ja`, `wait <ms>`.

The language key of the KEYBOARD view (ADR-037) is held for 600 ms by `hold_lang`, past LVGL's 400 ms
long-press time, so the picker opens and the short click does not fire; `pick_lang` taps one of its rows.
A language a fixture turns on is still offered only when it is usable: `tap_lang` and the picker skip a
language whose decoder is not installed (`zh 1` / `ja 1`) or whose text cannot be delivered because
`unicode` is `host_ime`.

A Settings tap is recorded and nothing is written back — the simulator holds no `config_store`. That is
exactly the state a device is in between the tap and the NVS write, so the golden of a tapped toggle shows the
**old** value (AGENTS-UI MUST 6).

## Checks

Per fixture: **AT-UIFW-01** (every visible interactive object inside 1024×600, hit box ≥ 44 px, no overlap
between two unrelated hit targets) and a hidden-element probe (the centre of every hidden interactive object is
pressed; none of them may take the press). Labels that are not contained in their parent are reported as a
warning.

Once per run: **AT-UIFW-02** (N view switches with the live allocation and observer counts flat),
**AT-UIFW-04** (a press on the touch surface is no LVGL input: no action, no phantom tab click, no view change),
**AT-UIFW-08** (a forced view switch while a key is held submits exactly one `KEY_UP`, after the cancel request
and before the new input area is reported) and **AT-UIFW-10** (N details-card open/close cycles; the pad comes
back every time). Every fixture and the whole run must return to the live-allocation baseline (**AT-UIFW-09**).

`kvm_sim` prints a summary table and exits non-zero on any failure.

## Not covered yet

* **Interactive SDL window** — the renderer is headless and needs no display server. An SDL2 window was not
  built: no SDL development package is present in the workspace toolchain directory and the rule against
  installing outside `../.tools/` makes fetching one a separate, explicit decision. The directory keeps its
  `lvgl_sdl` name for the paths the specification uses.
