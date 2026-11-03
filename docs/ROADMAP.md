# KVMpad roadmap

**Status date:** 2026-11-02 · **Normative source for task contents:** `docs/spec/05-IMPLEMENTATION-PLAN.md` (milestones
M0–M9, task backlog, order, Definition of Done) plus the ADRs in `docs/ADR/` for everything added after the specification was
frozen. This file adds what 05 cannot carry: the tasks introduced by ADRs, the current status of each item, the release gates as
they stand today, and the next concrete steps. No calendar dates: a single developer and hardware-gated milestones make dates
fiction; progress is measured by gates passed with evidence (`test/reports/`, `hardware/boards.md`).

Status values follow 05 §13: `DESIGNED` → `IMPLEMENTED_SW` → `READY_FOR_HIL` → `VERIFIED_HW` → `RELEASE_CANDIDATE` → `RELEASED`;
`BLOCKED` names an ADR or a failing test. `NEXT` marks the item being started.

## 1. Where the project stands

| Done | Evidence |
|---|---|
| Specification v1.1.0 — English edition, frozen (SHA256 manifest 12/12) | `docs/spec/`, `tools/verify_spec_manifest.py` |
| ADR-014 … ADR-036 accepted | `docs/ADR/README.md`, `docs/EXTENSIONS.md` |
| Board unit #1 identified: 16 MiB Flash, 8 MiB PSRAM, rev v0.2 (ADR-009 resolved) | `hardware/boards.md` |
| Repository skeleton: component READMEs with roles, rules, tests; contribution rules; license notices | `docs/REPO-LAYOUT.md`, `CONTRIBUTING.md` |
| Public repository with description and topics | github.com/D-Ogi/esp32-s3-kvmpad |

Firmware builds and runs on board #1 (M0..M6, see the status column). A hardware claim is `PASS_HW` only where a report in `test/reports/` says so; everything else is `NOT_RUN_HW`.

## 2. Milestones, gates and tasks

The table merges 05 §2/§3 with the tasks that the ADRs added. "Inputs" names the decisions a task must implement.

