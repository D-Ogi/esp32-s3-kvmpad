# `ui_core`

> **Status:** TASK-060B `IMPLEMENTED_SW` (2026-11-02): contracts, bounded state mailbox, LVGL Subjects store and the
> typed action gateway, host-tested against the real LVGL 9.6 sources; wired into the firmware (`main/ui_glue.c`) and
> checked on board #1 with `ui store`. Router/overlay (TASK-060C) not started.

**Role:** typed state mailbox, `ui_store` (presentation projection as native LVGL Subjects), typed actions; later the
router, overlay and lifecycle (TASK-060C).

| | |
|---|---|
| Tasks | TASK-060B (store/subjects/mailbox), TASK-060C (router/overlay) |
| Specification | 07 §1–5, §10; AGENTS-UI.md; ADR-027 (LVGL heap in PSRAM) |
| Tests | `test/ui_store/` (host, real LVGL 9.6); AT-UIFW-02/03/09 at store level; `ui store` on the board |

| File | Content |
|---|---|
| `include/ui_contracts.h` | `ui_view_id_t`, `ui_action_type_t`, `ui_action_t`, `ui_state_snapshot_t` (07 §3) + `sources`, per-source slices, `ui_event_t`; HOST/owner enums are hid_core's, CLIENT mirrors 01 §6.2 |
| `include/ui_mailbox.h`, `src/ui_mailbox.c` | bounded mailbox: level-triggered state per source (newest wins, late domain sequence refused, invalid enum refused, unchanged coalesced) + a 16-entry event ring (overflow drops the oldest and forces a resync) |
| `include/ui_store.h`, `src/ui_store.c` | `lv_subject_create(LV_SUBJECT_TYPE_INT)` per field; domain subjects are `UI_STORE_UNKNOWN` (-1) until their source publishes; one `ui_store_tick()` per UI tick |
| `include/ui_actions.h`, `src/ui_actions.c` | validation of 07 §3 values, request ids, local sink (router) vs domain sink (hid_dispatch in firmware) |
| `include/ui_present.h`, `src/ui_present.c` | status wording of 01 §6.1/§6.2; unknown never upgraded |

**Data flow (firmware, `main/ui_glue.c`):** a 20 ms `esp_timer` publisher copies `hid_dispatch_get_snapshot()`, the
HOST LED report and the GT911 health into the mailbox (no LVGL call); an `lv_timer` in the LVGL task applies the newest
snapshot to the Subjects. CLIENT is published as `DISABLED` (no network core before M7); drag lock stays unknown until
the gesture engine runs live (TASK-060E). No view submits actions yet (TASK-070A..C).

**Rules (MUST):**

- Data binding through the native LVGL Observer — without a custom Redux/DOM/DSL.
- The UI is not the source of truth for HOST_ACTIVE, CLIENT_CONNECTED, ownership, Caps LED, drag lock, DOWN keys.
- No `tusb.h` / `tinyusb.h` and no network transport headers.
- Subjects and the store are touched only by the LVGL owner (ui_platform task or under `ui_platform_lock()`).

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
