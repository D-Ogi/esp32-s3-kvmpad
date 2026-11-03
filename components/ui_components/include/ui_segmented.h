// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiSegmented (TASK-060D) - the sixth shared component. 07 §7 gives `UiTopBar` the responsibility
// "navigation + two independent HOST/CLIENT statuses"; this is the navigation half, split out
// because every screen needs it and because FR-003 (the topbar permanently presents
// Mouse / Keyboard / Shortcuts) and FR-007 (switching the screen with a single tap, no gesture)
// make it the one control that must behave identically in all three views.
//
// It reports a tap and renders the selection; it does not navigate. The caller turns the control id
// into UI_ACT_NAVIGATE and lets the router perform the atomic mode switch of 07 §5 (cancel the
// pending touch, order the HID release, only then show the new screen). The component must never
// move the selection on its own from a tap: call ui_segmented_set_active() when the router has
// actually switched, or bind the active-view subject.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *label;
    const char *icon;    // LV_SYMBOL_* or NULL
    uint16_t control_id; // 07 §3: a UI-local id, never a keycode
} ui_segment_t;

typedef void (*ui_segmented_fn)(uint16_t control_id, uint16_t index, void *user_data);

/** Caller-owned sink; it must outlive the widget. */
typedef struct {
    ui_segmented_fn fn;
    void *user_data;
} ui_segmented_sink_t;

/** Caller-owned descriptor; it must outlive the widget (the factory keeps the pointer). */
typedef struct {
    const ui_segment_t *segments;
    uint16_t count;
    int32_t width;  // 0 => LV_PCT(100)
    int32_t height; // 0 => UI_SEGMENTED_H
} ui_segmented_desc_t;

/** Returns NULL if `parent`, `desc`, `count` or the theme is unavailable. `sink` may be NULL. */
lv_obj_t *ui_segmented_create(lv_obj_t *parent, const ui_segmented_desc_t *desc,
                              ui_segmented_sink_t *sink);

/** Select exactly one tab. An out-of-range index clears the selection (unknown active view). */
void ui_segmented_set_active(lv_obj_t *seg, int32_t index);

/** The selected index, or -1 when none is selected. */
int32_t ui_segmented_get_active(lv_obj_t *seg);

/**
 * Follow an int subject (e.g. the store's active-view projection) through the object, so the
 * observer dies with the widget (07 §4, AT-UIFW-09). A negative or out-of-range value clears the
 * selection instead of guessing a view.
 */
void ui_segmented_bind_active(lv_obj_t *seg, lv_subject_t *subject);

/** The tab widget at `index`, for hit-area tests and for a view that needs to disable one. */
lv_obj_t *ui_segmented_tab(lv_obj_t *seg, uint16_t index);

#ifdef __cplusplus
}
#endif
