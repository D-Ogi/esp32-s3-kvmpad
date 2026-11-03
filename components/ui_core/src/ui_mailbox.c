// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Bounded UI mailbox (TASK-060B). See ui_mailbox.h for the semantics.
#include "ui_mailbox.h"

#include <string.h>

enum { SRC_IDX_HID = 0, SRC_IDX_NET, SRC_IDX_TOUCH, SRC_IDX_GESTURE };

static void mb_lock(ui_mailbox_t *mb)
{
    if (mb->lk.lock) {
        mb->lk.lock(mb->lk.ctx);
    }
}

static void mb_unlock(ui_mailbox_t *mb)
{
    if (mb->lk.unlock) {
        mb->lk.unlock(mb->lk.ctx);
    }
}

void ui_mailbox_init(ui_mailbox_t *mb, const ui_mailbox_lock_t *lk)
{
    memset(mb, 0, sizeof(*mb));
    if (lk) {
        mb->lk = *lk;
    }
}

// Caller holds the lock. True when the publish may proceed; records the sequence.
static bool seq_accept(ui_mailbox_t *mb, int idx, uint32_t bit, uint32_t seq)
{
    if ((mb->state.sources & bit) && (int32_t)(seq - mb->last_seq[idx]) <= 0) {
        mb->c.stale++;
        return false;
    }
    mb->last_seq[idx] = seq;
    return true;
}

static ui_pub_result_t finish(ui_mailbox_t *mb, bool changed)
{
    if (changed) {
        mb->state.version++;
        mb->c.applied++;
        return UI_PUB_APPLIED;
    }
    mb->c.unchanged++;
    return UI_PUB_UNCHANGED;
}

ui_pub_result_t ui_mailbox_publish_hid(ui_mailbox_t *mb, const ui_hid_slice_t *s, uint32_t domain_seq)
{
    if (!s || s->host_usb_state > KVM_HOST_USB_ERROR || s->owner_kind > OWNER_REMOTE) {
        mb_lock(mb);
        mb->c.published++;
        mb->c.invalid++;
        mb_unlock(mb);
        return UI_PUB_INVALID;
    }
    mb_lock(mb);
    mb->c.published++;
    ui_pub_result_t r = UI_PUB_STALE;
    if (seq_accept(mb, SRC_IDX_HID, UI_SRC_HID, domain_seq)) {
        ui_state_snapshot_t *st = &mb->state;
        bool changed = !(st->sources & UI_SRC_HID) || st->host_usb_state != s->host_usb_state ||
                       st->owner_kind != s->owner_kind || st->owner_epoch != s->owner_epoch ||
                       st->modifiers_down != s->modifiers_down || st->mods_armed != s->mods_armed ||
                       st->mods_locked != s->mods_locked || st->caps_led_known != s->caps_led_known ||
                       st->caps_led_active != s->caps_led_active || st->maintenance != s->maintenance ||
                       st->error_code != s->error_code;
        st->sources |= UI_SRC_HID;
        st->host_usb_state = s->host_usb_state;
        st->owner_kind = s->owner_kind;
        st->owner_epoch = s->owner_epoch;
        st->modifiers_down = s->modifiers_down;
        st->mods_armed = s->mods_armed;
        st->mods_locked = s->mods_locked;
        st->caps_led_known = s->caps_led_known;
        st->caps_led_active = s->caps_led_known && s->caps_led_active;
        st->maintenance = s->maintenance;
        st->error_code = s->error_code;
        r = finish(mb, changed);
    }
    mb_unlock(mb);
    return r;
}

ui_pub_result_t ui_mailbox_publish_net(ui_mailbox_t *mb, const ui_net_slice_t *s, uint32_t domain_seq)
{
    if (!s || s->client_state >= UI_CLIENT_COUNT) {
        mb_lock(mb);
        mb->c.published++;
        mb->c.invalid++;
        mb_unlock(mb);
        return UI_PUB_INVALID;
    }
    mb_lock(mb);
    mb->c.published++;
    ui_pub_result_t r = UI_PUB_STALE;
    if (seq_accept(mb, SRC_IDX_NET, UI_SRC_NET, domain_seq)) {
        ui_state_snapshot_t *st = &mb->state;
        bool changed = !(st->sources & UI_SRC_NET) || st->client_state != s->client_state ||
                       st->wifi_connected != s->wifi_connected;
        st->sources |= UI_SRC_NET;
        st->client_state = s->client_state;
        st->wifi_connected = s->wifi_connected;
        r = finish(mb, changed);
    }
    mb_unlock(mb);
    return r;
}