| Milestone | Gate (05 §2) | Tasks | Inputs from ADRs | Status |
|---|---|---|---|---|
| **M0** Toolchain, proof of compatibility | CAN BUILD + DETECT HW | TASK-000 | ADR-003 (ESP-IDF v6.1, pinned components), ADR-009 (16 MiB), ADR-024 TECH-081 (`ime_data` partition), ADR-025 TECH-083/084 (header checker in CI, retained notice string), **ADR-026** (target board 7B) | **IMPLEMENTED_SW**, gate passed on board #1 2026-11-02 (build + HW detection; `test/reports/2026-11-02_M0-M2_d36ab0a.yaml`); not yet committed |
| **M1** BSP: expander + GT911 + RGB (7B: CH32V003, ADR-026) | LCD + MULTITOUCH HIL | TASK-010, TASK-020, TASK-030 | ADR-008 (framebuffer experiment in TASK-020), ADR-026 | **VERIFIED_HW** 2026-11-02 on board #1: expander, LCD A/B/C bench, stress, glitch recovery, touch fault recovery, 5-finger trials, five control points, R/G/B / alignment sign-off, both connectors; HOST-off with auxiliary power `BLOCKED` by the board power path (HID-port VBUS = 5V rail, back-feed; status limitation in ADR-026) |
| **M2** USB device: boot keyboard + mouse | enumerate in Windows / Linux / UEFI | TASK-050 (USB part) | ADR-005, ADR-010 (HOST state from USB), ADR-018 (3rd interface Consumer Control, `AC Pan`, no CDC on OTG) | **Windows: PASS** 2026-11-02 (3 interfaces, LED output report); Linux and UEFI `NOT_RUN_HW` |
| **M3** HID core, arbiter, `RELEASE_ALL` | tests, no stuck key | TASK-050 (core part), **ADR-016** (where `kbd_fsm` lives — write before TASK-070B) | ADR-011 (LOCAL/REMOTE arbitration), ADR-018 (Consumer in the ledger), ADR-020 (`KVM_HID_KEEPALIVE`), ADR-024 FR-748 (cancellable burst) | **IMPLEMENTED_SW** 2026-11-02: host suites `PASS_SW` (AT-HID-03..09, AT-KEY-04/05/07/10, VEC-005..009, 10k-shortcut stress, seeded random walk); on board #1 the scheduler runs in the TinyUSB task, neutral start + `RELEASE_ALL` read by Windows, bootloop 15/15 (`test/reports/2026-11-02_M3_d36ab0a.yaml`); input on the wire checked from the Windows HOST with the bench `hid probe` + Raw Input capture: AT-HID-02 (keyboard), AT-HID-03 (6KRO, 7th refused), AT-HID-04 (int8 split, totals and net motion exact), break for every make `PASS_HW`; AT-HID-06 (Alt+Tab, Win+D, Ctrl+Shift+Esc in the order of 02 §7.3, the owner saw them act on the desktop) and the revoke half of AT-HID-08 (chord cut by `RELEASE_ALL`, nothing left held) `PASS_HW` (`test/reports/2026-11-02_M3_HID-wire_d36ab0a.yaml`); unplug/suspend during a chord (AT-HID-08, owner at the cable, 10k-shortcut soak) `PASS_HW`: release on every unplug, nothing held; AT-HID-09 `FINDING`: 0 stuck modifiers, but the HOST check counted 3 repeated makes and 3 stray breaks around very short cable contacts, re-run postponed by the owner; Ctrl+Alt+Del on the wire (AT-HID-05) tried by the owner by hand, formal run postponed (`NOT_RUN_HW`); ADR-016 ACCEPTED |
| **M4** Deterministic GestureFSM | vectors + fuzz | TASK-040 | ADR-007 | **IMPLEMENTED_SW** 2026-11-02: `gesture_engine` pure C; VEC-001..004 and AT-GEST-01..12 `PASS_SW`, 100k random sequences on four seeds, five mutants caught; vectors replay on board #1 (`gesture replay`, no HID); the GT911 sampler is routed into the engine by TASK-060E (`ui_touch_bridge`); real-finger gestures `NOT_RUN_HW` until the owner is at the bench (`test/reports/2026-11-02_M4_d36ab0a.yaml`) |
| **M5** LVGL 800×480 UI | hitboxes + 3-mode HIL | TASK-060A (adapter spike, **decides ADR-013**), TASK-060B/C/D/E, TASK-070A/B/C/D, **TASK-071** (layouts), **TASK-072** (IME) | ADR-013 (UI Core), ADR-014 (no generators, TASK-061 cancelled), ADR-022/023 (Settings → Remote screens, policies), ADR-024 (Host layout, composition mode, delivery modes), ADR-025 FR-751 (Settings → About) | **IN PROGRESS** 2026-11-02 — TASK-060A..E and TASK-070A..C `IMPLEMENTED_SW` and on board #1: `ui_platform` (LVGL 9.6.0 + `esp_lvgl_adapter` 0.7.4, ADR-027 ACCEPTED), store/mailbox, router/overlays, theme/components, one-reader touch bridge, and the product shell `ui_app` with MOUSE, KEYBOARD, SHORTCUTS and the HOST/CLIENT details cards; owner used the pad and the on-screen keyboard on the HOST; snapshots of every screen (`test/reports/2026-11-02_M5_*`). TASK-070D L1 simulator: 23 fixtures at 1024×600 match their goldens, AT-UIFW-01/02/04/08/10 `PASS_SW` (`test/reports/2026-11-02_M5_TASK-070D_d36ab0a.yaml`). Settings card (TASK-070C: Remote policies with a confirmation for every relaxation, paired clients with trusted/Forget, Alt+F4 confirmation, keep-awake, read-only keyboard values until TASK-071, About with build-time third-party notices) `IMPLEMENTED_SW`, 39 goldens, opens on board #1 with the saved values; the LVGL stack went to 12 KiB after it overflowed 8 KiB (`test/reports/2026-11-02_M5_TASK-070C-settings_d36ab0a.yaml`). TASK-072 host slice `IMPLEMENTED_SW`: the Korean 2-set composer (`ime_engine`) and the FR-747 Unicode delivery sequences (`kbd_layouts`: `linux`, `macos`, `windows-hex`, `wincompose`), and the FR-748 cancellable text burst in `hid_core` / `hid_dispatch`, fed by an ordered text queue (`main/text_glue`); the KEYBOARD view's KO layer composes Korean on the device (preedit strip in a generated Noto Sans KR subset, OFL-1.1) and the Settings Keyboard pane chooses the FR-747 delivery mode with a setup hint; 40 simulator goldens; on board #1 without typing into the HOST (`test/reports/2026-11-02_M5_TASK-072-host_d36ab0a.yaml`). TASK-072 Chinese engine: the vendored PinyinIME (AOSP, Apache-2.0) decodes on board #1 from the read-only `ime_data` partition (SHA256-verified image, `tools/ime/build_ime_data.py`): open 121 ms, worst 9.3 ms per search (target 50 ms), ~1.04 MiB of PSRAM while open, internal heap unchanged, two upstream leaks fixed locally (`test/reports/2026-11-02_M5_TASK-072-pinyin_d36ab0a.yaml`). TASK-072 ZH layer `IMPLEMENTED_SW`: pinyin input and a six-candidate strip in the KEYBOARD view, the hanzi font (Noto Sans SC, 16,580 glyphs, OFL-1.1) read in place from the `ime_data` image, decoder and font handed to the LVGL owner by a background worker (`main/ime_glue`); 41 simulator goldens; on board #1 the image verifies in 1.2 s after boot and the decoder installs itself (`test/reports/2026-11-02_M5_TASK-072-zh-layer_d36ab0a.yaml`). TASK-072 Japanese: the vendored OpenWnn engine (AOSP, Apache-2.0) with C ports of its romaji and clause converters, host-tested (`test/reports/2026-11-02_M5_TASK-072-openwnn_d36ab0a.yaml`); the JA layer of the KEYBOARD view (kana reading, candidates, Japanese punctuation) and both candidate fonts LVGL-compressed in a 0x7B0000 `ime_data` partition (ADR-033 ACCEPTED); on board #1 both decoders install 5.4 s after boot, after a task-watchdog reset in the loader was found and fixed (`test/reports/2026-11-02_M5_TASK-072-ja-layer_d36ab0a.yaml`). Open: changing a setting by touch and the dedicated mouse buttons on the HOST (owner); TASK-071 generator `BLOCKED` on a Linux tooling host (TECH-077; WSL2 does not start on the workstation without virtualization); typing on the KO, ZH and JA layers (QWERTY and the 12-key flick pad, implemented and simulator-tested) into a real HOST (owner) |
| **M6** NVS, Wi-Fi provisioning, independent statuses | local works offline | **ADR-015** (component for TASK-080 — write first), TASK-080, TASK-090 | ADR-017 (modem sleep off, BSSID cache), ADR-020/022/023/024 (NVS keys and defaults) | **IMPLEMENTED_SW** 2026-11-02, on board #1: `net_status` (ADR-015 ACCEPTED) with the link in backoff while local HID keeps working, ADR-017 cached reconnect (`cached=1`); `config_store` record survives a reset (`load=ok`) (`test/reports/2026-11-02_M6_*`). AT-EXT-01 measured: WIFI_PS_NONE first packet p50 3 ms vs modem sleep 26 ms (10 s idle) / 94 ms (60 s idle), TECH-070 kept; AT-EXT-02: 20/20 reconnects, cached and full scan both about 1.6 s on this single-AP channel-6 network (`test/reports/2026-11-02_M6_AT-EXT-01-02_d36ab0a.yaml`). Open: AP channel/BSSID change on hardware (ADR-015 accepted 2026-11-02) |
| **Release `local-only`** | after M6; the critical USB safety tests pass; not presented as a remote KVM | release checklist 04 §13 (local scope) | ADR-024: the on-device IME is **inside** this gate, not after it; ADR-035: release images are built with `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`, development images stay `-Og`) and pass `lcd stats` `missed` = 0 across view switches | — |
| **M7** Secure remote CLIENT | security tests | TASK-100, **TASK-105** (native Windows CLIENT) | ADR-021 (variant A, certificate pinning), ADR-022 (mDNS + 6-digit code + PAKE; algorithm confirmed in TASK-100), ADR-023 (approval policies), ADR-019/024 (type text on the CLIENT, `host_layout`, delivery modes), ADR-025 TECH-084 (`LegalCopyright`) | **IN PROGRESS** 2026-11-02: the M3 precondition (`release_all`, epoch, USB HIL) is met. `remote_protocol` (validator + session layer, ADR-030 ACCEPTED) and the `secure_remote/pairing` core (EC-JPAKE P-256 through PSA, ADR-029 ACCEPTED, 4 interop vectors) are `IMPLEMENTED_SW` with host suites (1269 checks + 200k-input fuzz; 41606 checks). Windows CLIENT (TASK-105, ADR-028 ACCEPTED) `IMPLEMENTED_SW`: .NET 9, 278 tests, the ADR-029 core compiled into a native library and all four vectors reproduced (`test/reports/2026-11-02_M7_TASK-105_d36ab0a.yaml`); `hid_dispatch_remote*` entry points in place. TLS/WSS transport, device certificate and mDNS (ADR-032 ACCEPTED) wired into the firmware with the DEVICE pairing and control-approval prompts; on board #1 with Remote off: TLS 1.3 with a stable certificate pin across a reset, admission refusals (403/400/404, no upgrade), no plain listener, mDNS discovery, and the CLIENT code finds the device and verifies the pin (`test/reports/2026-11-02_M7_TASK-100_d36ab0a.yaml`). Pairing, control approval and remote HID on the wire wait for the owner at the board (`NOT_RUN_HW`) |
| **Release `remote-enabled`** | after M7 and full security tests | 04 §13 (full) | — | — |
| **M8** OTA, hardening, recovery | rollback; eFuse only by the operator | TASK-110 | ADR-012 (production protections, owner decision for eFuses), ADR-024 TECH-081 (`ime_data` shared by both slots), ADR-031 (OTA source and signing, ACCEPTED) | **IMPLEMENTED_SW / READY_FOR_HIL** 2026-11-02: `update_manager` host suites 1788 checks; in the firmware with app rollback; on board #1 boot state, http refusal and a non-image rejected over HTTPS with input released `PASS_HW` (`test/reports/2026-11-02_M8_TASK-110_d36ab0a.yaml`). **Signed OTA on board #1** (development key, no eFuse): AT-SEC-09, AT-FAULT-07, AT-FAULT-08 rows 1-6 and AT-PERF-09 (fps during the write) `PASS_HW` after two fixes found there: the OTA/NVS write crash (ADR-034, ACCEPTED) and a health check that confirmed an image without the touch reader (`test/reports/2026-11-02_M8_TASK-110-ota_d36ab0a.yaml`). Key custody settled by ADR-036 (ACCEPTED: one key per builder, offline; no eFuse ever). Scan-out: ADR-035 ACCEPTED after the owner's visual check (no jump). Measured on board #1: `DOUBLE_DIRECT` lost 3–5 frames per view switch (the 10-line jumps), `DOUBLE_PARTIAL` with a 12-line internal strip lost none (render 265 ms instead of 159 ms, −14 % with `-O2`); the LCD_CAM shutdown handler gave 8 of 8 clean requested restarts (before: 3 of 6 through the RTC watchdog); `lcd stats` now counts under LVGL (`test/reports/2026-11-02_M5_ui-scanout-drift_d36ab0a.yaml`) |
| **M9** Performance, soak, release | full acceptance | TASK-120, TASK-130 | 05 §14 (optimization only after correctness) | DESIGNED |

