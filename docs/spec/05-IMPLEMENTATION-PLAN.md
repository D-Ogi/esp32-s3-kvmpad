# 05 — Implementation plan and task delegation

**Version:** 1.1.0 · **Goal:** running the implementation as small, independently verifiable units of work; every unit of work has an unambiguous contract, scope and test.

## 1. Working model — small, verifiable changes

Required roles (one person may hold several of them in consecutive sessions, but **not** under one giant instruction):

- **Coordinator / integrator:** watches dependencies, blockers/ADR, the schedule, the lockfile; does not edit the same files in parallel.
- **Implementer:** implements only one `TASK-xxx` on the basis of the indicated files and contracts.
- **Reviewer:** independently analyzes the diff from the point of view of requirements, safety, memory, API and the tests that were actually run.
- **Tester/HIL operator:** performs real tests on the Waveshare; does not write `PASS_HW` without the hardware connected.
- **Security reviewer:** checks the network client boundary, TLS, pairing, ownership, logs and OTA; for a release, independent of the author.

**Time/size rule:** one task → a small change (roughly 100–500 lines of logical code, excluding generated resources) and its own tests. This is not a rigid limit; a large task must be split up. Do not start `Remote TLS+UI+USB+OTA` all at once.

## 2. Product gates — milestones

```text
M0  Toolchain and proof-of-compatibility on Waveshare 7    [GATE: CAN BUILD + DETECT HW]
M1  BSP: CH422G + GT911 + RGB stable                       [GATE: LCD+MULTITOUCH HIL]
M2  USB device: boot keyboard + boot mouse                 [GATE: enumerate in Windows/Linux/UEFI]
M3  Pure HID state/arbiter + release-all                   [GATE: tests, no stuck]
M4  GestureFSM deterministic (1/2/5 point)                [GATE: vectors + fuzz]
M5  LVGL 800x480 Mouse / Keyboard / Shortcuts              [GATE: hitboxes + 3-mode HIL]
M6  NVS, Wi-Fi provisioning & independent statuses        [GATE: local works offline]
M7  Secure remote CLIENT (WSS+auth+approval+leases)       [GATE: security tests]
M8  OTA, security hardening, production recovery           [GATE: rollback, operator-controlled eFuse]
M9  Performance, soak, integration, release                [GATE: full acceptance]
```

**A `local-only` release** may take place after M6, provided it is not falsely presented as a finished remote KVM and it passes the critical USB safety tests. A `remote-enabled` release only after M7+ and full security tests.

## 3. Backlog of atomic tasks

### TASK-000 — Repo bootstrap / versions / build proof

**Input:** `README.md`, `02 §2`, the Waveshare repo and pinmap.  
**Scope:** component structure, CMake, toolchain v6.1, `idf_component.yml`, `dependencies.lock`, `sdkconfig.defaults`, build and environment document.  
**Deliverables:** compilation of the `app_main` program and a unit-test skeleton; log of `idf.py version`, `idf.py build`, `idf.py size-components`; Flash/PSRAM identified.  
**Tests:** L0; build clean + reproducible.  
**Prohibitions:** do not implement full HID or TLS; do not claim that the components are compatible before a build.  
**DoD:** `build=true`, versions frozen, no magic `*` in release dependencies, zero undocumented SDK changes.

### TASK-010 — Waveshare BSP / CH422G / USB mux

**Input:** `02 §1` and the Waveshare schematic.  
**Scope:** a central I²C manager, CH422G EXIO1..5, safe init, `EXIO5=LOW (USB)` and a `USB/CAN` multiplex test.  
**Deliverables:** `board_hal.c/h`, probe API, debug utility.  
**Tests:** I²C scan + expected output, USB mux confirmed, boot 100x.  
**Prohibitions:** do not occupy GPIO19/20 for CAN; do not write over i2c in parallel without a lock.  
**DoD:** real CH422G and GT911 addresses in the logs and a confirmed EXIO5 state.

### TASK-020 — LCD RGB and stable frame

