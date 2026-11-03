// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiToast (TASK-060D, 07 §7). The widget's user data holds the timeout timer; the widget's delete
// event removes it. The timeout callback clears the pointer before it deletes the widget, because
// LVGL auto-deletes a repeat-count-1 timer itself once the callback returns - that is the only
// ordering in which neither side can free the other twice.
#include "ui_toast.h"

#include "ui_theme.h"

lv_timer_t *ui_toast_timer(lv_obj_t *toast)
{
    return toast ? (lv_timer_t *)lv_obj_get_user_data(toast) : NULL;
}

static void toast_timeout_cb(lv_timer_t *timer)
{
    lv_obj_t *toast = lv_timer_get_user_data(timer);
    if (!toast) {
        return;
    }
    lv_obj_set_user_data(toast, NULL); // LVGL deletes this timer right after the callback returns
    lv_obj_delete(toast);
}

static void toast_delete_cb(lv_event_t *e)
{
    lv_obj_t *toast = lv_event_get_current_target_obj(e);
    lv_timer_t *timer = ui_toast_timer(toast);
    if (timer) {
        lv_obj_set_user_data(toast, NULL);
        lv_timer_delete(timer);
    }
}

void ui_toast_dismiss(lv_obj_t *toast)
{
    if (toast) {
        lv_obj_delete(toast); // toast_delete_cb removes the timer
    }
}

lv_obj_t *ui_toast_show(lv_obj_t *parent, const char *text, uint32_t duration_ms)
{
    if (!parent || !text || !ui_theme_is_ready()) {
        return NULL;
    }
    lv_obj_t *toast = lv_obj_create(parent);
    lv_obj_remove_style_all(toast);
    ui_theme_apply(toast, UI_STYLE_TOAST, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(toast, false);
    lv_obj_set_clickable(toast, false); // never steals a press from the PAD underneath (07 §7)
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -UI_TOAST_BOTTOM);

    lv_obj_t *label = lv_label_create(toast);
    ui_theme_apply(label, UI_STYLE_TEXT_INVERSE, LV_STATE_DEFAULT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, text);
    lv_obj_set_style_max_width(label, UI_TOAST_MAX_W - 2 * UI_TOAST_PAD_HOR, LV_PART_MAIN);

    lv_obj_add_event_cb(toast, toast_delete_cb, LV_EVENT_DELETE, NULL);
    if (duration_ms != UI_TOAST_NO_TIMEOUT) {
        const uint32_t ms = (duration_ms > 0) ? duration_ms : (uint32_t)UI_TOAST_DEFAULT_MS;
        lv_timer_t *timer = lv_timer_create(toast_timeout_cb, ms, toast);
        if (timer) {
            lv_timer_set_repeat_count(timer, 1);
            lv_obj_set_user_data(toast, timer);
        }
    }
    return toast;
}
