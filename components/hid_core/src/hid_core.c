// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// HID core: InputArbiter, pressed-ledger, bounded queue and HidScheduler (spec 02 §7, 03 §3..5).
//
// Delivery model: every ledger change becomes one report on its interface; the next change on that
// interface waits until the HOST has read the previous report (kvm_hid_core_on_report_complete), so
// a short DOWN is never collapsed into the following UP (FR-203) and each chord phase is a real IN
// transfer (02 §7.3). Motion and wheel coalesce in the queue tail; everything else is non-droppable,
// and a full queue triggers a fail-safe RELEASE_ALL (TECH-005).
#include "hid_core.h"

#include <string.h>

#include "usb_hid_reports.h"

enum {
    Q_KEY_DOWN = 1, // a = usage, b = extra modifier mask pressed in the same report (ADR-016 one-shot)
    Q_KEY_UP,       // a = usage
    Q_MODS_DOWN,    // a = mask
    Q_MODS_UP,      // a = mask
    Q_MOVE,         // x, y
    Q_BTN_DOWN,     // a = button id
    Q_BTN_UP,
    Q_WHEEL,        // x = horizontal, y = vertical
    Q_CHORD,        // b = chord id
    Q_CONSUMER_DOWN, // b = usage
    Q_CONSUMER_UP,
    Q_KEEPALIVE,
};

#define CHORD_DONE 0xFF

enum { BURST_IDLE = 0, BURST_SENDING, BURST_CANCELLING };
#define REMOTE_DELTA_LIMIT 4096 // 03 §6.5: dx/dy outside [-4096, 4096] -> ERR_RANGE

typedef kvm_hid_staged_t staged_t;

static inline staged_t *staged(kvm_hid_core_t *core, uint8_t itf)
{
    return &core->staged[itf];
}

// ---- helpers -------------------------------------------------------------------------------
static void emit_event(kvm_hid_core_t *core, kvm_hid_event_kind_t kind, uint32_t a, uint64_t session)
{
    if (core->port.event == NULL) {
        return;
    }
    kvm_hid_event_t ev = {.kind = kind, .a = a, .epoch = core->owner.epoch, .session_id = session};
    core->port.event(core->port.ctx, &ev);
}

static uint8_t protocol_of(kvm_hid_core_t *core, uint8_t itf)
{
    if (core->port.protocol == NULL) {
        return KVM_HID_PROTOCOL_REPORT;
    }
    return core->port.protocol(core->port.ctx, itf);
}

