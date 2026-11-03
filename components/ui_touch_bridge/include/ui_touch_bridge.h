// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UI touch bridge (TASK-060E, 07 §6): the single consumer of touch_input samples hands each sample
// to this bridge; one task routes it (touch_router) to a custom LVGL pointer indev and to the gesture
// FSM. Gesture output (typed HID intents) leaves only through the sink installed by the integrator
// (firmware: hid_dispatch, i.e. InputArbiter -> HidScheduler). No GT911 access, no USB stack here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gesture_engine.h"
#include "kvm_touch.h"
#include "touch_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Gesture intent sink; returns true when the intent was queued. Called from the bridge task. */
typedef bool (*ui_touch_hid_sink_fn)(void *ctx, const kvm_hid_intent_t *intent);

typedef struct {
    uint16_t h_res, v_res;
    ui_touch_hid_sink_fn sink;
    void *sink_ctx;
    bool hid_enabled; // pad -> HID at start; off means the pad is routed but the gesture FSM is idle
} ui_touch_bridge_cfg_t;

typedef struct {
    touch_router_stats_t router;
    kvm_gesture_stats_t gesture;
    uint32_t posted;
    uint32_t queue_full;       // samples dropped because the bridge lagged (the FSM sees a gap)
    uint32_t intents;          // handed to the sink
    uint32_t intents_refused;  // sink refused (queue full): hid_dispatch then releases everything
    uint32_t indev_reads;
    uint32_t indev_resets;     // LVGL presses cancelled without a click
    bool hid_enabled;
    bool drag_lock;
    bool left_held;
    kvm_gesture_state_t gesture_state;
    touch_rect_t pad;          // the pad rectangle the router uses now
    bool pad_enabled;
} ui_touch_bridge_stats_t;

/** After ui_platform_start(): creates the LVGL pointer indev (under the UI lock) and the bridge task. */
esp_err_t ui_touch_bridge_start(const ui_touch_bridge_cfg_t *cfg);
bool ui_touch_bridge_running(void);

/** From the single touch_input consumer: never blocks; a full queue drops the sample and counts it. */
void ui_touch_bridge_post(const kvm_touch_sample_t *s);

/** Pad rectangle and whether it accepts new contacts (MOUSE view, no blocking overlay). */
void ui_touch_bridge_set_pad(const touch_rect_t *pad, bool enabled);

/** View change / modal / owner loss (FR-110): cancel the LVGL press and the gesture, release held buttons. */
void ui_touch_bridge_cancel(void);

/** Pad -> HID on/off. Turning it off cancels the gesture first, so every held button is released. */
void ui_touch_bridge_set_hid_enabled(bool on);

/** Drag lock (FR-107/115), applied in the bridge task; turning it off releases LEFT if held. */
void ui_touch_bridge_set_drag_lock(bool on);

void ui_touch_bridge_get_stats(ui_touch_bridge_stats_t *out);

#ifdef __cplusplus
}
#endif
