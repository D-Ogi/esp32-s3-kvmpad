// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Touch router (TASK-060E, 07 §6): one GT911 sample from touch_input is split between the LVGL
// pointer (CHROME: bars, rail, buttons, modals) and the pure gesture FSM (PAD: the touchpad rectangle,
// MOUSE view only). Each contact is classified once, at TOUCH_DOWN, and keeps its target until it
// lifts (per-contact capture by track_id), even when the finger crosses the pad edge.
//
// Conflict rules (07 §6 "two fingers covering the PAD and a button"):
// - a contact that starts on the pad while a CHROME press is held is BLOCKED (no HID);
// - a contact that starts outside the pad while a PAD gesture is held is BLOCKED (no phantom click);
// - LVGL has one pointer: a second CHROME contact is BLOCKED until it lifts;
// - an invalid sample or touch_router_cancel() (view change, modal, owner loss) cancels the LVGL press
//   without a click and blocks every current contact until it lifts.
// Pure C, no allocation, single context (the bridge task).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kvm_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_TARGET_NONE = 0,
    TOUCH_TARGET_CHROME,
    TOUCH_TARGET_PAD,
    TOUCH_TARGET_BLOCKED,
} touch_target_t;

typedef struct {
    int16_t x1, y1, x2, y2; // inclusive
} touch_rect_t;

typedef struct {
    uint32_t samples;
    uint32_t invalid;
    uint32_t chrome_downs;
    uint32_t pad_downs;
    uint32_t blocked_downs;
    uint32_t pointer_cancels; // LVGL presses ended without a click
    uint32_t cancels;         // touch_router_cancel() calls
} touch_router_stats_t;

typedef struct {
    bool used;
    uint8_t id;
    touch_target_t target;
} touch_contact_t;

typedef struct {
    touch_rect_t pad;
    bool pad_enabled; // MOUSE view and no blocking overlay
    touch_contact_t c[KVM_TOUCH_MAX_POINTS];
    bool ptr_active;  // a CHROME contact drives the LVGL pointer
    uint8_t ptr_id;
    int16_t ptr_x, ptr_y;
    bool cancel_pending;
    touch_router_stats_t st;
} touch_router_t;

typedef struct {
    // LVGL pointer
    bool ptr_pressed;
    int16_t ptr_x, ptr_y;    // last position (kept after release: LVGL wants the release point)
    bool ptr_cancel;         // reset the LVGL press now (lv_indev_reset): the release is not a click
    // gesture FSM input: the same timestamp and sequence, only the PAD contacts (an invalid input
    // sample stays invalid so the FSM cancels)
    kvm_touch_sample_t pad;
} touch_route_out_t;

void touch_router_init(touch_router_t *r);

/** The pad rectangle in screen pixels and whether it accepts new contacts (MOUSE view, no modal). */
void touch_router_set_pad(touch_router_t *r, const touch_rect_t *pad, bool enabled);

/** Route one sample. Call for every sample, empty ones included (they carry the release). */
void touch_router_route(touch_router_t *r, const kvm_touch_sample_t *s, touch_route_out_t *out);

/** View change / modal / owner loss: the next route output cancels the LVGL press; held contacts are blocked. */
void touch_router_cancel(touch_router_t *r);

/** Target of a held contact (TOUCH_TARGET_NONE when not held). */
touch_target_t touch_router_target(const touch_router_t *r, uint8_t track_id);

const char *touch_target_name(touch_target_t t);

#ifdef __cplusplus
}
#endif
