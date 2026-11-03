// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiStatusItem (TASK-060D, 07 §7): "text, color, state and details". A value -> (text, severity)
// descriptor table turns one integer of a domain projection into a readable status. Any value the
// table does not list - including UI_STORE_UNKNOWN (-1) - reads the unknown text with
// UI_SEVERITY_UNKNOWN: the component never upgrades a status it was not told (07 §12 item 2,
// FR-401/403). FR-005: the severity is carried by an icon glyph and by the text, not by color alone.
//
// Not allowed here (07 §7): computing the real connection. The component renders what it is given.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One row of the value -> presentation table. `text` must outlive the widget (static storage). */
typedef struct {
    int32_t value;
    const char *text;
    ui_severity_t severity;
} ui_status_row_t;

/**
 * Descriptor of a status item. The caller owns it and it must outlive every widget created from
 * it: the factory stores the pointer, it does not copy the table.
 */
typedef struct {
    const char *title;          // static caption, e.g. "Host"; NULL hides the caption
    const ui_status_row_t *rows;
    uint16_t row_count;
    const char *unknown_text;   // NULL => "Unknown"
    int32_t width;              // 0 => size to content; > 0 => fixed width, value text clipped
    uint16_t control_id;        // FR-003: tapping an indicator opens the details
    bool clickable;             // false => inert indicator (no hit area)
} ui_status_desc_t;

/**
 * Create the item under `parent`. When `subject` is not NULL the item binds to it through the
 * object, so the observer dies with the widget (07 §4, AT-UIFW-09) and the subject may also be
 * deleted first. When it is NULL, drive the item with ui_status_item_set_value().
 * Returns NULL if `parent`, `desc` or the theme is unavailable.
 */
lv_obj_t *ui_status_item_create(lv_obj_t *parent, const ui_status_desc_t *desc, lv_subject_t *subject);

/** Apply a raw domain value. Safe to call before the first render and with an unlisted value. */
void ui_status_item_set_value(lv_obj_t *item, int32_t value);

/**
 * Re-fix the slot width. `width > 0` pins the item and lets the value label clip with dots inside
 * it; `width <= 0` returns the item to content sizing. UiTopBar uses this to pin every slot to
 * UI_TOPBAR_STATUS_W whatever the descriptor asked for (07 §7: the bar must not grow with the text).
 */
void ui_status_item_set_width(lv_obj_t *item, int32_t width);

/** The severity `desc` assigns to `value`; UI_SEVERITY_UNKNOWN when the table does not list it. */
ui_severity_t ui_status_item_severity_of(const ui_status_desc_t *desc, int32_t value);

/** The text `desc` assigns to `value`; the unknown text when the table does not list it. */
const char *ui_status_item_text_of(const ui_status_desc_t *desc, int32_t value);

/** The value label, for tests and for a view that needs to measure it. NULL if `item` is not one. */
lv_obj_t *ui_status_item_value_label(lv_obj_t *item);

#ifdef __cplusplus
}
#endif