**Input:** `02 §1,§6`; Waveshare sample timing.  
**Scope:** `esp_lcd_new_rgb_panel`, PSRAM RGB565 fb, bounce variants, VSYNC/PSRAM telemetry.  
**Deliverables:** 800×480 test pattern red/green/blue/text and an FPS log; A/B benchmark.  
**Tests:** powercycle, Wi-Fi load, flash write, RGB shift recovery.  
**Prohibitions:** do not implement an SPI display; do not raise PCLK blindly.  
**DoD:** the panel does not "float" and the colors are correct in a stable demo + HIL evidence.

### TASK-030 — GT911 touch, multi tracking

**Input:** `02 §5`, the contract `03 §2`.  
**Scope:** IRQ/poll, 0..5 touch, `track_id`, mapping 800×480, error/recovery, snapshots.  
**Deliverables:** `touch_input`, test geometry corners and a 5-point demo (on a separate diagnostic screen).  
**Tests:** AT-GEST-07 precondition, AT-FAULT-01, I²C error injection.  
**Prohibitions:** do not treat the order in the array as a finger identifier.  
**DoD:** HIL with 5 fingers + a reverse-order test with no jump.

### TASK-040 — Pure gesture engine

**Input:** `01 §3`, `03 §2` and VEC-001..004.  
**Scope:** a pure FSM from `TouchSample` to `HidIntent`; configuration thresholds, tap/longpress, 2-finger scroll, doubletap-drag, drag lock release.  
**Deliverables:** `gesture_engine.c/h`, bounded state, host unit tests + fuzz.  
**Tests:** AT-GEST-01..12 + ≥100k property sequences nightly.  
**Prohibitions:** do not read I²C and do not call TinyUSB/LVGL directly in the engine.  
**DoD:** all vectors deterministic, zero stuck buttons at the end of every sequence.

### TASK-050 — HID core/arbiter/USB descriptors

**Input:** `01 §4/5/6`, `02 §7`, `03 §3..5`.  
**Scope:** `pressed-ledger`, `release_all`, owner + epoch, report scheduler, TinyUSB boot keyboard + mouse, descriptors/LED output.  
**Deliverables:** host-unit pure core + USB/HIL descriptor trace + keymap.  
**Tests:** AT-HID-01..09, AT-KEY-04/05/10, VEC-005..009.  
**Prohibitions:** do not generate `tud_hid_*` from an LVGL callback; no dynamic queue; do not log keystrokes.  
**DoD:** Windows/Linux enumerate, manual BIOS/UEFI test; 10k reconnect/shortcut cycles without stuck.

### TASK-060 — LVGL system shell 800×480

**Input:** `reference/kvm-essential-800x480.html`, `01 §2`, `02 §6`.  
**Scope:** native LVGL9.6 shell, topbar, Mouse/Keyboard/Shortcuts routing, independent UI model, overlay; structure according to document 07. To be split into TASK-060A..E, and the view UI into TASK-070A..D; TASK-061 as a separate optional EEZ no-Flow spike.  
**Deliverables:** 3 screenshots real framebuffer, hitbox CSV/JSON (diagnostics offline), framerate metrics.  
**Tests:** AT-UI-01..08.  
**Prohibitions:** do not run HTML/WebView on the ESP; do not copy unverified graphical effects.  
**DoD:** all elements within 800×480, ≥44px touch target, no UI task blocking HID.

### TASK-070 — On-screen keyboard, shortcuts, trackpad UI

**Input:** `01 §3..5`, `03 §5.2`, the complete UI mockup.  
**Scope:** keyboard grid with all usages, Fn layer, one-shot/locked modifiers, Caps LED UI, SHORTCUTS, dedicated Right Click + Drag lock.  
**Deliverables:** `kbd_map`, `kbd_fsm`, UI wiring to arbiter (through typed event queue).  
**Tests:** AT-KEY-01..10, AT-UI-02, AT-GEST-12.  
**Prohibitions:** `Fn` is not HID; do not send an illegal AltGr as ASCII; do not inherit a locked Ctrl into shortcuts.  
**DoD:** 100% keys tested, screenshots + physical touch tests, zero stuck.

