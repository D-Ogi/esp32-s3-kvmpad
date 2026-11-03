// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UI store (TASK-060B): mailbox snapshot -> LVGL 9.6 Subjects. See ui_store.h.
#include "ui_store.h"

#include <string.h>

static void delete_all(ui_store_t *s)
{
    for (int i = 0; i < UI_SUBJ_COUNT; i++) {
        lv_subject_delete(s->subj[i]); // NULL-safe; detaches the observers before freeing
        s->subj[i] = NULL;
    }
}

bool ui_store_init(ui_store_t *s, ui_mailbox_t *mb)
{
    memset(s, 0, sizeof(*s));
    s->mb = mb;
    for (int i = 0; i < UI_SUBJ_COUNT; i++) {
        s->subj[i] = lv_subject_create(LV_SUBJECT_TYPE_INT);
        if (!s->subj[i]) {
            delete_all(s);
            return false;
        }
        int32_t init = i < UI_SUBJ_DOMAIN_COUNT ? UI_STORE_UNKNOWN : 0;
        if (i == UI_SUBJ_ACTIVE_VIEW) {
            init = UI_VIEW_MOUSE;
        }
        lv_subject_set_int(s->subj[i], init); // no observers yet: nothing is notified
    }
    // version 0 = nothing published yet: the first take returns only once a source has published
    s->seen_version = 0;
    s->ready = true;
    return true;
}

void ui_store_deinit(ui_store_t *s)
{
    if (!s->ready) {
        return;
    }
    delete_all(s);
    s->ready = false;
}

void ui_store_set_event_handler(ui_store_t *s, ui_store_event_fn fn, void *ctx)
{
    s->on_event = fn;
    s->event_ctx = ctx;
}

static void set(ui_store_t *s, ui_subject_id_t id, bool known, int32_t v)
{
    lv_subject_set_int(s->subj[id], known ? v : UI_STORE_UNKNOWN);
}

void ui_store_apply(ui_store_t *s, const ui_state_snapshot_t *snap)
{
    const bool hid = (snap->sources & UI_SRC_HID) != 0;
    const bool net = (snap->sources & UI_SRC_NET) != 0;
    const bool touch = (snap->sources & UI_SRC_TOUCH) != 0;
    const bool gest = (snap->sources & UI_SRC_GESTURE) != 0;
    set(s, UI_SUBJ_HOST_USB, hid, snap->host_usb_state);
    set(s, UI_SUBJ_OWNER_KIND, hid, snap->owner_kind);
    set(s, UI_SUBJ_OWNER_EPOCH, hid, (int32_t)(snap->owner_epoch & 0x7FFFFFFFu));
    set(s, UI_SUBJ_MODIFIERS, hid, snap->modifiers_down);
    set(s, UI_SUBJ_MODS_ARMED, hid, snap->mods_armed);
    set(s, UI_SUBJ_MODS_LOCKED, hid, snap->mods_locked);
    set(s, UI_SUBJ_CAPS_LED, hid && snap->caps_led_known, snap->caps_led_active ? 1 : 0);
    set(s, UI_SUBJ_MAINTENANCE, hid, snap->maintenance ? 1 : 0);
    set(s, UI_SUBJ_ERROR, hid, snap->error_code);
    set(s, UI_SUBJ_CLIENT, net, snap->client_state);
    set(s, UI_SUBJ_WIFI, net, snap->wifi_connected ? 1 : 0);
    set(s, UI_SUBJ_TOUCH_OK, touch, snap->touch_healthy ? 1 : 0);
    set(s, UI_SUBJ_DRAG_LOCK, gest, snap->drag_lock_active ? 1 : 0);
    s->applied = *snap;
    s->c.applied++;
}

bool ui_store_tick(ui_store_t *s)
{
    if (!s->ready) {
        return false;
    }
    s->c.ticks++;
    bool applied = false;
    if (s->mb) {
        ui_state_snapshot_t snap;
        bool resync = false;
        if (ui_mailbox_take(s->mb, &s->seen_version, &snap, &resync)) {
            if (resync) {
                s->c.resyncs++;
                // lv_subject_set_int() notifies only on change; a resync must reach every observer
                ui_store_apply(s, &snap);
                for (int i = 0; i < UI_SUBJ_DOMAIN_COUNT; i++) {
                    lv_subject_notify(s->subj[i]);
                }
            } else {
                ui_store_apply(s, &snap);
            }
            applied = true;
        }
        ui_event_t ev;
        // bounded: at most one ring's worth per tick, so a flooding producer cannot starve rendering
        for (int n = 0; n < UI_MAILBOX_EVENT_CAP && ui_mailbox_take_event(s->mb, &ev); n++) {
            s->c.events++;
            if (s->on_event) {
                s->on_event(s->event_ctx, &ev);
            }
        }
    }
    return applied;
}

lv_subject_t *ui_store_subject(ui_store_t *s, ui_subject_id_t id)
{
    return (unsigned)id < UI_SUBJ_COUNT && s->ready ? s->subj[id] : NULL;
}

int32_t ui_store_get(ui_store_t *s, ui_subject_id_t id)
{
    return (unsigned)id < UI_SUBJ_COUNT && s->ready ? lv_subject_get_int(s->subj[id]) : UI_STORE_UNKNOWN;
}

void ui_store_set_active_view(ui_store_t *s, ui_view_id_t v)
{
    if ((unsigned)v < UI_VIEW_COUNT) {
        lv_subject_set_int(s->subj[UI_SUBJ_ACTIVE_VIEW], (int32_t)v);
    }
}

void ui_store_set_overlay(ui_store_t *s, uint16_t overlay_id)
{
    lv_subject_set_int(s->subj[UI_SUBJ_OVERLAY], overlay_id);
}

const char *ui_subject_name(ui_subject_id_t id)
{
    static const char *const names[UI_SUBJ_COUNT] = {
        "host_usb", "client", "owner_kind", "owner_epoch", "wifi", "touch_ok", "drag_lock",
        "modifiers", "mods_armed", "mods_locked", "caps_led", "maintenance", "error", "active_view", "overlay",
    };
    return (unsigned)id < UI_SUBJ_COUNT ? names[id] : "?";
}