static bool ledger_keyboard_empty(const kvm_hid_core_t *core)
{
    if (core->modifiers != 0) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (core->keys[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool ledger_empty(const kvm_hid_core_t *core)
{
    return ledger_keyboard_empty(core) && core->buttons == 0 && core->consumer == 0;
}

static uint8_t keys_down(const kvm_hid_core_t *core)
{
    uint8_t n = 0;
    for (int i = 0; i < 6; i++) {
        n = (uint8_t)(n + (core->keys[i] != 0));
    }
    return n;
}

static uint8_t entry_itf(kvm_hid_core_t *core, const kvm_hid_qentry_t *e)
{
    switch (e->kind) {
    case Q_MOVE:
    case Q_BTN_DOWN:
    case Q_BTN_UP:
    case Q_WHEEL:
    case Q_KEEPALIVE:
        return KVM_HID_ITF_MOUSE;
    case Q_CONSUMER_DOWN:
    case Q_CONSUMER_UP:
        return KVM_HID_ITF_CONSUMER;
    case Q_CHORD: {
        const kvm_chord_t *ch = kvm_chord_get(e->b);
        return (ch != NULL && ch->kind == KVM_CHORD_KIND_CONSUMER) ? KVM_HID_ITF_CONSUMER : KVM_HID_ITF_KEYBOARD;
    }
    default:
        (void)core;
        return KVM_HID_ITF_KEYBOARD;
    }
}

static uint8_t chord_itf(kvm_hid_core_t *core)
{
    const kvm_chord_t *ch = kvm_chord_get(core->chord_id);
    return (ch != NULL && ch->kind == KVM_CHORD_KIND_CONSUMER) ? KVM_HID_ITF_CONSUMER : KVM_HID_ITF_KEYBOARD;
}

static bool itf_busy(kvm_hid_core_t *core, uint8_t itf)
{
    staged_t *s = staged(core, itf);
    if (core->in_flight[itf] || core->neutral_pending[itf] || (s != NULL && s->pending)) {
        return true;
    }
    return core->chord_active && chord_itf(core) == itf;
}

// ---- report staging ------------------------------------------------------------------------
static void try_send(kvm_hid_core_t *core, uint8_t itf)
{
    staged_t *s = staged(core, itf);
    if (s == NULL || !s->pending || core->in_flight[itf] || core->usb != KVM_HOST_USB_MOUNTED) {
        return;
    }
    if (core->port.send != NULL && core->port.send(core->port.ctx, itf, s->buf, s->len)) {
        s->pending = false;
        core->in_flight[itf] = true;
        core->in_flight_since[itf] = core->now_us;
        core->c.reports_sent++;
    } else {
        core->c.send_refused++;
    }
}

static void stage_keyboard(kvm_hid_core_t *core)
{
    staged_t *s = staged(core, KVM_HID_ITF_KEYBOARD);
    s->len = (uint8_t)kvm_hid_build_keyboard(s->buf, core->modifiers, core->keys);
    s->pending = true;
    try_send(core, KVM_HID_ITF_KEYBOARD);
}

static void stage_mouse(kvm_hid_core_t *core, int8_t dx, int8_t dy, int8_t wheel, int8_t pan)
{
    staged_t *s = staged(core, KVM_HID_ITF_MOUSE);
    s->len = (uint8_t)kvm_hid_build_mouse(s->buf, protocol_of(core, KVM_HID_ITF_MOUSE), core->buttons, dx, dy, wheel,
                                          pan);
    s->pending = true;
    try_send(core, KVM_HID_ITF_MOUSE);
}

static void stage_consumer(kvm_hid_core_t *core)
{
    staged_t *s = staged(core, KVM_HID_ITF_CONSUMER);
    s->len = (uint8_t)kvm_hid_build_consumer(s->buf, core->consumer);
    s->pending = true;
    try_send(core, KVM_HID_ITF_CONSUMER);
}

static void stage_neutral(kvm_hid_core_t *core, uint8_t itf)
{
    staged_t *s = staged(core, itf);
    s->len = (uint8_t)kvm_hid_build_neutral(itf, protocol_of(core, itf), s->buf);
    s->pending = true;
    core->neutral_pending[itf] = false;
    try_send(core, itf);
}

// ---- ledger --------------------------------------------------------------------------------
static bool ledger_key_down(kvm_hid_core_t *core, uint8_t usage)
{
    if (kvm_usage_is_modifier(usage)) {
        uint8_t bit = KVM_MOD_BIT(usage);
        if (core->modifiers & bit) {
            return false; // idempotent (03 §3 rule 5)
        }
        core->modifiers = (uint8_t)(core->modifiers | bit);
        return true;
    }
    int free_slot = -1;
    for (int i = 0; i < 6; i++) {
        if (core->keys[i] == usage) {
            return false; // idempotent
        }
        if (core->keys[i] == 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        core->c.rollover_rejected++; // AT-HID-03: never more than 6 usages, reject gracefully
        return false;
    }
    core->keys[free_slot] = usage;
    return true;
}

static bool ledger_key_up(kvm_hid_core_t *core, uint8_t usage)
{
    if (kvm_usage_is_modifier(usage)) {
        uint8_t bit = KVM_MOD_BIT(usage);
        if (!(core->modifiers & bit)) {
            core->c.keyup_without_down++;
            return false;
        }
        core->modifiers = (uint8_t)(core->modifiers & ~bit);
        return true;
    }
    for (int i = 0; i < 6; i++) {
        if (core->keys[i] == usage) {
            // Keep press order: shift the later keys down.
            for (int j = i; j < 5; j++) {
                core->keys[j] = core->keys[j + 1];
            }
            core->keys[5] = 0;
            return true;
        }
    }
    core->c.keyup_without_down++; // no random bitmap change (03 §3 rule 5)
    return false;
}

static void ledger_clear(kvm_hid_core_t *core)
{
    core->modifiers = 0;
    memset(core->keys, 0, sizeof(core->keys));
    core->buttons = 0;
    core->consumer = 0;
}

// ---- queue ---------------------------------------------------------------------------------
static kvm_hid_qentry_t *q_at(kvm_hid_core_t *core, uint16_t i)
{
    return &core->q[(core->q_head + i) % KVM_HID_QUEUE_CAP];
}

static void q_pop(kvm_hid_core_t *core)
{
    core->q_head = (uint16_t)((core->q_head + 1) % KVM_HID_QUEUE_CAP);
    core->q_count--;
}

static int32_t clamp_accum(kvm_hid_core_t *core, int32_t v)
{
    int32_t lim = core->cfg.move_accum_limit;
    if (v > lim) {
        core->c.moves_dropped++;
        return lim;
    }
    if (v < -lim) {
        core->c.moves_dropped++;
        return -lim;
    }
    return v;
}

static kvm_hid_err_t q_push(kvm_hid_core_t *core, uint8_t kind, uint8_t a, uint16_t b, int32_t x, int32_t y)
{
    const uint32_t epoch = core->owner.epoch;
    if ((kind == Q_MOVE || kind == Q_WHEEL) && core->q_count > 0) {
        kvm_hid_qentry_t *tail = q_at(core, (uint16_t)(core->q_count - 1));
        if (tail->kind == kind && tail->epoch == epoch) {
            tail->x = clamp_accum(core, tail->x + x);
            tail->y = clamp_accum(core, tail->y + y);
            return KVM_HID_OK;
        }
    }
    if (core->q_count >= KVM_HID_QUEUE_CAP) {
        if (kind == Q_MOVE || kind == Q_WHEEL || kind == Q_KEEPALIVE) {
            core->c.moves_dropped++; // pure motion may be compressed or dropped (03 §5.3)
            return KVM_HID_OK;
        }
        core->c.queue_overflows++;
        emit_event(core, KVM_HID_EV_QUEUE_OVERFLOW, 0, core->owner.session_id);
        kvm_hid_core_release_all(core, KVM_RA_QUEUE_OVERFLOW);
        return KVM_HID_ERR_HID_QUEUE_OVERFLOW;
    }
    kvm_hid_qentry_t *e = q_at(core, core->q_count);
    e->kind = kind;
    e->a = a;
    e->b = b;
    e->epoch = epoch;
    e->x = clamp_accum(core, x);
    e->y = clamp_accum(core, y);
    core->q_count++;
    if (core->q_count > core->c.queue_high_water) {
        core->c.queue_high_water = core->q_count;
    }
    return KVM_HID_OK;
}

static int8_t take_chunk(int32_t *acc)
{
    int32_t v = *acc;
    if (v > 127) {
        v = 127;
    } else if (v < -127) {
        v = -127;
    }
    *acc -= v;
    return (int8_t)v;
}

// ---- chords --------------------------------------------------------------------------------
static void chord_finish(kvm_hid_core_t *core, bool delivered)
{
    if (!core->chord_active) {
        return;
    }
    core->chord_active = false;
    if (delivered) {
        core->c.chords_delivered++;
        emit_event(core, KVM_HID_EV_CHORD_DELIVERED, core->chord_id, 0);
    } else {
        core->c.chords_aborted++;
        emit_event(core, KVM_HID_EV_CHORD_ABORTED, core->chord_id, 0);
    }
}

static void chord_start(kvm_hid_core_t *core, uint16_t chord_id)
{
    const kvm_chord_t *ch = kvm_chord_get(chord_id);
    if (ch == NULL) {
        return;
    }
    core->chord_active = true;
    core->chord_id = chord_id;
    core->chord_phase = 0;
    if (ch->kind == KVM_CHORD_KIND_KEYBOARD) {
        // FR-209: a shortcut never inherits keyboard modifiers; start from a neutral keyboard.
        kvm_kbd_init(&core->kbd);
        if (!ledger_keyboard_empty(core)) {
            core->modifiers = 0;
            memset(core->keys, 0, sizeof(core->keys));
            stage_keyboard(core);
        }
    } else if (core->consumer != 0) {
        core->consumer = 0;
        stage_consumer(core);
    }
}

// Stage the next phase when the chord's interface is free; returns true while the chord runs.
static void chord_step(kvm_hid_core_t *core)
{
    const kvm_chord_t *ch = kvm_chord_get(core->chord_id);
    uint8_t itf = chord_itf(core);
    staged_t *s = staged(core, itf);
    if (ch == NULL || core->in_flight[itf] || core->neutral_pending[itf] || s->pending) {
        return;
    }
    if (core->chord_phase == CHORD_DONE) {
        chord_finish(core, true); // the last phase was read by the HOST
        return;
    }
    if (ch->kind == KVM_CHORD_KIND_CONSUMER) {
        if (core->chord_phase == 0) {
            core->consumer = ch->usage;
            core->chord_phase = 1;
        } else {
            core->consumer = 0;
            core->chord_phase = CHORD_DONE;
        }
        stage_consumer(core);
        return;
    }
    // Keyboard: 1 mods, 2 mods+key, 3 mods, 4 neutral (02 §7.3); 1 and 3 skipped without modifiers.
    for (;;) {
        uint8_t p = core->chord_phase;
        if (p == 0 && ch->modifiers == 0) {
            core->chord_phase = 1;
            continue;
        }
        if (p == 2 && ch->modifiers == 0) {
            core->chord_phase = 3;
            continue;
        }
        break;
    }
    memset(core->keys, 0, sizeof(core->keys));
    switch (core->chord_phase) {
    case 0:
        core->modifiers = ch->modifiers;
        core->chord_phase = 1;
        break;
    case 1:
        core->modifiers = ch->modifiers;
        core->keys[0] = (uint8_t)ch->usage;
        core->chord_phase = 2;
        break;
    case 2:
        core->modifiers = ch->modifiers;
        core->chord_phase = 3;
        break;
    default:
        core->modifiers = 0;
        core->chord_phase = CHORD_DONE;
        break;
    }
    stage_keyboard(core);
}

// ---- processing ----------------------------------------------------------------------------
// Apply the queue head to the ledger and stage its report. Returns false if the head must wait.
static bool process_head(kvm_hid_core_t *core)
{
    if (core->q_count == 0) {
        return false;
    }
    kvm_hid_qentry_t *e = q_at(core, 0);
    if (e->epoch != core->owner.epoch) {
        core->c.stale_dropped++; // INV-002: old queued intents are rejected after an epoch change
        if (e->kind == Q_CHORD) {
            core->c.chords_aborted++;
            emit_event(core, KVM_HID_EV_CHORD_ABORTED, e->b, 0);
        }
        q_pop(core);
        return true;
    }
    uint8_t itf = entry_itf(core, e);
    if (itf_busy(core, itf)) {
        return false;
    }
    switch (e->kind) {
    case Q_KEY_DOWN: {
        bool changed = false;
        uint8_t extra = (uint8_t)e->b;
        if (extra & (uint8_t)~core->modifiers) {
            core->modifiers = (uint8_t)(core->modifiers | extra);
            changed = true;
        }
        changed = ledger_key_down(core, e->a) || changed;
        q_pop(core);
        if (changed) {
            stage_keyboard(core);
        }
        return true;
    }
    case Q_KEY_UP: {
        bool changed = ledger_key_up(core, e->a);
        q_pop(core);
        if (changed) {
            stage_keyboard(core);
        }
        return true;
    }
    case Q_MODS_DOWN:
    case Q_MODS_UP: {
        uint8_t before = core->modifiers;
        core->modifiers = e->kind == Q_MODS_DOWN ? (uint8_t)(before | e->a) : (uint8_t)(before & ~e->a);
        q_pop(core);
        if (core->modifiers != before) {
            stage_keyboard(core);
        }
        return true;
    }
    case Q_MOVE: {
        int8_t dx = take_chunk(&e->x);
        int8_t dy = take_chunk(&e->y);
        if (e->x == 0 && e->y == 0) {
            q_pop(core);
        }
        if (dx != 0 || dy != 0) {
            stage_mouse(core, dx, dy, 0, 0); // the current button mask is kept (03 §5.3 drag)
        }
        return true;
    }
    case Q_WHEEL: {
        if (protocol_of(core, KVM_HID_ITF_MOUSE) == KVM_HID_PROTOCOL_BOOT) {
            core->c.wheel_dropped_boot++; // boot mouse has no wheel/pan: explicit fallback, nothing synthesized
            q_pop(core);
            return true;
        }
        int8_t pan = take_chunk(&e->x);
        int8_t wheel = take_chunk(&e->y);
        if (e->x == 0 && e->y == 0) {
            q_pop(core);
        }
        if (pan != 0 || wheel != 0) {
            stage_mouse(core, 0, 0, wheel, pan);
        }
        return true;
    }
    case Q_BTN_DOWN:
    case Q_BTN_UP: {
        uint8_t bit = (uint8_t)(1u << (e->a - 1));
        uint8_t before = core->buttons;
        if (e->kind == Q_BTN_DOWN) {
            core->buttons = (uint8_t)(before | bit);
        } else if (before & bit) {
            core->buttons = (uint8_t)(before & ~bit);
        } else {
            core->c.keyup_without_down++;
        }
        q_pop(core);
        if (core->buttons != before) {
            stage_mouse(core, 0, 0, 0, 0);
        }
        return true;
    }
    case Q_CONSUMER_DOWN: {
        uint16_t before = core->consumer;
        core->consumer = e->b;
        q_pop(core);
        if (core->consumer != before) {
            stage_consumer(core);
        }
        return true;
    }
    case Q_CONSUMER_UP: {
        bool changed = core->consumer == e->b && e->b != 0;
        if (changed) {
            core->consumer = 0;
        } else {
            core->c.keyup_without_down++;
        }
        q_pop(core);
        if (changed) {
            stage_consumer(core);
        }
        return true;
    }
    case Q_CHORD: {
        uint16_t id = e->b;
        q_pop(core);
        chord_start(core, id);
        return true;
    }
    case Q_KEEPALIVE: {
        q_pop(core);
        if (!ledger_empty(core)) {
            core->c.keepalive_rejected++; // never while anything is held (FR-713)
            return true;
        }
        core->c.keepalive_sent++;
        stage_mouse(core, 0, 0, 0, 0); // dx = dy = 0, no buttons (FR-714)
        return true;
    }
    default:
        q_pop(core);
        return true;
    }
}

// ---- ownership -----------------------------------------------------------------------------
static void revoke_remote(kvm_hid_core_t *core, kvm_release_reason_t reason)
{
    if (core->owner.kind != OWNER_REMOTE) {
        return;
    }
    uint64_t session = core->owner.session_id;
    core->owner.epoch++;
    kvm_hid_core_release_all(core, reason);
    core->owner.kind = OWNER_NONE;
    core->owner.session_id = 0;
    core->owner.lease_expires_us = 0;
    core->c.remote_revokes++;
    emit_event(core, KVM_HID_EV_REMOTE_REVOKED, (uint32_t)reason, session);
}

static kvm_hid_err_t host_gate(kvm_hid_core_t *core)
{
    if (core->maintenance) {
        core->c.rejected_maintenance++;
        return KVM_HID_ERR_OTA_IN_PROGRESS;
    }
    if (core->usb == KVM_HOST_USB_SUSPENDED) {
        core->c.rejected_suspended++;
        return KVM_HID_ERR_USB_SUSPENDED;
    }
    if (core->usb != KVM_HOST_USB_MOUNTED) {
        core->c.rejected_offline++; // INV-005: no simulated confirmation without a mounted HOST
        return KVM_HID_ERR_HOST_OFFLINE;
    }
    return KVM_HID_OK;
}

// 03 §4.3 on_local_input, ownership part: revoke + release + new epoch before the local action.
static kvm_hid_err_t local_admit(kvm_hid_core_t *core, uint64_t now_us)
{
    kvm_hid_err_t err = host_gate(core);
    if (err != KVM_HID_OK) {
        return err;
    }
    if (core->owner.kind == OWNER_REMOTE) {
        revoke_remote(core, KVM_RA_LOCAL_OVERRIDE);
    }
    if (core->owner.kind != OWNER_LOCAL) {
        core->owner.epoch++;
        core->owner.kind = OWNER_LOCAL;
        core->owner.session_id = 0;
        core->owner.lease_expires_us = 0;
    }
    core->last_local_input_us = now_us;
    core->last_real_input_us = now_us;
    return KVM_HID_OK;
}

static kvm_hid_err_t validate(kvm_hid_core_t *core, const kvm_hid_intent_t *in, bool remote)
{
    switch (in->type) {
    case KVM_HID_KEY_DOWN:
    case KVM_HID_KEY_UP:
        if (!kvm_usage_is_valid_key(in->data.key.usage_id)) {
            core->c.rejected_usage++;
            return KVM_HID_ERR_USAGE;
        }
        return KVM_HID_OK;
    case KVM_HID_MOUSE_BTN_DOWN:
    case KVM_HID_MOUSE_BTN_UP:
        if (in->data.mouse_button.button_id < KVM_MOUSE_BTN_LEFT ||
            in->data.mouse_button.button_id > KVM_MOUSE_BTN_MIDDLE) {
            core->c.rejected_usage++;
            return KVM_HID_ERR_USAGE;
        }
        return KVM_HID_OK;
    case KVM_HID_MOUSE_MOVE:
        if (remote && (in->data.mouse_move.dx > REMOTE_DELTA_LIMIT || in->data.mouse_move.dx < -REMOTE_DELTA_LIMIT ||
                       in->data.mouse_move.dy > REMOTE_DELTA_LIMIT || in->data.mouse_move.dy < -REMOTE_DELTA_LIMIT)) {
            return KVM_HID_ERR_RANGE; // no hidden clipping
        }
        return KVM_HID_OK;
    case KVM_HID_MOUSE_WHEEL:
        if (remote && (in->data.wheel.horizontal > REMOTE_DELTA_LIMIT || in->data.wheel.horizontal < -REMOTE_DELTA_LIMIT ||
                       in->data.wheel.vertical > REMOTE_DELTA_LIMIT || in->data.wheel.vertical < -REMOTE_DELTA_LIMIT)) {
            return KVM_HID_ERR_RANGE;
        }
        return KVM_HID_OK;
    case KVM_HID_CHORD:
        if (kvm_chord_get(in->data.chord.chord_id) == NULL) {
            core->c.rejected_usage++;
            return KVM_HID_ERR_USAGE;
        }
        return KVM_HID_OK;
    case KVM_HID_CONSUMER_DOWN:
    case KVM_HID_CONSUMER_UP:
        if (remote || !kvm_consumer_is_allowed(in->data.consumer.usage)) {
            // TECH-075: remote Consumer only through a shortcut command_id (KVM_HID_CHORD).
            core->c.rejected_usage++;
            return KVM_HID_ERR_USAGE;
        }
        return KVM_HID_OK;
    case KVM_HID_RELEASE_ALL:
        return KVM_HID_OK;
    case KVM_HID_KEEPALIVE: // SYSTEM only, generated inside the core (INV-007)
    default:
        core->c.rejected_origin++;
        return KVM_HID_ERR_BAD_INTENT;
    }
}

static kvm_hid_err_t enqueue_intent(kvm_hid_core_t *core, const kvm_hid_intent_t *in)
{
    switch (in->type) {
    case KVM_HID_KEY_DOWN:
        return q_push(core, Q_KEY_DOWN, in->data.key.usage_id, 0, 0, 0);
    case KVM_HID_KEY_UP:
        return q_push(core, Q_KEY_UP, in->data.key.usage_id, 0, 0, 0);
    case KVM_HID_MOUSE_MOVE:
        return q_push(core, Q_MOVE, 0, 0, in->data.mouse_move.dx, in->data.mouse_move.dy);
    case KVM_HID_MOUSE_BTN_DOWN:
        return q_push(core, Q_BTN_DOWN, in->data.mouse_button.button_id, 0, 0, 0);
    case KVM_HID_MOUSE_BTN_UP:
        return q_push(core, Q_BTN_UP, in->data.mouse_button.button_id, 0, 0, 0);
    case KVM_HID_MOUSE_WHEEL:
        return q_push(core, Q_WHEEL, 0, 0, in->data.wheel.horizontal, in->data.wheel.vertical);
    case KVM_HID_CHORD:
        return q_push(core, Q_CHORD, 0, in->data.chord.chord_id, 0, 0);
    case KVM_HID_CONSUMER_DOWN:
        return q_push(core, Q_CONSUMER_DOWN, 0, in->data.consumer.usage, 0, 0);
    case KVM_HID_CONSUMER_UP:
        return q_push(core, Q_CONSUMER_UP, 0, in->data.consumer.usage, 0, 0);
    default:
        return KVM_HID_ERR_BAD_INTENT;
    }
}

// ---- text burst (ADR-024 FR-748) -----------------------------------------------------------
static bool q_has_keyboard(kvm_hid_core_t *core)
{
    for (uint16_t i = 0; i < core->q_count; i++) {
        const kvm_hid_qentry_t *e = q_at(core, i);
        if (e->kind != Q_KEEPALIVE && entry_itf(core, e) == KVM_HID_ITF_KEYBOARD) {
            return true;
        }
    }
    return false;
}

static bool burst_writable(const kvm_hid_core_t *core)
{
    return core->usb == KVM_HOST_USB_MOUNTED && !core->maintenance && core->owner.kind == OWNER_LOCAL &&
           core->owner.epoch == core->burst.epoch;
}

/** Balanced: each DOWN of a key not held, each UP of a held key, <= 6 ordinary keys, nothing held at the end. */
static kvm_hid_err_t burst_validate(const kvm_hid_burst_step_t *steps, uint16_t n)
{
    uint8_t held[32];
    memset(held, 0, sizeof(held));
    int regular = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t u = steps[i].usage;
        if (!kvm_usage_is_valid_key(u)) {
            return KVM_HID_ERR_USAGE;
        }
        bool is_held = (held[u >> 3] >> (u & 7)) & 1u;
        if (steps[i].down == is_held) {
            return KVM_HID_ERR_BAD_INTENT;
        }
        held[u >> 3] = (uint8_t)(held[u >> 3] ^ (1u << (u & 7)));
        if (!kvm_usage_is_modifier(u)) {
            regular += steps[i].down ? 1 : -1;
            if (regular > 6) {
                return KVM_HID_ERR_BAD_INTENT; // more than the 6KRO report can carry
            }
        }
    }
    for (int i = 0; i < 32; i++) {
        if (held[i] != 0) {
            return KVM_HID_ERR_BAD_INTENT;
        }
    }
    return KVM_HID_OK;
}

static void burst_step(kvm_hid_core_t *core)
{
    kvm_hid_burst_t *b = &core->burst;
    if (b->state == BURST_IDLE) {
        return;
    }
    if (!burst_writable(core)) {
        kvm_hid_core_release_all(core, KVM_RA_OWNER_CHANGE); // defensive: every owner/USB change also releases
        return;
    }
    if (q_has_keyboard(core) || itf_busy(core, KVM_HID_ITF_KEYBOARD)) {
        return; // one step at a time: the previous report must be read by the HOST first
    }
    if (b->pos >= b->n) {
        uint16_t sent = b->pos;
        if (b->state == BURST_SENDING) {
            b->state = BURST_IDLE;
            core->c.bursts_done++;
            emit_event(core, KVM_HID_EV_BURST_DONE, sent, 0);
        } else {
            kvm_release_reason_t reason = b->cancel_reason;
            b->state = BURST_IDLE;
            core->c.bursts_cancelled++;
            emit_event(core, KVM_HID_EV_BURST_ABORTED, sent, 0);
            kvm_hid_core_release_all(core, reason);
        }
        return;
    }
    if (core->now_us < b->next_us) {
        return;
    }
    const kvm_hid_burst_step_t st = b->steps[b->pos++];
    if (q_push(core, st.down ? Q_KEY_DOWN : Q_KEY_UP, st.usage, 0, 0, 0) != KVM_HID_OK) {
        return; // q_push already ran the fail-safe RELEASE_ALL, which ended the burst
    }
    b->next_us = core->now_us + KVM_HID_BURST_STEP_US;
    core->last_local_input_us = core->now_us;
    core->last_real_input_us = core->now_us;
}

kvm_hid_err_t kvm_hid_core_burst_start(kvm_hid_core_t *core, const kvm_hid_burst_step_t *steps, uint16_t n,
                                       const kvm_hid_burst_step_t *cancel, uint8_t ncancel, uint64_t now_us)
{
    core->now_us = now_us;
    if (steps == NULL || n == 0 || n > KVM_HID_BURST_CAP || ncancel > KVM_HID_BURST_CANCEL_CAP ||
        (ncancel > 0 && cancel == NULL)) {
        core->c.rejected_usage++;
        return KVM_HID_ERR_BAD_INTENT;
    }
    kvm_hid_err_t err = burst_validate(steps, n);
    if (err == KVM_HID_OK) {
        err = burst_validate(cancel, ncancel);
    }
    if (err != KVM_HID_OK) {
        core->c.rejected_usage++;
        return err;
    }
    if (core->burst.state != BURST_IDLE || !ledger_keyboard_empty(core) || !kvm_kbd_is_idle(&core->kbd) ||
        core->chord_active || q_has_keyboard(core)) {
        return KVM_HID_ERR_BUSY; // never two sequences in flight, never mixed with held keys
    }
    err = local_admit(core, now_us);
    if (err != KVM_HID_OK) {
        return err;
    }
    kvm_hid_burst_t *b = &core->burst;
    memcpy(b->steps, steps, (size_t)n * sizeof(steps[0]));
    if (ncancel > 0) {
        memcpy(b->cancel, cancel, (size_t)ncancel * sizeof(cancel[0]));
    }
    b->n = n;
    b->pos = 0;
    b->ncancel = ncancel;
    b->epoch = core->owner.epoch;
    b->next_us = now_us;
    b->state = BURST_SENDING;
    core->c.bursts_started++;
    core->c.accepted++;
    return KVM_HID_OK;
}

void kvm_hid_core_burst_cancel(kvm_hid_core_t *core, kvm_release_reason_t reason)
{
    kvm_hid_burst_t *b = &core->burst;
    if (b->state == BURST_IDLE) {
        return;
    }
    if (!burst_writable(core)) {
        b->state = BURST_IDLE;
        core->c.bursts_cancelled++;
        emit_event(core, KVM_HID_EV_BURST_ABORTED, b->pos, 0);
        kvm_hid_core_release_all(core, reason);
        return;
    }
    if (b->state == BURST_CANCELLING) {
        return;
    }
    if (b->pos == 0 || b->pos >= b->n) {
        // Nothing sent yet, or every step already queued: no partial sequence to end with the cancel key.
        bool complete = b->pos >= b->n;
        b->state = BURST_IDLE;
        if (complete) {
            core->c.bursts_done++;
            emit_event(core, KVM_HID_EV_BURST_DONE, b->pos, 0);
        } else {
            core->c.bursts_cancelled++;
            emit_event(core, KVM_HID_EV_BURST_ABORTED, 0, 0);
        }
        kvm_hid_core_release_all(core, reason);
        return;
    }
    // Keys the burst holds now, in press order (steps already queued count as sent).
    uint8_t held[KVM_HID_BURST_CAP];
    uint16_t nheld = 0;
    for (uint16_t i = 0; i < b->pos; i++) {
        if (b->steps[i].down) {
            held[nheld++] = b->steps[i].usage;
        } else {
            for (uint16_t j = 0; j < nheld; j++) {
                if (held[j] == b->steps[i].usage) {
                    memmove(&held[j], &held[j + 1], (size_t)(nheld - j - 1));
                    nheld--;
                    break;
                }
            }
        }
    }
    // Release them (last pressed first) before the cancel key, so e.g. Ctrl+Shift never meet Esc.
    uint16_t n = 0;
    for (uint16_t j = nheld; j > 0; j--) {
        b->steps[n].usage = held[j - 1];
        b->steps[n].down = false;
        n++;
    }
    for (uint8_t k = 0; k < b->ncancel; k++) {
        b->steps[n++] = b->cancel[k];
    }
    b->n = n;
    b->pos = 0;
    b->cancel_reason = reason;
    b->state = BURST_CANCELLING;
    b->next_us = core->now_us;
}

bool kvm_hid_core_burst_active(const kvm_hid_core_t *core)
{
    return core->burst.state != BURST_IDLE;
}

// ---- public API ----------------------------------------------------------------------------
void kvm_hid_core_init(kvm_hid_core_t *core, const kvm_hid_port_t *port, const kvm_hid_config_t *cfg)
{
    memset(core, 0, sizeof(*core));
    if (port != NULL) {
        core->port = *port;
    }
    const kvm_hid_config_t def = KVM_HID_CONFIG_DEFAULT;
    core->cfg = cfg != NULL ? *cfg : def;
    if (core->cfg.move_accum_limit <= 0) {
        core->cfg.move_accum_limit = def.move_accum_limit;
    }
    core->usb = KVM_HOST_USB_UNINITIALIZED;
    core->owner.kind = OWNER_NONE;
    core->keepalive_interval_us = 30000000u;
    kvm_kbd_init(&core->kbd);
}

void kvm_hid_core_release_all(kvm_hid_core_t *core, kvm_release_reason_t reason)
{
    if (core->burst.state != BURST_IDLE) {
        // FR-748: every RELEASE_ALL ends the burst at once; nothing of it is replayed later.
        core->burst.state = BURST_IDLE;
        core->c.bursts_aborted++;
        emit_event(core, KVM_HID_EV_BURST_ABORTED, core->burst.pos, 0);
    }
    chord_finish(core, false);
    // Shortcuts still waiting in the queue are dropped too: report each one as aborted, so the UI and
    // the CLIENT never wait for a confirmation that cannot come (and nothing is replayed later).
    for (uint16_t i = 0; i < core->q_count; i++) {
        const kvm_hid_qentry_t *e = q_at(core, i);
        if (e->kind == Q_CHORD) {
            core->c.chords_aborted++;
            emit_event(core, KVM_HID_EV_CHORD_ABORTED, e->b, 0);
        }
    }
    ledger_clear(core);
    kvm_kbd_init(&core->kbd);
    core->q_head = 0;
    core->q_count = 0;
    for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
        staged_t *s = staged(core, i);
        if (s != NULL) {
            s->pending = false; // a staged non-neutral report is superseded by the neutral one
        }
        core->neutral_pending[i] = true;
    }
    core->c.release_all++;
    if (reason < KVM_RA_COUNT) {
        core->c.release_by_reason[reason]++;
    }
    emit_event(core, KVM_HID_EV_RELEASE_ALL, (uint32_t)reason, core->owner.session_id);
    if (core->usb == KVM_HOST_USB_MOUNTED) {
        for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
            if (!core->in_flight[i]) {
                stage_neutral(core, i);
            }
        }
    }
}

void kvm_hid_core_set_usb(kvm_hid_core_t *core, kvm_host_usb_t state, uint32_t usb_epoch, uint64_t now_us)
{
    core->now_us = now_us;
    kvm_host_usb_t prev = core->usb;
    if (state == KVM_HOST_USB_MOUNTED) {
        core->usb = state;
        if (usb_epoch != core->usb_epoch || prev != KVM_HOST_USB_MOUNTED) {
            if (usb_epoch != core->usb_epoch) {
                // Fresh mount: nothing is in flight on the new configuration.
                for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
                    core->in_flight[i] = false;
                }
            }
            core->usb_epoch = usb_epoch;
            // TECH-035 / FR-402: every (re)mount and resume starts from neutral reports only.
            for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
                staged_t *s = staged(core, i);
                s->pending = false;
                core->neutral_pending[i] = true;
            }
        }
        return;
    }
    core->usb = state;
    if (prev == KVM_HOST_USB_MOUNTED || !ledger_empty(core) || core->q_count != 0 || core->chord_active) {
        // INV-006 / 03 §3 rule 6: the ledger is empty after umount or suspend, regardless of delivery.
        kvm_hid_core_release_all(core, state == KVM_HOST_USB_SUSPENDED ? KVM_RA_USB_SUSPEND : KVM_RA_USB_LOSS);
    }
}

