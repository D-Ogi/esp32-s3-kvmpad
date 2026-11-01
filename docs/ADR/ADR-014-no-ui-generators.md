# ADR-014 — No UI generators: only LVGL 9.6 + native Observer + KVMpad UI Core
Status: ACCEPTED
Date: 2026-11-01
Approver: project owner (decision of 2026-11-01)

## Context

- ADR-013 (spec/06, spec/07 §0) adopts LVGL 9.6, the native Observer and our own UI Core (router, store, typed actions).
- spec/07 §8 and AGENTS-UI §8 allowed an **optional** EEZ Studio spike (LVGL, no-Flow) as TASK-061,
  with generated code in `components/ui_generated/` and the project in `design/kvm.eez-project`.
- AGENTS-UI §9 already excludes LVGL Pro runtime XML and the LVGL Pro Editor without a separate ADR.

## Options

- **A.** EEZ Studio no-Flow spike (TASK-061), ACCEPT/REJECT decision after measurements.
- **B.** Giving up any code generators / visual editors; UI written by hand in C/LVGL.

## Decision

**B.** Dropped: EEZ Studio (also without Flow), EEZ Flow, LVGL Pro Editor, LVGL Pro runtime XML
and other UI code generators. UI stack: **LVGL 9.6 + native Observer (`lv_subject_*`) + KVMpad UI Core**.

Consequences for the structure and the tasks:

- TASK-061 → **CANCELLED**. Neither `components/ui_generated/` nor `design/` is created.
- AT-UIFW-06 (EEZ re-export) → **N/A**.
- The fallback variant from spec/07 §8 applies: `ui_components` and `ui_views` in pure C/LVGL,
  factory functions of 10–50 lines and descriptor tables; **no** custom DSL in XML/JSON.
- The AGENTS-UI §8 rules (EEZ) cease to apply; §9 (the LVGL Pro ban) remains in force.
- The adapter choice (`esp_lvgl_adapter` 0.7.4 vs `esp_lvgl_port` 2.9.0) remains unchanged — TASK-060A.

## Consequences

- Security/HID: no second place for logic outside the C contracts — a simpler review of the HID path.
- Licensing: no dependency on generated-code licenses or on commercial editors.
- UX/work: the layout is designed in code; preview through the SDL simulator (`simulator/lvgl_sdl/`), visual pattern `reference/`.
- `ui_bindings` (mentioned in spec/07 §8) is no longer resolved by TASK-061 — see `docs/REPO-LAYOUT.md`.

## Tests

AT-UIFW-01..05, 07..10 unchanged. AT-UIFW-06: N/A (this ADR).

## Migration

None — nothing has been generated or imported.
