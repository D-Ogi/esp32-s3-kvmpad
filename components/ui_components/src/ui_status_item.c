// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiStatusItem (TASK-060D, 07 §7). Tree: item > { severity glyph, text column > { caption, value } }.
// Stacking the caption over the value is what lets a UI_TOPBAR_STATUS_W slot carry both "Host"
// and "Disconnected" at a legible size. The descriptor lives in the widget's user data; no per-widget
// allocation of our own.
//
// Single-line heights are explicit: LV_LABEL_LONG_MODE_DOTS only clips when the label has a fixed
// size. A label left at LV_SIZE_CONTENT wraps instead, which is exactly the growth 07 §7 forbids.
#include "ui_status_item.h"

#include "ui_theme.h"

#define CHILD_ICON   0
#define CHILD_COLUMN 1
#define COL_TITLE    0
#define COL_VALUE    1

static const ui_status_row_t *find_row(const ui_status_desc_t *d, int32_t value)
{
    if (!d || !d->rows) {
        return NULL;
    }
    for (uint16_t i = 0; i < d->row_count; i++) {
        if (d->rows[i].value == value) {
            return &d->rows[i];
        }
    }
    return NULL;
}

ui_severity_t ui_status_item_severity_of(const ui_status_desc_t *desc, int32_t value)
{
    const ui_status_row_t *row = find_row(desc, value);
    return row ? row->severity : UI_SEVERITY_UNKNOWN;
}

const char *ui_status_item_text_of(const ui_status_desc_t *desc, int32_t value)
{
    const ui_status_row_t *row = find_row(desc, value);
    if (row) {
        return row->text;
    }
    return (desc && desc->unknown_text) ? desc->unknown_text : "Unknown";
}

lv_obj_t *ui_status_item_value_label(lv_obj_t *item)
{
    if (!item || !lv_obj_get_user_data(item) || lv_obj_get_child_count(item) <= CHILD_COLUMN) {
        return NULL;
    }
    lv_obj_t *column = lv_obj_get_child(item, CHILD_COLUMN);
    return (lv_obj_get_child_count(column) > COL_VALUE) ? lv_obj_get_child(column, COL_VALUE) : NULL;
}

/** Pin a label to exactly one line of its own font, so DOTS clips instead of wrapping. */
static void one_line(lv_obj_t *label)
{
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

void ui_status_item_set_value(lv_obj_t *item, int32_t value)
{
    lv_obj_t *val = ui_status_item_value_label(item);
    if (!val) {
        return;
    }
    const ui_status_desc_t *d = lv_obj_get_user_data(item);
    const ui_severity_t sev = ui_status_item_severity_of(d, value);
    lv_obj_t *icon = lv_obj_get_child(item, CHILD_ICON);
    lv_label_set_text(icon, ui_theme_severity_icon(sev));
    lv_obj_set_style_text_color(icon, ui_theme_severity_dot(sev), LV_PART_MAIN);
    lv_label_set_text(val, ui_status_item_text_of(d, value));
    lv_obj_set_style_text_color(val, ui_theme_severity_color(sev), LV_PART_MAIN);
}

void ui_status_item_set_width(lv_obj_t *item, int32_t width)
{
    lv_obj_t *value = ui_status_item_value_label(item);
    if (!value) {
        return;
    }
    lv_obj_t *column = lv_obj_get_parent(value);
    if (width > 0) {
        lv_obj_set_width(item, width);
        lv_obj_set_flex_grow(column, 1); // the slot clips the text; the item never grows
        lv_obj_set_width(value, LV_PCT(100));
        lv_obj_set_width(lv_obj_get_child(column, COL_TITLE), LV_PCT(100));
    } else {
        lv_obj_set_width(item, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(column, 0);
        lv_obj_set_width(column, LV_SIZE_CONTENT);
        lv_obj_set_width(value, LV_SIZE_CONTENT);
        lv_obj_set_width(lv_obj_get_child(column, COL_TITLE), LV_SIZE_CONTENT);
    }
}

static void status_observer_cb(lv_observer_t *observer, lv_subject_t *subject)
{
    ui_status_item_set_value(lv_observer_get_target_obj(observer), lv_subject_get_int(subject));
}

static lv_obj_t *add_line(lv_obj_t *column, ui_style_id_t style, const char *text)
{
    lv_obj_t *label = lv_label_create(column);
    ui_theme_apply(label, style, LV_STATE_DEFAULT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, text);
    one_line(label);
    return label;
}

lv_obj_t *ui_status_item_create(lv_obj_t *parent, const ui_status_desc_t *desc, lv_subject_t *subject)
{
    if (!parent || !desc || !ui_theme_is_ready()) {
        return NULL;
    }
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_scrollable(item, false);
    lv_obj_set_clickable(item, desc->clickable);
    lv_obj_set_user_data(item, (void *)desc);
    lv_obj_set_size(item, LV_SIZE_CONTENT, UI_STATUS_ITEM_H);
    lv_obj_set_style_pad_gap(item, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(item, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_set_style_radius(item, UI_RADIUS_M, LV_PART_MAIN);
    ui_theme_apply(item, UI_STYLE_STATE_PRESSED, LV_STATE_PRESSED);
    ui_theme_apply(item, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(item);
    ui_theme_apply(icon, UI_STYLE_TEXT_LABEL, LV_STATE_DEFAULT);
    lv_label_set_text(icon, ui_theme_severity_icon(UI_SEVERITY_UNKNOWN));

    lv_obj_t *column = lv_obj_create(item);
    lv_obj_remove_style_all(column);
    lv_obj_set_scrollable(column, false);
    lv_obj_set_clickable(column, false);
    lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = add_line(column, UI_STYLE_TEXT_MUTED, desc->title ? desc->title : "");
    lv_obj_set_hidden(title, desc->title == NULL);
    add_line(column, UI_STYLE_TEXT_BODY, ui_status_item_text_of(desc, INT32_MIN));
    ui_status_item_set_width(item, desc->width);

    if (subject) {
        lv_subject_add_observer_obj(subject, status_observer_cb, item, NULL);
    } else {
        ui_status_item_set_value(item, INT32_MIN);
    }
    return item;
}
