# KVMpad repository layout

Directory map consistent with specification v1.1.0 (`docs/spec/02` §3.1 + `docs/spec/07` §2).
Every component directory has a `README.md` with its role, task, spec references and rules.

```text
esp32-s3-kvmpad/
├── AGENTS.md, AGENTS-UI.md   # work contract (part of the spec package, not modified)
├── CONTRIBUTING.md           # rules for working in the repo: paths, ADRs, PRs, license, tools
├── LICENSE.md                # PolyForm Noncommercial 1.0.0 (+ Required Notice)
├── THIRD_PARTY_NOTICES.md    # vendored third-party paths + licenses (read by the header checker, rendered in About)
├── docs/
│   ├── spec/                 # specification v1.1.0 01–07 + README + MANIFEST (frozen, SHA256)
│   ├── ADR/                  # new ADRs (ADR-014+); ADR-001..013 are in spec/06
│   ├── EXTENSIONS.md         # index of requirements added after spec v1.1.0 (ADR-017+)
│   ├── ROADMAP.md            # milestones, gates, ADR-added tasks, current status (05 §2 + ADRs)
│   ├── prior-art/            # analyses of other people's projects (what we take, what we do not)
│   └── REPO-LAYOUT.md        # this file
├── reference/                # HTML mockup + PNG — UX reference, not firmware code
├── hardware/boards.md        # board unit record (ADR-009: Flash/PSRAM — measured)
├── main/                     # app_main.c                                    [TASK-000]
├── components/
│   ├── board_hal/            # I²C, CH422G, USB_SEL, LCD init, backlight     [TASK-010/020]
│   ├── touch_input/          # GT911 0..5 points, track_id — the ONLY reader [TASK-030]
│   ├── gesture_engine/       # pure gesture FSM, host-testable               [TASK-040]
│   ├── hid_core/             # ledger, InputArbiter, RELEASE_ALL, scheduler  [TASK-050]
│   ├── usb_hid_device/       # TinyUSB: boot kbd + boot mouse, USB states    [TASK-050]
│   ├── ui_platform/          # LVGL ↔ esp_lcd adapter, LVGL owner            [TASK-060A]
│   ├── ui_core/              # store/mailbox, router, overlay, actions       [TASK-060B/C]
│   ├── ui_theme/             # tokens, styles, fonts                         [TASK-060D]
│   ├── ui_components/        # status item, topbar, action key, modal, toast [TASK-060D]
│   ├── ui_touch_bridge/      # 1 GT911 sample → LVGL indev + GestureFSM      [TASK-060E]
│   ├── ui_views/             # Mouse / Keyboard / Shortcuts / Settings       [TASK-070A–C]
│   ├── kbd_layouts/          # generated layout tables, Unicode delivery     [TASK-071]
│   ├── ime_engine/           # JP/zh/ko composition (ADR-024)                [TASK-072]
│   ├── config_store/         # NVS schema + migrations                       [TASK-090]
│   ├── remote_protocol/      # CLIENT v1: parser, validation, sessions       [TASK-100] 🔒
│   ├── secure_remote/        # TLS/WSS, pairing, leases                      [TASK-100] 🔒
│   ├── update_manager/       # signed OTA, rollback                          [TASK-110]
│   └── diagnostics/          # metrics, counters, ring log                   [TASK-120]
├── test/
│   ├── host_unit/  fuzz/  hil/          # L1 / L2 / L3–L4 (spec 04)
│   ├── ui_store/  ui_nav/  ui_touch/    # UI Core tests (AGENTS-UI)
│   └── reports/                          # test reports (04 §12 format)
├── simulator/{lvgl_sdl,fixtures,golden}/ # LVGL 9.6 SDL 800×480     [TASK-070D]
└── tools/                    # verify_spec_manifest.py, check_license_headers.py, kbd_layouts/ generator, flash, statistics
```

🔒 = blocked until a working `release_all`/epoch + HIL USB (05 §4). ADR-002 resolved by ADR-021 (native Windows CLIENT).

## Repository boundary