### TASK-080 — Status and network base

**Input:** `01 §6`, `02 §8`.  
**Scope:** USB state publisher, Wi-Fi STA/provisioning, CLIENT state read-only `DISABLED..LISTENING`, UI detail dialog.  
**Deliverables:** status service, connection state tests, local-only fallback.  
**Tests:** AT-CONN-01..05, AT-UI-04.  
**Prohibitions:** do not pretend CONNECTED when Wi-Fi alone has an IP; do not require the Internet for local HID.  
**DoD:** HOST, Wi-Fi and CLIENT independent; an isolated network failure breaks nothing locally.

### TASK-090 — NVS settings/recovery

**Input:** `01 §8`, `02 §9`.  
**Scope:** schema/versioned NVS, defaults, brightness/sensitivity/scroll, pairing erase, factory safe recovery.  
**Tests:** AT-FAULT-02/03/10, AT-SEC-08.  
**Prohibitions:** do not write flash on every move/tap; do not store keystrokes.  
**DoD:** a fault-injected corrupted NVS returns to safe operation.

### TASK-100 — Secure remote, pairing, protocol v1

**Input:** `03 §4..7`, `02 §8`, ADR-002 with an approved cert deployment.  
**Scope:** TLS/WSS, auth, pairing physical, read-only status, ownership leases, rate-limit, parser fuzz, event validation.  
**Tests:** AT-CONN-04..09, AT-SEC-01..08, AT-FAULT-06.  
**Prohibitions:** no `ws://` in production; no self-signed cert without established trust; remote injection before a grant; no auth tokens in the URL.  
**DoD:** an unauthorized remote generates no HID report at all.

### TASK-110 — Signed OTA, maintenance, rollback

**Input:** `02 §9`, `01 PROC-009`, ADR-003/004.  
**Scope:** power-safe OTA2 slots, signed image verification, health-check, revoked session and neutral HID, rollback/recovery.  
**Tests:** AT-FAULT-07/08, AT-PERF-09, AT-SEC-09.  
**Prohibitions:** do not program eFuses without operator approval; do not assume that the screen refreshes in bounce mode during flashing.  
**DoD:** two successful rollback tests with the power cut in different OTA phases.

### TASK-120 — Telemetry/benchmark and optimization

**Input:** `02 §10`, `04 §9`.  
**Scope:** input latency statistics, scanout/render, heap/PSRAM, status, 72h soak, comparison of bounce buffers, warnings.
**Tests:** AT-PERF-01..09, no regressions HID.  
**Prohibitions:** do not improve FPS at the cost of key-up, do not "speed things up" by means of unmeasured overclocking.  
**DoD:** a benchmark CSV with p50/p95/p99 and an explicit LCD configuration decision.

### TASK-130 — Release security review and packaging

**Input:** `04`, all the ADRs, the results of M0..M8.  
**Scope:** threat model check, secure boot/flash encryption readiness, VID/PID compliance, reproducible build, release notes/upgrade/rollback.
**Tests:** the complete release checklist `04 §13`.  
**Prohibitions:** the implementer does not perform the physical eFuse "burn"; only the operator, after an approved procedure.  
**DoD:** a signed release artifact + security summary + HIL report.

## 4. Required implementation order

1. TASK-000 → TASK-010 → TASK-020 + TASK-030 (in parallel only if branch/file ownership is independent).
2. TASK-050 (USB/HID) **before** or in parallel with TASK-040 (GestureFSM pure); integration only after both unit tests.
3. TASK-060 → TASK-070; feature v1 local-only ready for testing.
4. TASK-080 → TASK-090; full statuses + local UI without the network.
5. **ADR-002 approve** (remote client type, cert trust) → TASK-100, a separate security gate.
6. TASK-110 → TASK-120 → TASK-130; along the way, USB/GT911 regression tests after every improvement.