void kvm_hid_core_on_report_complete(kvm_hid_core_t *core, uint8_t itf)
{
    if (itf < KVM_HID_ITFS) {
        core->in_flight[itf] = false;
    }
}

static void keepalive_tick(kvm_hid_core_t *core)
{
    if (!core->keepalive_enabled) {
        return;
    }
    if (core->owner.epoch != core->keepalive_epoch) {
        // An owner change restarts the idle interval (TECH-076: rejected across an epoch change).
        core->keepalive_epoch = core->owner.epoch;
        core->last_real_input_us = core->now_us;
        return;
    }
    if (core->now_us - core->last_real_input_us < core->keepalive_interval_us) {
        return;
    }
    if (core->usb != KVM_HOST_USB_MOUNTED || core->maintenance || !ledger_empty(core) ||
        !kvm_kbd_is_idle(&core->kbd) || core->q_count != 0 || core->chord_active ||
        core->burst.state != BURST_IDLE) {
        core->c.keepalive_rejected++;
        core->last_real_input_us = core->now_us;
        return;
    }
    core->last_real_input_us = core->now_us;
    q_push(core, Q_KEEPALIVE, 0, 0, 0, 0);
}

void kvm_hid_core_poll(kvm_hid_core_t *core, uint64_t now_us)
{
    core->now_us = now_us;
    if (core->owner.kind == OWNER_REMOTE && now_us >= core->owner.lease_expires_us) {
        revoke_remote(core, KVM_RA_LEASE_TIMEOUT);
    }
    for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
        if (core->in_flight[i] && now_us - core->in_flight_since[i] > core->cfg.completion_timeout_us) {
            core->in_flight[i] = false;
            core->c.completion_timeouts++;
            core->c.completion_timeouts_itf[i]++;
            kvm_hid_core_release_all(core, KVM_RA_COMPLETION_TIMEOUT); // no chord repeat (02 §7.3)
        }
    }
    keepalive_tick(core);
    if (core->usb != KVM_HOST_USB_MOUNTED) {
        return;
    }
    for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
        try_send(core, i);
        if (core->neutral_pending[i] && !core->in_flight[i]) {
            stage_neutral(core, i);
        }
    }
    if (core->chord_active) {
        chord_step(core);
    }
    burst_step(core);
    // Bounded work per poll: at most one staged report per interface can be in flight anyway.
    for (int guard = 0; guard < KVM_HID_QUEUE_CAP && process_head(core); guard++) {
        if (core->chord_active) {
            chord_step(core);
        }
    }
}

