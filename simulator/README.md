# `simulator/`

Desktop LVGL 9.6 (SDL) — level L1 of the UI tests (07 §10). TASK-070D.

| Directory | Contents |
|---|---|
| `lvgl_sdl/` | real LVGL 800×480 + fake USB/NET state publishers |
| `fixtures/` | deterministic states + touch playback |
| `golden/` | dumps from native LVGL (not from an HTML render) |

The simulator does not prove operation on the MCU — HIL separately.