ui_pub_result_t ui_mailbox_publish_touch(ui_mailbox_t *mb, const ui_touch_slice_t *s, uint32_t domain_seq)
{
    if (!s) {
        return UI_PUB_INVALID;
    }
    mb_lock(mb);
    mb->c.published++;
    ui_pub_result_t r = UI_PUB_STALE;
    if (seq_accept(mb, SRC_IDX_TOUCH, UI_SRC_TOUCH, domain_seq)) {
        ui_state_snapshot_t *st = &mb->state;
        bool changed = !(st->sources & UI_SRC_TOUCH) || st->touch_healthy != s->touch_healthy;
        st->sources |= UI_SRC_TOUCH;
        st->touch_healthy = s->touch_healthy;
        r = finish(mb, changed);
    }
    mb_unlock(mb);
    return r;
}

ui_pub_result_t ui_mailbox_publish_gesture(ui_mailbox_t *mb, const ui_gesture_slice_t *s, uint32_t domain_seq)
{
    if (!s) {
        return UI_PUB_INVALID;
    }
    mb_lock(mb);
    mb->c.published++;
    ui_pub_result_t r = UI_PUB_STALE;
    if (seq_accept(mb, SRC_IDX_GESTURE, UI_SRC_GESTURE, domain_seq)) {
        ui_state_snapshot_t *st = &mb->state;
        bool changed = !(st->sources & UI_SRC_GESTURE) || st->drag_lock_active != s->drag_lock_active;
        st->sources |= UI_SRC_GESTURE;
        st->drag_lock_active = s->drag_lock_active;
        r = finish(mb, changed);
    }
    mb_unlock(mb);
    return r;
}

bool ui_mailbox_post_event(ui_mailbox_t *mb, const ui_event_t *ev)
{
    if (!ev || ev->kind >= UI_EV_COUNT) {
        return false;
    }
    bool kept_all = true;
    mb_lock(mb);
    mb->c.events_posted++;
    if (mb->ev_count == UI_MAILBOX_EVENT_CAP) {
        mb->ev_head = (uint16_t)((mb->ev_head + 1u) % UI_MAILBOX_EVENT_CAP);
        mb->ev_count--;
        mb->c.events_lost++;
        mb->resync_pending = true;
        kept_all = false;
    }
    uint16_t tail = (uint16_t)((mb->ev_head + mb->ev_count) % UI_MAILBOX_EVENT_CAP);
    mb->ev[tail] = *ev;
    mb->ev_count++;
    if (mb->ev_count > mb->c.event_high_water) {
        mb->c.event_high_water = mb->ev_count;
    }
    mb_unlock(mb);
    return kept_all;
}

bool ui_mailbox_take(ui_mailbox_t *mb, uint32_t *seen_version, ui_state_snapshot_t *out, bool *resync)
{
    bool wrote = false;
    bool rs = false;
    mb_lock(mb);
    rs = mb->resync_pending;
    if (rs || mb->state.version != *seen_version) {
        *out = mb->state;
        *seen_version = mb->state.version;
        mb->resync_pending = false;
        mb->c.takes++;
        wrote = true;
    }
    mb_unlock(mb);
    if (resync) {
        *resync = rs;
    }
    return wrote;
}

bool ui_mailbox_take_event(ui_mailbox_t *mb, ui_event_t *out)
{
    bool got = false;
    mb_lock(mb);
    if (mb->ev_count) {
        *out = mb->ev[mb->ev_head];
        mb->ev_head = (uint16_t)((mb->ev_head + 1u) % UI_MAILBOX_EVENT_CAP);
        mb->ev_count--;
        got = true;
    }
    mb_unlock(mb);
    return got;
}

void ui_mailbox_counters(ui_mailbox_t *mb, ui_mailbox_counters_t *out)
{
    mb_lock(mb);
    *out = mb->c;
    mb_unlock(mb);
}

const char *ui_pub_result_name(ui_pub_result_t r)
{
    switch (r) {
    case UI_PUB_APPLIED: return "APPLIED";
    case UI_PUB_UNCHANGED: return "UNCHANGED";
    case UI_PUB_STALE: return "STALE";
    case UI_PUB_INVALID: return "INVALID";
    }
    return "?";
}
