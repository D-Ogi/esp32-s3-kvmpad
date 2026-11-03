# Architecture Decision Records

ADR-001 … ADR-013 are recorded in the specification: [`../spec/06-DECISIONS-AND-RISKS.md`](../spec/06-DECISIONS-AND-RISKS.md)
(ADR-013 expanded in [`../spec/07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md`](../spec/07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md)).
The specification is frozen — **new decisions and status changes** go here as separate files.

## Index

| ID | Title | Status | File |
|---|---|---|---|
| ADR-001 | What KVM and CLIENT mean | ACCEPTED (REFERENCE) | spec/06 §2 |
| ADR-002 | Encryption and the remote CLIENT type | **RESOLVED 2026-11-01 → variant A** (ADR-021); default Remote OFF unchanged | spec/06 §3 + ADR-021 |
| ADR-003 | ESP-IDF/components, versioning | ACCEPTED (REFERENCE) + PENDING-HW proof | spec/06 §4 |
| ADR-004 | Power supply, VBUS separation | 🔒 PENDING-HW | spec/06 §5 |
| ADR-005 | USB boot interfaces | ACCEPTED (REFERENCE), HIL required | spec/06 §6 |
| ADR-006 | Native LVGL instead of embedded HTML | ACCEPTED | spec/06 §7 |
| ADR-007 | GestureFSM vs LVGL9 gestures | ACCEPTED (REFERENCE) | spec/06 §8 |
| ADR-008 | RGB single/double PSRAM framebuffers | EXPERIMENT in TASK-020 — interim 2026-11-02: A/B/C stable on board #1 after the cache change (32 KiB I / 64 KiB 64 B D), D (no bounce) rejected, variant A is the working default; final after p95 touch-to-HID + 72 h soak (`test/reports/2026-11-02_M0-M2_d36ab0a.yaml`) | spec/06 §9 |
| ADR-009 | 8 MiB or 16 MiB Flash | **RESOLVED 2026-11-01: 16 MiB Flash, 8 MiB PSRAM** (board unit #1, esptool measurement) | spec/06 §10 → `hardware/boards.md` |
| ADR-010 | HOST state at the USB level | ACCEPTED | spec/06 §11 |
| ADR-011 | LOCAL/REMOTE arbitration | ACCEPTED | spec/06 §12 |
| ADR-012 | Production firmware protections | 🔒 ACCEPTED (REFERENCE), release procedures pending approval | spec/06 §13 |
| ADR-013 | KVMpad UI Core on LVGL 9.6 | ACCEPTED (REFERENCE), PENDING-HW | spec/06 + spec/07 |
| ADR-014 | No UI generators (EEZ Studio, LVGL Pro Editor) | **ACCEPTED** 2026-11-01 | [ADR-014-no-ui-generators.md](ADR-014-no-ui-generators.md) |
| ADR-015 | Component for TASK-080: new `net_status` owns the Wi-Fi STA link and its status; USB state stays in `usb_hid_device`; CLIENT states above `NETWORK_DOWN` belong to `secure_remote` | **ACCEPTED** 2026-11-02 | [ADR-015-network-status-component.md](ADR-015-network-status-component.md) |
| ADR-016 | Placement of `kbd_fsm` (modifiers, Fn): in `hid_core`, run by `hid_dispatch` | **ACCEPTED** 2026-11-02 | [ADR-016-kbd-fsm-placement.md](ADR-016-kbd-fsm-placement.md) |
| ADR-017 | Wi-Fi: modem sleep off + fast reconnect | **ACCEPTED**, PENDING-HW | [ADR-017-wifi-latency-fast-reconnect.md](ADR-017-wifi-latency-fast-reconnect.md) |
| ADR-018 | Consumer Control + horizontal scrolling | **ACCEPTED**, PENDING-HW | [ADR-018-consumer-control-hscroll.md](ADR-018-consumer-control-hscroll.md) |
| ADR-019 | Type text on the CLIENT side | **ACCEPTED** (after M7) | [ADR-019-type-text-client-side.md](ADR-019-type-text-client-side.md) |
| ADR-020 | Keep-awake (mouse jiggler) | **ACCEPTED**, PENDING-HW | [ADR-020-keep-awake.md](ADR-020-keep-awake.md) |
| ADR-021 | Native Windows CLIENT → ADR-002 variant A | **ACCEPTED** 2026-11-01 | [ADR-021-native-windows-client.md](ADR-021-native-windows-client.md) |
| ADR-022 | Pairing: 6-digit code + PAKE, no other typing in | **ACCEPTED** 2026-11-01; PAKE algorithm to be confirmed in TASK-100 | [ADR-022-pairing-6-digit-pake.md](ADR-022-pairing-6-digit-pake.md) |
| ADR-023 | Approval policies configurable from the device UI; hard invariants without a switch | **ACCEPTED** 2026-11-01 | [ADR-023-user-configurable-approval-policies.md](ADR-023-user-configurable-approval-policies.md) |
| ADR-024 | Keyboard layouts from xkeyboard-config + on-device IME (JP/zh/ko) with per-OS Unicode delivery | **ACCEPTED** 2026-11-01; IME footprint PENDING-HW (TASK-072 spike) | [ADR-024-keyboard-layouts-xkeyboard-config.md](ADR-024-keyboard-layouts-xkeyboard-config.md) |
| ADR-025 | License notices: three-line header in every source file, checker in CI, notice embedded in firmware and CLIENT, `Settings → About` | **ACCEPTED** 2026-11-02 | [ADR-025-license-notices.md](ADR-025-license-notices.md) |
| ADR-026 | Target board = ESP32-S3-Touch-LCD-7B (1024×600, CH32V003 expander at 0x24); 7B exclusion of `AGENTS.md` §1 lifted; TECH-085..088 | **ACCEPTED** 2026-11-02 | [ADR-026-target-board-7b.md](ADR-026-target-board-7b.md) |
| ADR-027 | LVGL integration backend: `esp_lvgl_adapter` 0.7.4, `DOUBLE_DIRECT` default (closes ADR-013 PENDING-HW) | **ACCEPTED** 2026-11-02 | [ADR-027-lvgl-integration-backend.md](ADR-027-lvgl-integration-backend.md) |
| ADR-028 | Windows CLIENT stack (.NET 9), authentication handshake and pairing-crypto interop (TASK-105) | **ACCEPTED** 2026-11-02 | [ADR-028-windows-client-stack.md](ADR-028-windows-client-stack.md) |
| ADR-029 | Pairing: EC-JPAKE P-256 through PSA, channel binding to the device certificate, `pair.*` wire format (TASK-100) | **ACCEPTED** 2026-11-02 | [ADR-029-pairing-wire-format-and-pake.md](ADR-029-pairing-wire-format-and-pake.md) |
| ADR-030 | Protocol v1 details the specification leaves open (`remote_protocol`, TASK-100) | **ACCEPTED** 2026-11-02 | [ADR-030-remote-protocol-component.md](ADR-030-remote-protocol-component.md) |
| ADR-031 | OTA image source (local HTTPS pull), signing without eFuse, update ordering, health check and rollback (TASK-110) | **ACCEPTED** 2026-11-02 | [ADR-031-ota-source-and-signing.md](ADR-031-ota-source-and-signing.md) |
| ADR-032 | TLS and WebSocket transport for the remote session: `esp_https_server` WSS, pre-handshake admission, per-device certificate in NVS, mDNS (`secure_remote`, TASK-100) | **ACCEPTED** 2026-11-02 | [ADR-032-remote-transport.md](ADR-032-remote-transport.md) |
| ADR-033 | IME candidate fonts (Noto Sans SC / JP) in `ime_data`, LVGL-compressed and read in place; `ime_data` grows to 7.7 MiB (TASK-072) | **ACCEPTED** 2026-11-02 | [ADR-033-ime-data-candidate-fonts.md](ADR-033-ime-data-candidate-fonts.md) |
| ADR-034 | No permanent flash mappings: IME fonts streamed through LVGL's glyph loader, OpenWnn dictionaries copied to PSRAM (a mapping disables the cache on every OTA / NVS write; amends ADR-033 item 1) | **ACCEPTED** 2026-11-02 | [ADR-034-no-permanent-flash-mappings.md](ADR-034-no-permanent-flash-mappings.md) |
| ADR-035 | Scan-out stability: `DOUBLE_PARTIAL` default with a 12-line internal strip (`DOUBLE_DIRECT` lost 3-5 frames per view switch: the 10-line jumps), panel callbacks chained so `lcd stats` keeps counting under LVGL, LCD_CAM stopped in a shutdown handler before a requested restart (amends ADR-027 items 2 and 5) | **ACCEPTED** 2026-11-02 | [ADR-035-scanout-stability.md](ADR-035-scanout-stability.md) |
| ADR-036 | OTA signing keys: one per builder, kept offline, never in the repository or CI; no eFuse is ever burned (Secure Boot / Flash Encryption out of scope; amends ADR-012 and ADR-031) | **ACCEPTED** 2026-11-02 | [ADR-036-signing-keys-per-builder-no-efuse.md](ADR-036-signing-keys-per-builder-no-efuse.md) |
| ADR-037 | One language key on the KEYBOARD view instead of the `KO` / `ZH` / `JA` caps: tap cycles, long press opens a picker; the active input languages are toggles in `Settings → Keyboard` (record schema 3, default English only; amends ADR-024, adds FR-752) | **ACCEPTED** 2026-11-02 | [ADR-037-input-language-key.md](ADR-037-input-language-key.md) |
| ADR-039 | Choosing one of 581 HOST layouts (FR-741): the picker gets a current-layout card with a home-row preview, up to 8 favorites, the last 5 recents, a search field with its own on-screen keyboard, and the two-level list with a nine-bucket index strip and six recycled rows; a long press on the KEYBOARD layout strip opens a quick-switch overlay; favorites and recents are stored as ids (record schema 4) and the shell, not the UI, owns their ordering (amends ADR-024) | PROPOSED 2026-11-03 | [ADR-039-host-layout-picker.md](ADR-039-host-layout-picker.md) |
| ADR-038 | RGB pixel clock 25 MHz (27.3 Hz scan-out): the 30 MHz vendor value starved the bounce-buffer refill while a finger was on the screen (25 → 2 missed frames in 70 s); rejected by the owner for a constant flicker of the scrim under modals; 30 MHz stays, the touch-time jumps are open | **REJECTED** 2026-11-03 | [ADR-038-pixel-clock-25mhz.md](ADR-038-pixel-clock-25mhz.md) |

New requirements from ADR-017 onwards (ID, task, component, tests): [`../EXTENSIONS.md`](../EXTENSIONS.md).
The ADR-015 gap is described in [`../REPO-LAYOUT.md`](../REPO-LAYOUT.md).

**The statuses of ADR-002…012** in the table are references — read the current status in spec/06.
ADR-001/002/004/012 **are not changed** without the owner's approval (spec/06 §16).

## Decisions awaiting the owner

_None — as of 2026-11-02._ ADR-009 resolved by a measurement of board unit #1 (`hardware/boards.md`); ADR-024 accepted 2026-11-01, ADR-025, ADR-026, ADR-015, ADR-016 and ADR-027..037 2026-11-02.

## Template — `ADR-<num>-<slug>.md`

```markdown
# ADR-0xx — Title
Status: PROPOSED | ACCEPTED | REJECTED
Date: YYYY-MM-DD
Context: observable facts with source links
Options: A/B/C and pros/cons
Decision: exact selected option
Consequences: security/memory/API/UX
Tests: AT-... and evidence paths
Migration: steps if replacing existing implementation
Approver: project owner / integration lead
```
