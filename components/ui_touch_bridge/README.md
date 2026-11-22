# `ui_touch_bridge`

**Role:** router of a single GT911 sample stream: per-contact capture → the LVGL pointer indev **and** a filtered stream
to the gesture FSM (`gesture_engine`), without contention.

**Tests.** `test/ui_touch/` (host); AT-UIFW-04 (pad + tab bar without a phantom click)

## Structure

| File | Role |
|---|---|
| `include/touch_router.h`, `src/touch_router.c` | Pure C router (host-tested). Target chosen at touch-down and captured per `track_id`: `CHROME` (drives the LVGL pointer), `PAD` (goes to the gesture FSM), `BLOCKED` (ignored until lifted) |
| `include/ui_touch_bridge.h`, `src/ui_touch_bridge.c` | Firmware task (priority 6, core 1): owns the router and the gesture FSM, one queue for samples and commands, 10 ms gesture tick, the LVGL indev read callback copies the pointer state under a spinlock |

With the HOST touchpad disabled, a local chart can claim a two-finger pinch. The reader preserves the first pair, its latest position and finger lifts until the LVGL task can process them. A captured pair cancels the single pointer press. The remaining finger is released before a new drag starts. A third contact, a sensor fault or a gap ends the gesture.

Conflict rules (one owner per contact, decided at touch-down):

- a pad contact while a chrome contact is pressed → `BLOCKED` (no pad gesture under a held button);
- a chrome contact while a pad contact is held → `BLOCKED` (a rail tap during a drag never presses a button);
- a second chrome contact → `BLOCKED` (LVGL has one pointer);
- After an invalid sample or `ui_touch_bridge_cancel()`, the LVGL input callback resets the press before
  reporting a release. Held track IDs stay blocked until a valid scan shows they have lifted. If a new
  press has already arrived, the callback requests another read in the same timer tick. The gesture FSM
  releases its held buttons on cancellation.

## API

```c
esp_err_t ui_touch_bridge_start(const ui_touch_bridge_cfg_t *cfg); // after ui_platform_start()
void ui_touch_bridge_post(const kvm_touch_sample_t *s);            // from the single GT911 consumer
void ui_touch_bridge_set_pad(const touch_rect_t *pad, bool enabled); // current screen's pad, NULL = none
void ui_touch_bridge_cancel(void);                                 // screen change, overlay, mode switch
void ui_touch_bridge_set_hid_enabled(bool on);                     // pad -> HID intents (off = FSM idle)
void ui_touch_bridge_set_drag_lock(bool on);
void ui_touch_bridge_get_stats(ui_touch_bridge_stats_t *out);
```

HID leaves the bridge only through the injected sink (`main/ui_glue.c` → `hid_dispatch_local()` → InputArbiter →
HidScheduler). The component has no USB include. The product shell supplies the active pad rectangle and enables the HID sink when it owns the screen. A contact that starts on the pad stays captured there until lift or cancellation.

## Firmware wiring (`main/`)

- `kvm_diag.c` (the single GT911 consumer) hands every sample to `ui_touch_bridge_post()` through `diag_set_touch_hook()`.
- `ui_glue.c` starts the bridge after the UI store, publishes `drag_lock_active` (gesture slice) every 20 ms and refreshes
  the pad rectangle from the current screen (`ui_glue_refresh_pad()`).
- Console: `ui touch` (counters + pad rectangle), `ui tap x y [ms]` (synthetic one-finger tap through the same path;
  refused while pad → HID is on), `ui pad on|off`, `ui drag on|off`. Every scene change calls `ui_touch_bridge_cancel()`
  first.