kvm_hid_err_t kvm_hid_core_local(kvm_hid_core_t *core, const kvm_hid_intent_t *intent, uint64_t now_us)
{
    core->now_us = now_us;
    kvm_hid_err_t err = validate(core, intent, false);
    if (err != KVM_HID_OK) {
        return err;
    }
    if (intent->type == KVM_HID_RELEASE_ALL) {
        kvm_hid_core_release_all(core, KVM_RA_REQUEST);
        return KVM_HID_OK;
    }
    if (core->burst.state != BURST_IDLE &&
        (intent->type == KVM_HID_KEY_DOWN || intent->type == KVM_HID_KEY_UP || intent->type == KVM_HID_CHORD)) {
        return KVM_HID_ERR_BUSY; // FR-748: the burst owns the keyboard until it ends or is cancelled
    }
    err = local_admit(core, now_us);
    if (err != KVM_HID_OK) {
        return err;
    }
    if (intent->type == KVM_HID_CHORD) {
        // FR-308: a macro needs a mounted HOST and LOCAL ownership — both hold after local_admit.
        kvm_kbd_init(&core->kbd);
    }
    err = enqueue_intent(core, intent);
    if (err == KVM_HID_OK) {
        core->c.accepted++;
    }
    return err;
}

static kvm_hid_err_t push_kbd_ops(kvm_hid_core_t *core, const kvm_kbd_op_t *ops, int n)
{
    for (int i = 0; i < n; i++) {
        kvm_hid_err_t err = KVM_HID_OK;
        switch (ops[i].kind) {
        case KVM_KBD_OP_KEY_DOWN:
            err = q_push(core, Q_KEY_DOWN, ops[i].usage, ops[i].mods, 0, 0);
            break;
        case KVM_KBD_OP_KEY_UP:
            err = q_push(core, Q_KEY_UP, ops[i].usage, 0, 0, 0);
            break;
        case KVM_KBD_OP_MODS_DOWN:
            err = q_push(core, Q_MODS_DOWN, ops[i].mods, 0, 0, 0);
            break;
        case KVM_KBD_OP_MODS_UP:
            err = q_push(core, Q_MODS_UP, ops[i].mods, 0, 0, 0);
            break;
        }
        if (err != KVM_HID_OK) {
            return err;
        }
    }
    core->c.accepted++;
    return KVM_HID_OK;
}

