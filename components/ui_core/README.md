# `ui_core`

The UI core owns the bounded state mailbox, native LVGL Subjects, typed actions, view router and overlay lifecycle.

**Tests.** `test/ui_store/` (host, real LVGL 9.6); AT-UIFW-02/03/09 at store level; `ui store` on the board

| File | Content |
|---|---|
| `include/ui_contracts.h` | `ui_view_id_t`, `ui_action_type_t`, `ui_action_t`, `ui_state_snapshot_t` (07 §3) + `sources`, per-source slices, `ui_event_t`; HOST/owner enums are hid_core's, CLIENT mirrors 01 §6.2 |
| `include/ui_mailbox.h`, `src/ui_mailbox.c` | bounded mailbox: level-triggered state per source (newest wins, late domain sequence refused, invalid enum refused, unchanged coalesced) + a 16-entry event ring (overflow drops the oldest and forces a resync) |
| `include/ui_store.h`, `src/ui_store.c` | `lv_subject_create(LV_SUBJECT_TYPE_INT)` per field; domain subjects are `UI_STORE_UNKNOWN` (-1) until their source publishes; one `ui_store_tick()` per UI tick |
| `include/ui_actions.h`, `src/ui_actions.c` | validation of 07 §3 values, request ids, local sink (router) vs domain sink (hid_dispatch in firmware) |
| `include/ui_router.h`, `include/ui_overlay.h` | view navigation, overlay stacking and lifecycle |
| `include/ui_present.h`, `src/ui_present.c` | status wording of 01 §6.1/§6.2; unknown never upgraded |

**Data flow (firmware, `main/ui_glue.c`):** a 20 ms `esp_timer` publisher copies `hid_dispatch_get_snapshot()`, the
HOST LED report and the GT911 health into the mailbox (no LVGL call); an `lv_timer` in the LVGL task applies the newest
snapshot to the Subjects. The network slice uses Wi-Fi, remote session and HID ownership state. The gesture slice publishes drag lock. Views submit typed actions through the router and domain sinks.

**Rules (MUST):**

- Data binding through the native LVGL Observer — without a custom Redux/DOM/DSL.
- The UI is not the source of truth for HOST_ACTIVE, CLIENT_CONNECTED, ownership, Caps LED, drag lock, DOWN keys.
- No `tusb.h` / `tinyusb.h` and no network transport headers.
- Subjects and the store are touched only by the LVGL owner (ui_platform task or under `ui_platform_lock()`).
