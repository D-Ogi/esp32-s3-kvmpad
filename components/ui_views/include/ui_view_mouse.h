// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// MOUSE view (TASK-070A; 01 §2.1, FR-107/108/110/115, reference mouse-view): the touch surface plus a footer with
// dedicated Left-click and Right-click buttons and the Drag lock toggle. The touch surface itself is not an LVGL
// input target: ui_touch_bridge routes touches inside the pad rectangle to the gesture engine (TASK-060E, one GT911
// reader).
//
// No HID here (AGENTS-UI §4): the buttons are ui_action_key widgets (TASK-060D) and submit typed actions through
// ui_actions. A button press maps to MOUSE_BUTTON_DOWN and its release, a lost press or the button's deletion to
// MOUSE_BUTTON_UP, so a tap is one click and a hold is a held button. Drag lock submits DRAG_LOCK_REQUEST on a click;
// the key's checked state, its "Release" label (FR-107), the live label and the hint follow the authoritative
// UI_SUBJ_DRAG_LOCK subject, never the local click (AGENTS-UI §5). While the HOST is not mounted every button is
// disabled (a held one is released first) and the surface shows "Host disconnected".
// The "?" key in the footer opens UI_OVERLAY_GESTURE_HELP, a blocking router overlay that lists the touchpad gestures.
// Being blocking, it takes the pad rectangle away from the gesture engine while it is open (ui_router_pad_area), so a
// tap on the card is never a click on the HOST. It is opened and closed through the bound router; it sends nothing.
// LVGL owner context only; ui_theme_init() must have run.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_actions.h"
#include "ui_action_key.h"
#include "ui_router.h"
#include "ui_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CTRL_MOUSE_LEFT 0x0100u
#define UI_CTRL_MOUSE_RIGHT 0x0101u
#define UI_CTRL_MOUSE_DRAG_LOCK 0x0102u
#define UI_CTRL_MOUSE_HELP 0x0103u
#define UI_CTRL_MOUSE_HELP_CLOSE 0x0104u

#define UI_OVERLAY_GESTURE_HELP 0x0501u
#define UI_MOUSE_OVERLAYS 1
#define UI_VIEW_MOUSE_HELP_ROWS 8

typedef struct {
    uint32_t button_down;    // MOUSE_BUTTON_DOWN accepted by the sink (left + right)
    uint32_t button_up;      // MOUSE_BUTTON_UP submitted for an accepted DOWN
    uint32_t refused;        // a submit the sink refused
    uint32_t drag_requests;  // DRAG_LOCK_REQUEST submitted
    uint32_t forced_releases;// a held button released by leave / HOST loss / deletion
    uint32_t help_opens;     // the gesture card was built
    uint32_t help_closes;    // ... and taken down (Close, router dismiss or teardown)
    uint32_t help_refused;   // "?" tapped without a router, or the router refused the overlay
} ui_view_mouse_counters_t;

typedef struct {
    // bound at init
    ui_store_t *store;
    ui_actions_t *actions;
    uint64_t (*now_us)(void);
    ui_view_descriptor_t desc; // id UI_VIEW_MOUSE; hand a copy to ui_router_init()
    ui_action_key_sink_t sink; // the buttons' edge sink (points back at this struct)
    ui_overlay_descriptor_t ov[UI_MOUSE_OVERLAYS]; // UI_OVERLAY_GESTURE_HELP; hand copies to ui_router_init()
    ui_router_t *router;       // bound by ui_view_mouse_bind_router(); NULL: "?" is refused

    // created by desc.create (NULL before / after)
    lv_obj_t *root;
    lv_obj_t *pad;
    lv_obj_t *live;
    lv_obj_t *offline;    // "Host disconnected" notice, shown instead of the invitation below
    lv_obj_t *idle_title; // "Just move." - hidden while the notice is up
    lv_obj_t *idle_sub;
    lv_obj_t *hint;
    lv_obj_t *btn_left;
    lv_obj_t *btn_right;
    lv_obj_t *btn_drag;
    lv_obj_t *drag_label;
    lv_obj_t *btn_help;

    // created by the overlay (NULL while it is closed)
    lv_obj_t *help_card;

    uint8_t held_mask; // bit (button id - 1): a DOWN was accepted and its UP is still owed
    bool leaving;      // inside on_leave / destroy: releases count as forced
    ui_view_mouse_counters_t c;
} ui_view_mouse_t;

/** Bind the view to the store and the action gateway and fill m->desc. Builds no object. */
void ui_view_mouse_init(ui_view_mouse_t *m, ui_store_t *store, ui_actions_t *actions, uint64_t (*now_us)(void));

/** The router that shows and dismisses the gesture card. Call after ui_view_mouse_init(), before the card is used. */
void ui_view_mouse_bind_router(ui_view_mouse_t *m, ui_router_t *router);

/** The view's overlay descriptors (UI_MOUSE_OVERLAYS of them) for ui_router_init(). */
const ui_overlay_descriptor_t *ui_view_mouse_overlays(ui_view_mouse_t *m, uint8_t *n);

/** Gesture card content, row by row (for tests and the simulator); NULL past the last row. */
const char *ui_view_mouse_help_gesture(uint8_t row);
const char *ui_view_mouse_help_result(uint8_t row);

#ifdef __cplusplus
}
#endif