kvm_hid_err_t kvm_hid_core_ui_modifier_tap(kvm_hid_core_t *core, uint8_t usage, uint64_t now_us)
{
    core->now_us = now_us;
    if (core->burst.state != BURST_IDLE) {
        return KVM_HID_ERR_BUSY;
    }
    if (!kvm_usage_is_modifier(usage)) {
        core->c.rejected_usage++;
        return KVM_HID_ERR_USAGE;
    }
    kvm_hid_err_t err = local_admit(core, now_us);
    if (err != KVM_HID_OK) {
        return err;
    }
    kvm_kbd_op_t ops[KVM_KBD_MAX_OPS];
    int n = kvm_kbd_modifier_tap(&core->kbd, usage, now_us, ops);
    return push_kbd_ops(core, ops, n < 0 ? 0 : n);
}

kvm_hid_err_t kvm_hid_core_ui_key_down(kvm_hid_core_t *core, uint8_t usage, uint64_t now_us)
{
    core->now_us = now_us;
    if (core->burst.state != BURST_IDLE) {
        return KVM_HID_ERR_BUSY;
    }
    if (!kvm_usage_is_valid_key(usage) || kvm_usage_is_modifier(usage)) {
        core->c.rejected_usage++;
        return KVM_HID_ERR_USAGE;
    }
    kvm_hid_err_t err = local_admit(core, now_us);
    if (err != KVM_HID_OK) {
        return err;
    }
    kvm_kbd_op_t ops[KVM_KBD_MAX_OPS];
    int n = kvm_kbd_key_down(&core->kbd, usage, now_us, ops);
    return push_kbd_ops(core, ops, n < 0 ? 0 : n);
}