### Tasks added by ADRs (not in 05 §3)

| Task | Scope | ADR | Milestone |
|---|---|---|---|
| TASK-071 | `tools/kbd_layouts/gen_layouts.py` on a Linux host, `kbd_layouts` component with committed tables, legend font subsets (≤ 1.5 MiB with the tables), `host_layout` snapshot field, CLIENT tables | 024 (FR-741..745, TECH-077..079) | M5 (with TASK-070B); CLIENT part with TASK-105 |
| TASK-072 | footprint spike on board #1, `ime_engine` (OpenWnn, PinyinIME, Hangul composer), `ime_data` partition, flick keyboard and candidate bar, Unicode delivery modes | 024 (FR-746..748, TECH-080/081) | M5, inside the `local-only` gate |
| TASK-105 | native Windows CLIENT with certificate pinning, pairing UI, type text through `keyboard.key`, About dialog and version resource | 021, 019, 022, 024, 025 | M7 |
| ADR-015 | component ownership for TASK-080 (Wi-Fi, status service, USB state publisher) | — | before M6 |
| ADR-016 | placement of the modifier FSM (`kbd_fsm`) | — | before TASK-070B |

Cancelled: TASK-061 (EEZ Studio spike) and `ui_generated/` — ADR-014.

## 3. Order and parallel tracks

