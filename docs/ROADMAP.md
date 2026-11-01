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
| ADR-014 … ADR-025 accepted (ADR-015/016 reserved, to be written) | `docs/ADR/README.md`, `docs/EXTENSIONS.md` |
| Board unit #1 identified: 16 MiB Flash, 8 MiB PSRAM, rev v0.2 (ADR-009 resolved) | `hardware/boards.md` |
| Repository skeleton: component READMEs with roles, rules, tests; contribution rules; license notices | `docs/REPO-LAYOUT.md`, `CONTRIBUTING.md` |
| Public repository with description and topics | github.com/D-Ogi/esp32-s3-kvmpad |

Nothing compiles yet. No hardware test has been run; every hardware claim below is `NOT_RUN_HW` until a report says otherwise.

## 2. Milestones, gates and tasks

The table merges 05 §2/§3 with the tasks that the ADRs added. "Inputs" names the decisions a task must implement.

| Milestone | Gate (05 §2) | Tasks | Inputs from ADRs | Status |
|---|---|---|---|---|
| **M0** Toolchain, proof of compatibility | CAN BUILD + DETECT HW | TASK-000 | ADR-003 (ESP-IDF v6.1, pinned components), ADR-009 (16 MiB), ADR-024 TECH-081 (`ime_data` partition), ADR-025 TECH-083/084 (header checker in CI, retained notice string) | **NEXT** |
| **M1** BSP: CH422G + GT911 + RGB | LCD + MULTITOUCH HIL | TASK-010, TASK-020, TASK-030 | ADR-008 (framebuffer experiment in TASK-020) | DESIGNED |
| **M2** USB device: boot keyboard + mouse | enumerate in Windows / Linux / UEFI | TASK-050 (USB part) | ADR-005, ADR-010 (HOST state from USB), ADR-018 (3rd interface Consumer Control, `AC Pan`, no CDC on OTG) | DESIGNED |
| **M3** HID core, arbiter, `RELEASE_ALL` | tests, no stuck key | TASK-050 (core part), **ADR-016** (where `kbd_fsm` lives — write before TASK-070B) | ADR-011 (LOCAL/REMOTE arbitration), ADR-018 (Consumer in the ledger), ADR-020 (`KVM_HID_KEEPALIVE`), ADR-024 FR-748 (cancellable burst) | DESIGNED |
| **M4** Deterministic GestureFSM | vectors + fuzz | TASK-040 | ADR-007 | DESIGNED |
| **M5** LVGL 800×480 UI | hitboxes + 3-mode HIL | TASK-060A (adapter spike, **decides ADR-013**), TASK-060B/C/D/E, TASK-070A/B/C/D, **TASK-071** (layouts), **TASK-072** (IME) | ADR-013 (UI Core), ADR-014 (no generators, TASK-061 cancelled), ADR-022/023 (Settings → Remote screens, policies), ADR-024 (Host layout, composition mode, delivery modes), ADR-025 FR-751 (Settings → About) | DESIGNED |
| **M6** NVS, Wi-Fi provisioning, independent statuses | local works offline | **ADR-015** (component for TASK-080 — write first), TASK-080, TASK-090 | ADR-017 (modem sleep off, BSSID cache), ADR-020/022/023/024 (NVS keys and defaults) | DESIGNED |
| **Release `local-only`** | after M6; the critical USB safety tests pass; not presented as a remote KVM | release checklist 04 §13 (local scope) | ADR-024: the on-device IME is **inside** this gate, not after it | — |
| **M7** Secure remote CLIENT | security tests | TASK-100, **TASK-105** (native Windows CLIENT) | ADR-021 (variant A, certificate pinning), ADR-022 (mDNS + 6-digit code + PAKE; algorithm confirmed in TASK-100), ADR-023 (approval policies), ADR-019/024 (type text on the CLIENT, `host_layout`, delivery modes), ADR-025 TECH-084 (`LegalCopyright`) | BLOCKED by design until `release_all` + epoch + USB HIL (05 §4) |
| **Release `remote-enabled`** | after M7 and full security tests | 04 §13 (full) | — | — |
| **M8** OTA, hardening, recovery | rollback; eFuse only by the operator | TASK-110 | ADR-012 (production protections, owner decision for eFuses), ADR-024 TECH-081 (`ime_data` shared by both slots) | DESIGNED |
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
| ADR-015, ADR-016 (reserved) | written before TASK-080 / TASK-070B |
| ADR-013: `esp_lvgl_adapter` vs `esp_lvgl_port` | TASK-060A with HIL evidence |
| ADR-004 (VBUS separation), ADR-008 (framebuffers), ADR-017 (modem sleep), ADR-018 (3rd interface), ADR-020 (keep-awake) | PENDING-HW — measurements in their tasks |
| ADR-022 PAKE algorithm | TASK-100 |
| ADR-024 IME footprint and latency | TASK-072 spike on board #1 |
| ADR-012 eFuse / secure boot procedures | owner decision + recovery plan before TASK-130 |

## 5. Out of scope (decided)

HDMI capture or any video path (ADR-001); UI generators and LVGL Pro (ADR-014); a browser or Android CLIENT in v1 (ADR-021);
CDC on the OTG port (ADR-018); settings changed from the CLIENT (ADR-023); runtime download of IME dictionaries (ADR-024).

## 6. Next steps

1. **TASK-000** — ESP-IDF v6.1 installed in the workspace tooling directory (never system-wide), first `idf.py build`, pinned
   `dependencies.lock`, partition table with ≥ 2 OTA slots and `ime_data`, CI running the manifest and license-header checks.
2. **TASK-040 and TASK-050 core** on the host, in parallel with **M1** on the board.
3. **ADR-015 and ADR-016** before TASK-080 and TASK-070B start.

## 7. Maintaining this file

Update the status column when a gate is passed (with the report path) or a task changes state per 05 §13; add a row when an ADR
introduces a task. Do not move task contents here — they stay in 05 and the ADRs.