kvm_hid_err_t kvm_hid_core_ui_key_up(kvm_hid_core_t *core, uint8_t usage, uint64_t now_us)
{
    core->now_us = now_us;
    if (core->burst.state != BURST_IDLE) {
        return KVM_HID_ERR_BUSY;
    }
    if (!kvm_usage_is_valid_key(usage) || kvm_usage_is_modifier(usage)) {
        core->c.rejected_usage++;
        return KVM_HID_ERR_USAGE;
    }
    // A key UP is always admitted to the queue when mounted: it can only release state.
    kvm_hid_err_t err = local_admit(core, now_us);
    if (err != KVM_HID_OK) {
        return err;
    }
    kvm_kbd_op_t ops[KVM_KBD_MAX_OPS];
    int n = kvm_kbd_key_up(&core->kbd, usage, now_us, ops);
    return push_kbd_ops(core, ops, n < 0 ? 0 : n);
}

static bool local_idle(const kvm_hid_core_t *core, uint64_t now_us)
{
    return now_us - core->last_local_input_us >= core->cfg.local_idle_us && ledger_empty(core) &&
           kvm_kbd_is_idle(&core->kbd) && !core->chord_active && core->q_count == 0 &&
           core->burst.state == BURST_IDLE;
}

kvm_hid_err_t kvm_hid_core_remote_request(kvm_hid_core_t *core, uint64_t session_id, uint64_t now_us)
{
    core->now_us = now_us;
    if (core->maintenance) {
        return KVM_HID_ERR_OTA_IN_PROGRESS;
    }
    if (core->owner.kind == OWNER_REMOTE && core->owner.session_id != session_id &&
        now_us < core->owner.lease_expires_us) {
        return KVM_HID_ERR_CONTROL_DENIED; // INV-004: one lease at a time
    }
    if (core->owner.kind == OWNER_LOCAL && !local_idle(core, now_us)) {
        return KVM_HID_ERR_CONTROL_DENIED; // INV-003
    }
    return KVM_HID_OK;
}

