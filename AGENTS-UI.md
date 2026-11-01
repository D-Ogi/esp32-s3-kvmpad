# AGENTS-UI.md — rules for agents working on KVMpad UI Core

**Version:** 1.1.0 | **Target stack:** LVGL 9.6 | **Hardware:** ESP32-S3-Touch-LCD-7 (GT911, 800×480)  
**Source of truth:** `07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md`, then the earlier documents 01–06. The HID and security constraints from `AGENTS.md` remain overriding.

## MUST invariants

1. Do not create a graphics framework: LVGL provides widgets, styles, Flex/Grid, animations and Observer. `ui_core` only navigation, state projection, lifecycle, typed actions and overlay.
2. **One GT911 read path.** Do not read I²C separately from LVGL and from `gesture_engine`. `TouchFrame` must have a stable `track_id`, timestamp and sequence.
3. **The sole owner of LVGL** is the defined task/adaptor context; the USB, Wi-Fi, gesture and ISR tasks do not call `lv_obj_*`, `lv_subject_set_*` or `lv_timer_handler`.
4. **The UI does not emit USB HID.** Only typed intents → InputArbiter → HidScheduler. No `tusb.h`, `tinyusb.h` include in `ui_core`, `ui_views`, `ui_components`.
5. **The UI is not the source of truth** for HOST_ACTIVE, CLIENT_CONNECTED, HID ownership, Caps Lock LED, drag lock and DOWN keys. This data must come from authoritative domain snapshots.
6. During a mode switch you must cancel the pending touch/gesture, order a HID release and only then show the new screen without any remaining sticky states. The GUI may report a timeout, but it does not decide the USB state optimistically.
7. Do not implement your own DOM, CSS, XML runtime, layout parser or Redux. LVGL Observer provides minimal data binding.
8. If EEZ Studio is used: **no EEZ Flow** and **do not edit `ui_generated` by hand**. The generated code is to be reproducible, isolated from the HID logic, and the editor project is in the repo.
9. Do not install the LVGL Pro runtime XML, do not assume a commercial entitlement to the Pro Editor; do not use Pro code in the repo without a separate ADR and license acceptance.
10. Do not change `esp_lvgl_port` to `esp_lvgl_adapter` "on the fly". The choice belongs exclusively to TASK-060A after build/HIL and the ADR-013 decision.
11. Every UI goes through the desktop SDL simulator with real LVGL, and then HIL. The HTML reference does not prove MCU operation.
12. Do not describe hardware FPS and latency results that were not measured. `NOT_RUN_HW` is mandatory when the board is unavailable.

## Ownership and files

| Task | Allowed paths | Excluded |
|---|---|---|
| TASK-060A | `components/ui_platform/**`, dependency manifests with the integrator's consent | UI views, HID core |
| TASK-060B | `components/ui_core/**`, `test/ui_store/**` | USB, CH422G |
| TASK-060C | `components/ui_core/ui_router.*`, `ui_overlay.*`, `test/ui_nav/**` | network, touch sampler |
| TASK-060D | `components/ui_theme/**`, `ui_components/**` | HID, pairing logic |
| TASK-060E | `components/ui_touch_bridge/**`, `test/ui_touch/**` | GT911 driver (without a separate task) |
| TASK-061 | `design/**`, `ui_generated/**`, `docs/ADR/**` | `ui_core/**`, HID |
| TASK-070A | `ui_views/ui_view_mouse.*`, tests | `usb_hid_device/**` |
| TASK-070B | `ui_views/ui_view_keyboard.*`, keymap UI / tests | direct TinyUSB |
| TASK-070C | `ui_views/ui_view_shortcuts.*`, bindings / tests | auth policy |
| TASK-070D | `simulator/**`, golden, test scripts | no unapproved refactors |

## Mandatory artifacts in a PR

- Task ID + the exact file paths and API.
- Evidence of `build` / `unit` / `SDL screenshot`; `HIL` separately, if available.
- A test verifying the lifecycle of observers and the absence of dangling pointers.
- A test of state snapshot delivery with a late / dropped event.
- A navigation test with an active `KEY_DOWN` or drag.
- A test of 2 fingers on the PAD + a navigation touch; no phantom click.
- A measurement of object count and live heap after 10000 screen changes (if the required stage has been reached).
- A list of the `AT-UIFW-01..10` tests, split into PASS / FAIL / NOT_RUN_HW.

## Prompt for the implementer (filled in by the coordinator)

```text
Task: TASK-060X [one task].
Before implementing read: AGENTS.md, AGENTS-UI.md,
07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md, the indicated fragments of 02/03/04.
You may edit only: [EXACT_PATHS].
Known interfaces/state: [CONTRACTS + PINNED HEADERS].
You must not: change the renderer, the touch, USB, HID ownership, generated code
beyond the exact scope. Always write tests for [AT-UIFW-ID].
If an API name does not exist in the headers, do NOT guess; report a blocker.
Report the exact commands and the real results. Do not claim that HIL is PASS
without access to the Waveshare ESP32-S3-Touch-LCD-7.
```
