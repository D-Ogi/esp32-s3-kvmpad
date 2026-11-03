// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiSegmented (TASK-060D). The track's user data is the descriptor; each tab's user data is the
// address of its row in the descriptor table, so the index is pointer arithmetic and no widget
// needs an allocation of its own.
#include "ui_segmented.h"

#include "ui_theme.h"

static const ui_segmented_desc_t *desc_of(lv_obj_t *seg)
{
    return seg ? (const ui_segmented_desc_t *)lv_obj_get_user_data(seg) : NULL;
}

lv_obj_t *ui_segmented_tab(lv_obj_t *seg, uint16_t index)
{
    const ui_segmented_desc_t *d = desc_of(seg);
    if (!d || index >= d->count || index >= lv_obj_get_child_count(seg)) {
        return NULL;
    }
    return lv_obj_get_child(seg, index);
}

void ui_segmented_set_active(lv_obj_t *seg, int32_t index)
{
    const ui_segmented_desc_t *d = desc_of(seg);
    if (!d) {
        return;
    }
    for (uint16_t i = 0; i < d->count; i++) {
        lv_obj_t *tab = ui_segmented_tab(seg, i);
        if (tab) {
            lv_obj_set_checked(tab, (int32_t)i == index);
        }
    }
}

int32_t ui_segmented_get_active(lv_obj_t *seg)
{
    const ui_segmented_desc_t *d = desc_of(seg);
    for (uint16_t i = 0; d && i < d->count; i++) {
        lv_obj_t *tab = ui_segmented_tab(seg, i);
        if (tab && lv_obj_has_state(tab, LV_STATE_CHECKED)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void segmented_observer_cb(lv_observer_t *observer, lv_subject_t *subject)
{
    ui_segmented_set_active(lv_observer_get_target_obj(observer), lv_subject_get_int(subject));
}

void ui_segmented_bind_active(lv_obj_t *seg, lv_subject_t *subject)
{
    if (seg && subject) {
        lv_subject_add_observer_obj(subject, segmented_observer_cb, seg, NULL);
    }
}

static void tab_clicked_cb(lv_event_t *e)
{
    lv_obj_t *tab = lv_event_get_current_target_obj(e);
    lv_obj_t *seg = lv_obj_get_parent(tab);
    const ui_segmented_desc_t *d = desc_of(seg);
    const ui_segment_t *row = lv_obj_get_user_data(tab);
    ui_segmented_sink_t *sink = lv_event_get_user_data(e);
    if (!d || !row || !sink || !sink->fn) {
        return; // the selection is not moved here: the router decides (07 §5)
    }
    sink->fn(row->control_id, (uint16_t)(row - d->segments), sink->user_data);
}

static void add_tab(lv_obj_t *seg, const ui_segment_t *row, int32_t width, ui_segmented_sink_t *sink)
{
    lv_obj_t *tab = lv_button_create(seg);
    lv_obj_remove_style_all(tab);
    ui_theme_apply(tab, UI_STYLE_SEG_TAB, LV_STATE_DEFAULT);
    ui_theme_apply(tab, UI_STYLE_SEG_TAB_CHECKED, LV_STATE_CHECKED);
    ui_theme_apply(tab, UI_STYLE_STATE_DISABLED, LV_STATE_DISABLED);
    ui_theme_apply(tab, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_user_data(tab, (void *)row);
    lv_obj_set_height(tab, LV_PCT(100));
    if (width > 0) {
        lv_obj_set_width(tab, width);
    } else {
        // Width 0 + grow, not LV_SIZE_CONTENT + grow: a content-sized tab whose label is wider than
        // its share pushes the neighboring tabs out of the track instead of clipping (07 §7).
        lv_obj_set_width(tab, 0);
        lv_obj_set_flex_grow(tab, 1);
    }
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (row->icon) {
        lv_label_set_text(lv_label_create(tab), row->icon);
    }
    if (row->label) {
        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(label, row->label);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_flex_grow(label, 1);
        // One line of its own font: DOTS only clips when the label has a fixed size.
        lv_obj_set_height(label,
                          lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    }
    lv_obj_add_event_cb(tab, tab_clicked_cb, LV_EVENT_CLICKED, sink);
}

lv_obj_t *ui_segmented_create(lv_obj_t *parent, const ui_segmented_desc_t *desc,
                              ui_segmented_sink_t *sink)
{
    if (!parent || !desc || !desc->segments || desc->count == 0 || !ui_theme_is_ready()) {
        return NULL;
    }
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_scrollable(seg, false);
    lv_obj_set_clickable(seg, false); // only the tabs are hit targets
    ui_theme_apply(seg, UI_STYLE_SEG_TRACK, LV_STATE_DEFAULT);
    lv_obj_set_user_data(seg, (void *)desc);
    lv_obj_set_size(seg, (desc->width > 0) ? desc->width : LV_PCT(100),
                    (desc->height > 0) ? desc->height : UI_SEGMENTED_H);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint16_t i = 0; i < desc->count; i++) {
        add_tab(seg, &desc->segments[i], 0, sink);
    }
    return seg;
}