kvm_hid_err_t kvm_hid_core_remote_grant(kvm_hid_core_t *core, uint64_t session_id, uint64_t now_us,
                                        uint32_t *lease_epoch)
{
    kvm_hid_err_t err = kvm_hid_core_remote_request(core, session_id, now_us);
    if (err != KVM_HID_OK) {
        return err;
    }
    if (core->owner.kind == OWNER_REMOTE) {
        revoke_remote(core, KVM_RA_OWNER_CHANGE);
    }
    core->owner.epoch++;
    kvm_hid_core_release_all(core, KVM_RA_OWNER_CHANGE);
    core->owner.kind = OWNER_REMOTE;
    core->owner.session_id = session_id;
    core->owner.lease_expires_us = now_us + core->cfg.lease_us;
    core->last_heartbeat_us = now_us;
    core->c.remote_grants++;
    if (lease_epoch != NULL) {
        *lease_epoch = core->owner.epoch;
    }
    emit_event(core, KVM_HID_EV_REMOTE_GRANTED, 0, session_id);
    return KVM_HID_OK;
}

void kvm_hid_core_remote_heartbeat(kvm_hid_core_t *core, uint64_t session_id, uint64_t now_us)
{
    core->now_us = now_us;
    if (core->owner.kind == OWNER_REMOTE && core->owner.session_id == session_id &&
        now_us < core->owner.lease_expires_us) {
        core->owner.lease_expires_us = now_us + core->cfg.lease_us;
        core->last_heartbeat_us = now_us;
    }
}

