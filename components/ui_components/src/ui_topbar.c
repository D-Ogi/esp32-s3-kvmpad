// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiTopBar (TASK-060D, 07 §7). Every slot exists at a fixed child index whether it is used or not;
// an unused slot is hidden, and the LVGL flex layout skips hidden children. That keeps the
// accessors index-based without a per-bar allocation, and keeps the width budget of ui_tokens.h
// (inner 992 px = 960 px of fixed slots + 24 px of gaps + an 8 px spacer) verifiable in a test.
#include "ui_topbar.h"

#include "ui_theme.h"

enum {
    CHILD_BRAND = 0,
    CHILD_NAV,
    CHILD_SPACER,
    CHILD_KEEP_AWAKE,
    CHILD_STATUS_FIRST, // + ui_topbar_status_t
    CHILD_TRAILING = CHILD_STATUS_FIRST + UI_TOPBAR_STATUS_COUNT,
    CHILD_COUNT,
};

static lv_obj_t *plain(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_size(obj, w, h);
    return obj;
}

static lv_obj_t *child(lv_obj_t *bar, int index)
{
    if (!bar || lv_obj_get_child_count(bar) < (uint32_t)CHILD_COUNT) {
        return NULL;
    }
    return lv_obj_get_child(bar, index);
}

lv_obj_t *ui_topbar_nav_slot(lv_obj_t *bar)
{
    return child(bar, CHILD_NAV);
}

lv_obj_t *ui_topbar_trailing_slot(lv_obj_t *bar)
{
    return child(bar, CHILD_TRAILING);
}

lv_obj_t *ui_topbar_status_item(lv_obj_t *bar, ui_topbar_status_t slot)
{
    if ((int)slot < 0 || (int)slot >= (int)UI_TOPBAR_STATUS_COUNT) {
        return NULL;
    }
    lv_obj_t *obj = child(bar, CHILD_STATUS_FIRST + (int)slot);
    return (obj && lv_obj_get_user_data(obj)) ? obj : NULL;
}

lv_obj_t *ui_topbar_keep_awake(lv_obj_t *bar)
{
    lv_obj_t *obj = child(bar, CHILD_KEEP_AWAKE);
    return (obj && lv_obj_get_user_data(obj)) ? obj : NULL;
}

/**
 * FR-712: the indicator shows only while the value is exactly 1. An unknown (-1) or any other
 * value hides it, so the bar never claims the feature is on. Bound through the object, so the
 * observer dies with the chip (07 §4).
 */
static void keep_awake_observer_cb(lv_observer_t *observer, lv_subject_t *subject)
{
    lv_obj_set_hidden(lv_observer_get_target_obj(observer), lv_subject_get_int(subject) != 1);
}

/** FR-712: icon + text, hidden unless the subject reads exactly 1 (an unknown value hides it). */
static void add_keep_awake(lv_obj_t *bar, const ui_topbar_desc_t *desc, lv_subject_t *subject)
{
    lv_obj_t *chip = plain(bar, UI_TOPBAR_KEEP_AWAKE_W, UI_STATUS_ITEM_H);
    if (!desc->keep_awake_text) {
        lv_obj_set_hidden(chip, true);
        return;
    }
    lv_obj_set_user_data(chip, (void *)desc);
    lv_obj_set_style_pad_gap(chip, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *icon = lv_label_create(chip);
    ui_theme_apply(icon, UI_STYLE_TEXT_LABEL, LV_STATE_DEFAULT);
    lv_label_set_text(icon, desc->keep_awake_icon ? desc->keep_awake_icon : LV_SYMBOL_EYE_OPEN);
    lv_obj_t *label = lv_label_create(chip);
    ui_theme_apply(label, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, desc->keep_awake_text);
    lv_obj_set_flex_grow(label, 1);
    // One line of its own font: DOTS only clips when the label has a fixed height.
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    if (subject) {
        lv_subject_add_observer_obj(subject, keep_awake_observer_cb, chip, NULL);
    } else {
        lv_obj_set_hidden(chip, true);
    }
}

lv_obj_t *ui_topbar_create(lv_obj_t *parent, const ui_topbar_desc_t *desc, const ui_topbar_bind_t *bind)
{
    if (!parent || !desc || !ui_theme_is_ready()) {
        return NULL;
    }
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_clickable(bar, false);
    ui_theme_apply(bar, UI_STYLE_TOPBAR, LV_STATE_DEFAULT);
    lv_obj_set_size(bar, LV_PCT(100), UI_TOPBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *brand = plain(bar, UI_TOPBAR_BRAND_W, LV_SIZE_CONTENT);
    if (desc->brand) {
        lv_obj_t *label = lv_label_create(brand);
        ui_theme_apply(label, UI_STYLE_TEXT_HEADING, LV_STATE_DEFAULT);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(label, desc->brand);
        lv_obj_set_size(label, LV_PCT(100),
                        lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    } else {
        lv_obj_set_hidden(brand, true);
    }
    plain(bar, UI_TOPBAR_NAV_W, UI_SEGMENTED_H); // CHILD_NAV: filled by the view with ui_segmented
    lv_obj_t *spacer = plain(bar, 0, 1);         // CHILD_SPACER: absorbs the remaining width
    lv_obj_set_flex_grow(spacer, 1);
    add_keep_awake(bar, desc, bind ? bind->keep_awake : NULL);

    for (int i = 0; i < UI_TOPBAR_STATUS_COUNT; i++) {
        const ui_status_desc_t *sd = desc->status[i];
        if (sd) {
            // The slot width is the bar's, not the descriptor's: a long status must clip, not grow.
            ui_status_item_set_width(ui_status_item_create(bar, sd, bind ? bind->status[i] : NULL),
                                     UI_TOPBAR_STATUS_W);
        } else {
            lv_obj_set_hidden(plain(bar, UI_TOPBAR_STATUS_W, UI_STATUS_ITEM_H), true);
        }
    }
    plain(bar, UI_TOPBAR_BTN_W, UI_TOPBAR_BTN_W); // CHILD_TRAILING: settings button slot
    return bar;
}