05 §4 fixes the order. What it allows in parallel, split by whether the board is needed:

| Host-only (no board) | Needs board #1 |
|---|---|
| TASK-040 GestureFSM (pure C, vectors, fuzz) | TASK-010/020/030 BSP, LCD, GT911 |
| TASK-050 core: ledger, arbiter, epoch, `RELEASE_ALL` (host unit tests) | TASK-050 USB: enumeration, descriptors, LED output |
| TASK-060B/C/D on the SDL simulator | TASK-060A adapter spike, TASK-060E touch router, TASK-070D HIL |
| TASK-071 generator and tables (Linux host, CI) | TASK-072 footprint spike (flash / PSRAM / latency) |
| ADR-015, ADR-016 | TASK-080 Wi-Fi, TASK-110 OTA, TASK-120 soak |

Hard rules that override any ordering convenience: no remote control (TASK-100) before a working `release_all`, epoch and USB
HIL (05 §4); no change to the critical HID path after M5 without replaying `AT-HID`, `AT-KEY`, `AT-GEST`, `AT-CONN` (05 §12).

## 4. Open decisions and hardware-pending items

| Item | Resolved by |
|---|---|
| ADR-015 (reserved) | written before TASK-080; ACCEPTED 2026-11-02 |
| ADR-016 (`kbd_fsm` in `hid_core`) | ACCEPTED 2026-11-02 |
| ADR-013: `esp_lvgl_adapter` vs `esp_lvgl_port` | decided by TASK-060A on 2026-11-02 → ADR-027 (adapter), ACCEPTED 2026-11-02 |
| ADR-004 (VBUS separation), ADR-008 (framebuffers), ADR-017 (modem sleep), ADR-018 (3rd interface), ADR-020 (keep-awake) | PENDING-HW — measurements in their tasks |
| ADR-022 PAKE algorithm | TASK-100 |
| ADR-024 IME footprint and latency | TASK-072 spike on board #1 |
| ADR-012 eFuse / secure boot procedures | ADR-036 ACCEPTED 2026-11-02: no eFuse is ever burned; per-builder offline signing keys |

