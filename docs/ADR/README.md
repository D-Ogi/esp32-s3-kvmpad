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
| ADR-008 | RGB single/double PSRAM framebuffers | EXPERIMENT in TASK-020 | spec/06 §9 |
| ADR-009 | 8 MiB or 16 MiB Flash | **RESOLVED 2026-11-01: 16 MiB Flash, 8 MiB PSRAM** (board unit #1, esptool measurement) | spec/06 §10 → `hardware/boards.md` |
| ADR-010 | HOST state at the USB level | ACCEPTED | spec/06 §11 |
| ADR-011 | LOCAL/REMOTE arbitration | ACCEPTED | spec/06 §12 |
| ADR-012 | Production firmware protections | 🔒 ACCEPTED (REFERENCE), release procedures pending approval | spec/06 §13 |
| ADR-013 | KVMpad UI Core on LVGL 9.6 | ACCEPTED (REFERENCE), PENDING-HW | spec/06 + spec/07 |
| ADR-014 | No UI generators (EEZ Studio, LVGL Pro Editor) | **ACCEPTED** 2026-11-01 | [ADR-014-no-ui-generators.md](ADR-014-no-ui-generators.md) |
| ADR-015 | Component for TASK-080 (Wi-Fi/status) | _reserved, to be written_ | — |
| ADR-016 | Placement of `kbd_fsm` (modifiers, Fn) | _reserved, to be written_ | — |
| ADR-017 | Wi-Fi: modem sleep off + fast reconnect | **ACCEPTED**, PENDING-HW | [ADR-017-wifi-latency-fast-reconnect.md](ADR-017-wifi-latency-fast-reconnect.md) |
| ADR-018 | Consumer Control + horizontal scrolling | **ACCEPTED**, PENDING-HW | [ADR-018-consumer-control-hscroll.md](ADR-018-consumer-control-hscroll.md) |
| ADR-019 | Type text on the CLIENT side | **ACCEPTED** (after M7) | [ADR-019-type-text-client-side.md](ADR-019-type-text-client-side.md) |
| ADR-020 | Keep-awake (mouse jiggler) | **ACCEPTED**, PENDING-HW | [ADR-020-keep-awake.md](ADR-020-keep-awake.md) |
| ADR-021 | Native Windows CLIENT → ADR-002 variant A | **ACCEPTED** 2026-11-01 | [ADR-021-native-windows-client.md](ADR-021-native-windows-client.md) |
| ADR-022 | Pairing: 6-digit code + PAKE, no other typing in | **ACCEPTED** 2026-11-01; PAKE algorithm to be confirmed in TASK-100 | [ADR-022-pairing-6-digit-pake.md](ADR-022-pairing-6-digit-pake.md) |
| ADR-023 | Approval policies configurable from the device UI; hard invariants without a switch | **ACCEPTED** 2026-11-01 | [ADR-023-user-configurable-approval-policies.md](ADR-023-user-configurable-approval-policies.md) |

New requirements from ADR-017 onwards (ID, task, component, tests): [`../EXTENSIONS.md`](../EXTENSIONS.md).
The ADR-015/016 gaps are described in [`../REPO-LAYOUT.md`](../REPO-LAYOUT.md).

**The statuses of ADR-002…012** in the table are references — read the current status in spec/06.
ADR-001/002/004/012 **are not changed** without the owner's approval (spec/06 §16).

## Decisions awaiting the owner

_None — as of 2026-11-01. ADR-009 resolved by a measurement of board unit #1 (`hardware/boards.md`)._

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