kvm_hid_err_t kvm_hid_core_remote(kvm_hid_core_t *core, uint64_t session_id, uint32_t lease_epoch,
                                  const kvm_hid_intent_t *intent, uint64_t now_us)
{
    core->now_us = now_us;
    kvm_hid_err_t err = host_gate(core);
    if (err != KVM_HID_OK) {
        return err;
    }
    if (core->owner.kind != OWNER_REMOTE) {
        core->c.rejected_epoch++;
        return KVM_HID_ERR_STALE_EPOCH; // the lease is gone (revoked / never granted)
    }
    if (core->owner.session_id != session_id) {
        core->c.rejected_session++;
        return KVM_HID_ERR_SESSION;
    }
    if (lease_epoch != core->owner.epoch) {
        core->c.rejected_epoch++;
        return KVM_HID_ERR_STALE_EPOCH;
    }
    if (now_us - core->last_heartbeat_us > core->cfg.stale_us || now_us >= core->owner.lease_expires_us) {
        core->c.rejected_stale++;
        return KVM_HID_ERR_CLIENT_STALE; // 03 §4.3: no sending after 3 s without heartbeat
    }
    err = validate(core, intent, true);
    if (err != KVM_HID_OK) {
        return err;
    }
    core->last_real_input_us = now_us;
    if (intent->type == KVM_HID_RELEASE_ALL) {
        kvm_hid_core_release_all(core, KVM_RA_REQUEST);
        return KVM_HID_OK;
    }
    err = enqueue_intent(core, intent);
    if (err == KVM_HID_OK) {
        core->c.accepted++;
    }
    return err;
}

void kvm_hid_core_remote_release(kvm_hid_core_t *core, uint64_t session_id)
{
    if (core->owner.kind == OWNER_REMOTE && core->owner.session_id == session_id) {
        revoke_remote(core, KVM_RA_REMOTE_RELEASE);
    }
}

void kvm_hid_core_remote_disconnect(kvm_hid_core_t *core, uint64_t session_id)
{
    if (core->owner.kind == OWNER_REMOTE && core->owner.session_id == session_id) {
        revoke_remote(core, KVM_RA_REMOTE_DISCONNECT);
    }
}

void kvm_hid_core_set_maintenance(kvm_hid_core_t *core, bool on)
{
    if (on && !core->maintenance) {
        // PROC-009 / TECH-053: take control from the CLIENT, release, block input.
        if (core->owner.kind == OWNER_REMOTE) {
            revoke_remote(core, KVM_RA_MAINTENANCE);
        } else {
            core->owner.epoch++;
            kvm_hid_core_release_all(core, KVM_RA_MAINTENANCE);
        }
    }
    core->maintenance = on;
}

void kvm_hid_core_set_keepalive(kvm_hid_core_t *core, bool enabled, uint32_t interval_s)
{
    if (interval_s < 10) {
        interval_s = 10;
    } else if (interval_s > 300) {
        interval_s = 300;
    }
    core->keepalive_enabled = enabled;
    core->keepalive_interval_us = (uint64_t)interval_s * 1000000u;
    core->keepalive_epoch = core->owner.epoch;
    core->last_real_input_us = core->now_us;
}

void kvm_hid_core_snapshot(const kvm_hid_core_t *core, kvm_hid_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->host_usb = core->usb;
    out->owner_kind = core->owner.kind;
    out->owner_epoch = core->owner.epoch;
    out->maintenance = core->maintenance;
    out->modifiers_down = core->modifiers;
    out->mods_armed = core->kbd.armed;
    out->mods_locked = core->kbd.locked;
    out->keys_down = keys_down(core);
    out->buttons_down = core->buttons;
    out->consumer_down = core->consumer;
    out->chord_active = core->chord_active;
    out->keepalive_enabled = core->keepalive_enabled;
    out->ledger_empty = ledger_empty(core);
    out->burst_active = core->burst.state != BURST_IDLE;
}

const kvm_hid_counters_t *kvm_hid_core_counters(const kvm_hid_core_t *core)
{
    return &core->c;
}

const kvm_control_owner_t *kvm_hid_core_owner(const kvm_hid_core_t *core)
{
    return &core->owner;
}

const char *kvm_hid_err_name(kvm_hid_err_t e)
{
    static const char *const k[] = {
        "OK", "ERR_HOST_OFFLINE", "ERR_USB_SUSPENDED", "ERR_STALE_EPOCH", "ERR_SESSION", "ERR_CLIENT_STALE",
        "ERR_CONTROL_DENIED", "ERR_USAGE", "ERR_RANGE", "ERR_HID_QUEUE_OVERFLOW", "ERR_OTA_IN_PROGRESS",
        "ERR_BAD_INTENT", "ERR_BUSY",
    };
    return (unsigned)e < sizeof(k) / sizeof(k[0]) ? k[e] : "?";
}

const char *kvm_release_reason_name(kvm_release_reason_t r)
{
    static const char *const k[] = {
        "request", "owner_change", "local_override", "lease_timeout", "remote_release", "remote_disconnect",
        "usb_loss", "usb_suspend", "maintenance", "queue_overflow", "completion_timeout", "gesture_cancel",
        "view_change",
    };
    return (unsigned)r < sizeof(k) / sizeof(k[0]) ? k[r] : "?";
}