## 5. Out of scope (decided)

HDMI capture or any video path (ADR-001); UI generators and LVGL Pro (ADR-014); a browser or Android CLIENT in v1 (ADR-021);
CDC on the OTG port (ADR-018); settings changed from the CLIENT (ADR-023); runtime download of IME dictionaries (ADR-024).

## 6. Ideas for later (not planned)

Candidates the owner wants kept in view. None of them is a task: each one needs an ADR before any work, and the conflicts
listed here must be resolved there.

| Idea | Source | What it would take / conflicts |
|---|---|---|
| **USB extended screen**: the HOST sends its desktop over the same USB cable (a Windows Indirect Display Driver captures the desktop, compresses each frame, e.g. to JPEG, and sends it with a small per-frame header over a USB vendor interface); audio and touch can travel the same way, so KVMpad would also act as a secondary touch display | Espressif *USB Extended Screen Solution* (esp-iot-solution documentation, USB Host & Device chapter; supported on ESP32-S2/S3/P4 with USB-OTG) | Reopens ADR-001 (no video path). Needs a vendor interface next to the three HID interfaces, against the 4 usable IN endpoints of ADR-018; a host driver to install on the HOST (KVMpad is driverless today); JPEG decode and PSRAM bandwidth next to the RGB scan-out (TECH-021); and a mode switch so HID control and the extended screen never compete |

## 7. Next steps

Status 2026-11-02: M0–M2 done; M3–M8 implemented in software and largely exercised on board #1 (see the status column).
What is left needs either Linux tooling, a person at the board, or a second computer:

1. **TASK-071** — run the `kbd-layouts` workflow (`.github/workflows/kbd-layouts.yml`; libxkbcommon needs Linux), commit
   `components/kbd_layouts/generated/` and `clients/windows/layouts/` from its artifact, then the legend font subsets
   (TECH-078), the `host_layout` snapshot field (FR-745) and the KEYBOARD / Settings integration (FR-741/742).
2. **Owner at the board** — gestures with real fingers (the `?` card in the MOUSE view lists them), a setting changed by
   touch, typing on the KO / ZH / JA languages into a prepared HOST (AT-EXT-27), pairing with the 6-digit code and the
   Accept / Allow prompts (TASK-100), the formal AT-HID-05 (Ctrl+Alt+Del) and the AT-HID-09 re-run with a timestamped capture.
3. **Second computer** — remote control from the CLIENT (the CLIENT refuses capture on the computer the KVMpad is plugged
   into, to avoid a feedback loop).
4. **Release builds** — `-O2` and `lcd stats` `missed = 0` across view switches (ADR-035); OTA signed with the builder's own
   key (ADR-036).

## 8. Maintaining this file

Update the status column when a gate is passed (with the report path) or a task changes state per 05 §13; add a row when an ADR
introduces a task. Do not move task contents here — they stay in 05 and the ADRs.
