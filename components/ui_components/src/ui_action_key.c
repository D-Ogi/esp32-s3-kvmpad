// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiActionKey (TASK-060D, 07 §7). The descriptor lives in the widget's user data, the sink in the
// event user data: both are caller-owned, so a key costs no allocation beyond its LVGL objects.
#include "ui_action_key.h"

#include "ui_theme.h"

static const ui_action_key_desc_t *desc_of(lv_obj_t *key)
{
    return key ? (const ui_action_key_desc_t *)lv_obj_get_user_data(key) : NULL;
}

bool ui_action_key_is_down(const lv_obj_t *key)
{
    return key && lv_obj_has_state(key, LV_STATE_USER_1);
}

uint16_t ui_action_key_control_id(lv_obj_t *key)
{
    const ui_action_key_desc_t *d = desc_of(key);
    return d ? d->control_id : 0u;
}

/** The single place that moves the down flag, so every edge is paired exactly once. */
static void emit(lv_obj_t *key, ui_action_key_sink_t *sink, bool down)
{
    if (down == ui_action_key_is_down(key)) {
        return;
    }
    lv_obj_set_state_user_1(key, down);
    const ui_action_key_desc_t *d = desc_of(key);
    if (sink && sink->fn && d) {
        sink->fn(d->control_id, down, sink->user_data);
    }
}

static void key_event_cb(lv_event_t *e)
{
    lv_obj_t *key = lv_event_get_current_target_obj(e);
    ui_action_key_sink_t *sink = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        if (!lv_obj_has_state(key, LV_STATE_DISABLED)) {
            emit(key, sink, true);
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_DELETE:
        // Prohibition 3: a lost press, a teardown or a view change must not leave a key DOWN.
        emit(key, sink, false);
        break;
    default:
        break;
    }
}

void ui_action_key_set_enabled(lv_obj_t *key, bool enabled)
{
    if (!key) {
        return;
    }
    if (!enabled && ui_action_key_is_down(key)) {
        lv_obj_send_event(key, LV_EVENT_PRESS_LOST, NULL); // release before the key goes inert
    }
    lv_obj_set_disabled(key, !enabled);
    lv_obj_set_clickable(key, enabled);
}

void ui_action_key_set_label(lv_obj_t *key, const char *text)
{
    if (key == NULL || text == NULL) {
        return;
    }
    const ui_action_key_desc_t *desc = lv_obj_get_user_data(key);
    if (desc == NULL || desc->label == NULL) {
        return; // an icon-only key has no label to change
    }
    lv_obj_t *label = lv_obj_get_child(key, -1); // created last, after the optional icon
    if (label != NULL) {
        lv_label_set_text(label, text);
    }
}

void ui_action_key_set_checked(lv_obj_t *key, bool checked)
{
    if (key) {
        lv_obj_set_checked(key, checked);
    }
}

lv_obj_t *ui_action_key_create(lv_obj_t *parent, const ui_action_key_desc_t *desc,
                               ui_action_key_sink_t *sink)
{
    if (!parent || !desc || !ui_theme_is_ready()) {
        return NULL;
    }
    lv_obj_t *key = lv_button_create(parent);
    lv_obj_remove_style_all(key);
    ui_theme_apply_key(key, desc->variant);
    lv_obj_set_style_text_font(key, ui_theme_font(desc->font), LV_PART_MAIN);
    const int32_t h = (desc->height > 0) ? desc->height : UI_TOUCH_PREFERRED;
    lv_obj_set_height(key, (h < UI_TOUCH_MIN) ? UI_TOUCH_MIN : h);
    if (desc->width > 0) {
        lv_obj_set_width(key, (desc->width < UI_TOUCH_MIN) ? UI_TOUCH_MIN : desc->width);
    } else {
        lv_obj_set_width(key, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(key, 1);
    }
    lv_obj_set_user_data(key, (void *)desc);
    lv_obj_set_checkable(key, desc->checkable);
    lv_obj_set_press_lock(key, false); // let LVGL report PRESS_LOST when the finger slides off
    lv_obj_set_flex_flow(key, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(key, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (desc->icon) {
        lv_obj_t *icon = lv_label_create(key);
        lv_label_set_text(icon, desc->icon);
    }
    if (desc->label) {
        lv_obj_t *label = lv_label_create(key);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(label, desc->label);
        lv_obj_set_style_max_width(label, LV_PCT(100), LV_PART_MAIN);
    }
    // Only the four codes that move the down flag, so the hot draw path never calls the handler.
    lv_obj_add_event_cb(key, key_event_cb, LV_EVENT_PRESSED, sink);
    lv_obj_add_event_cb(key, key_event_cb, LV_EVENT_RELEASED, sink);
    lv_obj_add_event_cb(key, key_event_cb, LV_EVENT_PRESS_LOST, sink);
    lv_obj_add_event_cb(key, key_event_cb, LV_EVENT_DELETE, sink);
    return key;
}