The repository contains **only what is published**. The workspace directory **above** the repo (outside git) holds:

| Directory (relative to the repo) | Contents |
|---|---|
| `../.tools/` | tool venvs (esptool etc.) — we do not install anything inside the repo |
| `../spec-source/` | the original ZIP of specification v1.1.0 |
| `../evidence/` | raw measurement and test logs; the key lines + the file's SHA256 stay in the repo |
| `../notes/` | working notes, settings of the project page on GitHub |

Rule: the product and its documentation → the repo; logs, dumps, venvs, notes → next to the repo.

## Dependency rules (07 §2, 02 §3)

```text
ui_views ──► ui_components ──► ui_theme
    └──────► ui_core ──(UiAction, typed queue)──► hid_core (InputArbiter) ──► usb_hid_device ──► TinyUSB
touch_input ──► ui_touch_bridge ──► { LVGL indev , gesture_engine ──► hid_core }
remote_protocol ◄── secure_remote          (remote → hid_core only via InputArbiter)
```

- `lvgl.h` is allowed only in `ui_*`. The domain core (`hid_core`, `gesture_engine`, …) does not import it.
- `tusb.h` / `tinyusb.h` is allowed only in `usb_hid_device`.
- `gesture_engine` and the core of `hid_core` — pure C, testable on the host without ESP-IDF.

## Why the spec is split across three places

The original package had a flat structure. The target locations were taken from the specification:
documents → `docs/spec/` (02 §3.1: `docs/`), `AGENTS*.md` → root (the "read this first" requirement),
mockup → `reference/` (02 §3.1). The repository holds the English edition of the package; the original ZIP stays in the
workspace (`../spec-source/`, outside git). The package is frozen — its SHA256 manifest is verified like this:

```sh
python -I tools/verify_spec_manifest.py
```

Read references in the spec of the form `02-TECHNICAL-SPECIFICATION.md` as `docs/spec/02-…`.
The name "KVM Essential" in the spec = KVMpad; the `kvm-essential/` tree in 02 §3.1 = this repo.

## Deliberately NOT created yet

| Item | When | Why not now |
|---|---|---|
| `CMakeLists.txt`, `main/CMakeLists.txt`, `sdkconfig.defaults`, `idf_component.yml`, `dependencies.lock` | TASK-000 | TECH-002: they arise from a **working** `idf.py build` (ESP-IDF v6.1), not "from memory" |
| `partitions.csv` | TASK-000 | ADR-009 resolved: 16 MiB Flash, 8 MiB PSRAM (`hardware/boards.md`); a table with ≥ 2 OTA slots plus a read-only `ime_data` partition for IME dictionaries (ADR-024 TECH-081) is created once the real binary size is known |
| `.github/workflows/` | TASK-000 | CI requires a settled toolchain |
| `clients/windows/` | TASK-105 | native CLIENT (ADR-021); only after TASK-100 and the 05 §4 gate |

**Will never be created (ADR-014):** `components/ui_generated/`, `design/` — EEZ Studio, LVGL Pro Editor
and other UI generators are out; TASK-061 cancelled. UI = LVGL 9.6 + native Observer + KVMpad UI Core, by hand in C.

## Gaps in the specification (to be resolved by an ADR, we do not guess)

1. **TASK-080 (Wi-Fi STA/provisioning, status service, USB state publisher)** — the 02 §3.1 tree does not indicate a component.
   Candidates: a new `net_status/` or `diagnostics/` + `usb_hid_device/`. → ADR-015.
2. **`kbd_map` / `kbd_fsm` (TASK-070)** — `hid_core` has a "key map", but the place for the modifier FSM (one-shot/locked, Fn) is not specified.
   Recommendation: the FSM in `hid_core` (the source of truth for the DOWN state), the view only a projection. → ADR-016.
3. **`ui_bindings`** — listed in 07 §8, absent from the 07 §2 tree; TASK-061 cancelled (ADR-014), so it will not resolve this.
   Recommendation: `ui_core/ui_bindings.c` (subject ↔ snapshot), resolved by TASK-060B.