**It is not allowed** to start remote control in TASK-100 before a working `release_all`, `epoch` and USB HIL. This is a project safety blocker, not merely a schedule item.

## 5. Task entry contract for the implementer

The coordinator passes the implementer **only**:

```text
TASK_ID: TASK-040
FILES_ALLOWED_TO_EDIT: components/gesture_engine/**, test/host_unit/gesture_*.c
REQUIREMENTS: 01 §3 (FR-101..116), 03 §2 and §8 (VEC-001..004)
INTERFACES_FROZEN: kvm_touch_sample_t, kvm_hid_intent_t, as documented in 03
HARDWARE_AVAILABLE: false
MUST_NOT_TOUCH: components/usb_hid_device/**, components/board_hal/**
ACCEPTANCE: AT-GEST-01..12 with exact expected outputs
OUTPUT: source changes + tests + exact commands and real logs + uncertainties
```

The implementer **is not given the instruction** "build the whole KVM", "improve it however you like" or "rewrite the architecture". Every task is assessed on its own.

## 6. Implementer response format (mandatory)

```markdown
### TASK-040 — result
**Status:** IMPLEMENTED_SW / BLOCKED / PARTIAL / READY_FOR_HIL
**Requirement IDs:** FR-101..FR-116
**Files touched:** (exact paths)
**Interfaces changed:** none / list with rationale
**Commands actually run:** exact commands; mark NOT_RUN if not run
**Test results:** AT-GEST-01..12: PASS_SW / FAIL / NOT_RUN
**Proof:** logs paths, metrics, sample test output
**Known limitations:** list, e.g., depends on real GT911 IDs
**Risk and security:** possible races, failure modes
**Next action:** TASK-xxx / blocking ADR
```

**MUST:** you must not write `tests passed` without literally executed tests. **MUST:** you must not invent plausible logs, HIL data, measurements and API names. **MUST:** mark `NOT_RUN_HW` if there is no board. **MUST:** do not ignore `-Werror`, test failures or regressions in other modules.

## 7. Task brief template for IMPLEMENTER (copy-paste ready)

> You are implementing **one** TASK from KVM Essential. Treat `AGENTS.md` and the indicated fragments of the specification as a contract. Do not change files outside `FILES_ALLOWED_TO_EDIT`. First summarize the contracts of the public APIs and the list of tests. Then implement the smallest complete change, add tests and run them. Do not use unverified API names — check the local headers and the versions in `dependencies.lock`. Preserve release_all, epoch, no dynamic allocation on the hot path. If an inconsistency appears, stop that fragment, create an ADR and inform the coordinator. At the end, give the commands actually executed and the results; do not claim hardware tests for yourself without hardware.

## 8. Task brief template for REVIEWER (copy-paste ready)

> Do a critical review of the diff for `TASK_ID`. Check the requirements and contracts, whether the tests really cover them, whether real APIs existing in dependencies.lock were used, whether there is no race/use-after-free/stack overflow/out-of-bounds, whether we will not lose KEY_UP/MOUSE_UP and whether the public contract has not been violated. Rate every bug: `BLOCKER`, `MAJOR`, `MINOR`, `NIT`; for BLOCKER and MAJOR give the exact file:line, a reproduction scenario and the shortest fix. Do not add features beyond the scope and do not accept "I tried it and it works" without test results.

## 9. Task brief template for TESTER / HIL (copy-paste ready)

> Install build `<SHA>` on the Waveshare ESP32-S3-Touch-LCD-7. Record the revision, Flash/PSRAM, the power supply scheme, the HOST version and the USB configuration. Run only the `AT-*` tests assigned to the task. Do not perform unauthorized eFuse operations. Keep the logs and the disconnection trials free of sensitive data. Mark every test PASS / FAIL / NOT_RUN_HW with evidence. Report regressions and do not assume that a GUI screenshot proves that HID works.

## 10. Task brief template for SECURITY REVIEWER

> Treat the Wi-Fi CLIENT as an untrusted boundary. List the possible injection paths into HID, missing auth/Origin, replay, stale lease, race local override, accidental replay of held modifiers, a missing neutral report during OTA, secrets in the logs and firmware, self-signed WSS without trust, VID/PID compliance and eFuse irreversibility. For every path, point out the existing or missing test from `04`. In particular, prove that a CLIENT without a granted lease emits zero reports to the HOST.

## 11. CODE REVIEW gate (blocking list)

A PR is automatically **REJECT** when even one condition occurs:

- There is no associated `TASK-ID`, `FR/TECH-ID`, `AT-ID`.
- The diff contains files outside the assignment without agreement with the coordinator.
- The code calls `tud_hid_*` directly from an ISR/touch/UI/network callback.
- No `RELEASE_ALL` in a new error/timeout path.
- HID output can pass from remote without authorization, owner and lease epoch.
- Unknown APIs/field names taken from memory, not from local headers.
- It allocates `malloc/free` per touch sample / HID report.
- It displays the "Host Active" status on the basis of VBUS alone.
- It keeps a dynamic, unbounded input queue.
- It exposes passwords/keystrokes in the logs, uses ws/plaintext in a release.
- It modifies the USB report descriptor without a HIL plan.
- It enables secure boot/flash encryption on a development board without consent.
- It assumes that the product captures the host screen or is HDMI capture, although there is no video path.
- It declares `PASS_HW` on the basis of a mockup or a compilation.

## 12. Rules of concurrent work and merging

- The coordinator creates branches per TASK: `task/040-gesture-fsm`, `task/050-usb-hid` etc.
- Two implementers do not edit `hid_core` or the `03` contracts at the same time, even if they are different methods in one file. A contract change goes through a PR with an integration test.
- An implementer may do only one task PR at a time; stale dependencies in `dependencies.lock` are a BLOCKER, not a reason to update everything on one's own.
- PR sequence: compile/tests → reviewer → unit merge → HIL gate → marker Mx accepted. Any regressions → rollback to the last green SHA.
- If a task has >2 public APIs and touches >3 components, it requires decomposition (unless the integration is itself the content of the task).
- A change in the critical HID path after M5 requires a replay of the `AT-HID`, `AT-KEY`, `AT-GEST`, `AT-CONN` tests.

## 13. Staged Definition of Done

| Status | When it may be entered |
|---|---|
| `DESIGNED` | the contract and test plan are known; no implementation |
| `IMPLEMENTED_SW` | the compilation and the required native unit tests actually passed |
| `READY_FOR_HIL` | reviewer approved, no BLOCKER, software tests PASS |
| `VERIFIED_HW` | the tester gave the board rev, SHA, real HIL logs and PASS |
| `RELEASE_CANDIDATE` | security + soak + perf gating complete |
| `RELEASED` | the artifact is signed, the rollback procedures and keys are approved |
| `BLOCKED` | it has an `ADR` document or a reproduceable failing test |

## 14. Separate performance-optimization track

Only after correctness is closed:

1. Baseline profile `NFR-001..009`; instrumentation without keystrokes.
2. LCD RGB variants `fb+10line bounce`, `fb+20line bounce`, `double fb`.
3. Comparison of CPU affinity/priority, I²C speed and GUI invalidation.
4. Choose the minimum complexity that satisfies input latency and stability.
5. Every perf commit must run the `10k release-all` test and the `AT-GEST` tests.

**We do not accept** "-O3 improves performance" without a measurement on the target ESP32-S3 or "I rewrote the UI for fps" without a test of HID latency under Wi-Fi.


## 11. UI plan extension (v1.1.0)

TASK-060 requires the small tasks `TASK-060A` to `TASK-060E` from document `07` to be carried out first, and TASK-070 analogously `TASK-070A` to `TASK-070D`; `TASK-061` evaluates the optional EEZ Studio editor without Flow. New tests `AT-UIFW-01..10`. UI implementers must read `AGENTS-UI.md`. The architecture and the typed contracts are in `07`, not in the editor's generated code.
