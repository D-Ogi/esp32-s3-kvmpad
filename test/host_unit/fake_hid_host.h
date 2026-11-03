// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Fake USB HOST for the hid_core tests: records every report the core hands to the endpoint, keeps a
// model of what the HOST believes is pressed, counts press transitions per usage (phantom repeats)
// and can refuse or delay completions to exercise busy endpoints and timeouts.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hid_core.h"
#include "usb_hid_reports.h"

#define FH_LOG_MAX 4096

typedef struct {
    uint8_t itf;
    uint8_t len;
    uint8_t b[8];
} fh_report_t;

typedef struct {
    fh_report_t log[FH_LOG_MAX];
    int n;
    int dropped_log;
    bool refuse[KVM_HID_ITFS];     // endpoint not ready: send() returns false
    bool no_complete;              // the HOST stops reading (completion never arrives)
    uint8_t protocol[KVM_HID_ITFS];
    // HOST-side model
    uint8_t mods;
    uint8_t keys[6];
    uint8_t buttons;
    uint16_t consumer;
    long sum_dx, sum_dy, sum_wheel, sum_pan;
    uint32_t key_presses[256];     // 0 -> 1 transitions per keyboard usage (modifiers as 0xE0..0xE7)
    uint32_t button_presses[8];
    uint32_t consumer_presses;
    int max_abs_delta;
    // events
    kvm_hid_event_t ev[256];
    int nev;
} fake_host_t;

static inline bool fh_key_in(const uint8_t keys[6], uint8_t u)
{
    for (int i = 0; i < 6; i++) {
        if (keys[i] == u) {
            return true;
        }
    }
    return false;
}

static inline void fh_apply(fake_host_t *h, uint8_t itf, const uint8_t *r, uint8_t len)
{
    if (itf == KVM_HID_ITF_KEYBOARD && len == 8) {
        uint8_t mods = r[0];
        for (int bit = 0; bit < 8; bit++) {
            if ((mods & (1u << bit)) && !(h->mods & (1u << bit))) {
                h->key_presses[0xE0 + bit]++;
            }
        }
        for (int i = 2; i < 8; i++) {
            if (r[i] != 0 && !fh_key_in(h->keys, r[i])) {
                h->key_presses[r[i]]++;
            }
        }
        h->mods = mods;
        memcpy(h->keys, &r[2], 6);
    } else if (itf == KVM_HID_ITF_MOUSE && (len == 3 || len == 5)) {
        for (int bit = 0; bit < 5; bit++) {
            if ((r[0] & (1u << bit)) && !(h->buttons & (1u << bit))) {
                h->button_presses[bit]++;
            }
        }
        h->buttons = r[0];
        int dx = (int8_t)r[1], dy = (int8_t)r[2];
        h->sum_dx += dx;
        h->sum_dy += dy;
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        if (ax > h->max_abs_delta) h->max_abs_delta = ax;
        if (ay > h->max_abs_delta) h->max_abs_delta = ay;
        if (len == 5) {
            h->sum_wheel += (int8_t)r[3];
            h->sum_pan += (int8_t)r[4];
        }
    } else if (itf == KVM_HID_ITF_CONSUMER && len == 2) {
        uint16_t u = (uint16_t)(r[0] | (r[1] << 8));
        if (u != 0 && u != h->consumer) {
            h->consumer_presses++;
        }
        h->consumer = u;
    }
}

static inline bool fh_send(void *ctx, uint8_t itf, const uint8_t *report, uint8_t len)
{
    fake_host_t *h = (fake_host_t *)ctx;
    if (itf >= KVM_HID_ITFS || h->refuse[itf]) {
        return false;
    }
    if (h->n < FH_LOG_MAX) {
        h->log[h->n].itf = itf;
        h->log[h->n].len = len;
        memcpy(h->log[h->n].b, report, len);
        h->n++;
    } else {
        h->dropped_log++;
    }
    fh_apply(h, itf, report, len);
    return true;
}

static inline uint8_t fh_protocol(void *ctx, uint8_t itf)
{
    fake_host_t *h = (fake_host_t *)ctx;
    return itf < KVM_HID_ITFS ? h->protocol[itf] : KVM_HID_PROTOCOL_REPORT;
}

static inline void fh_event(void *ctx, const kvm_hid_event_t *ev)
{
    fake_host_t *h = (fake_host_t *)ctx;
    if (h->nev < 256) {
        h->ev[h->nev++] = *ev;
    }
}

static inline void fh_init(fake_host_t *h)
{
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < KVM_HID_ITFS; i++) {
        h->protocol[i] = KVM_HID_PROTOCOL_REPORT;
    }
}

static inline kvm_hid_port_t fh_port(fake_host_t *h)
{
    kvm_hid_port_t p = {.send = fh_send, .protocol = fh_protocol, .event = fh_event, .ctx = h};
    return p;
}

/** The HOST reads every in-flight report, then the core runs; repeated until nothing moves. */
static inline void fh_pump(kvm_hid_core_t *core, fake_host_t *h, uint64_t *now_us)
{
    for (int round = 0; round < 2000; round++) {
        int before = h->n;
        if (!h->no_complete) {
            for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
                if (core->in_flight[i]) {
                    kvm_hid_core_on_report_complete(core, i);
                }
            }
        }
        *now_us += 1000; // 1 ms poll interval of the HOST
        kvm_hid_core_poll(core, *now_us);
        bool busy = false;
        for (uint8_t i = 0; i < KVM_HID_ITFS; i++) {
            busy = busy || core->in_flight[i];
        }
        if (h->n == before && !busy && core->q_count == 0 && !core->chord_active) {
            break;
        }
        if (h->no_complete && h->n == before) {
            break;
        }
    }
}

static inline bool fh_host_neutral(const fake_host_t *h)
{
    static const uint8_t zero[6] = {0};
    return h->mods == 0 && memcmp(h->keys, zero, 6) == 0 && h->buttons == 0 && h->consumer == 0;
}

static inline int fh_count_events(const fake_host_t *h, kvm_hid_event_kind_t kind)
{
    int n = 0;
    for (int i = 0; i < h->nev; i++) {
        n += h->ev[i].kind == kind;
    }
    return n;
}

static inline kvm_hid_intent_t fh_key(kvm_hid_action_type_t t, uint8_t usage)
{
    kvm_hid_intent_t in;
    memset(&in, 0, sizeof(in));
    in.type = t;
    in.data.key.usage_id = usage;
    return in;
}

static inline kvm_hid_intent_t fh_move(int16_t dx, int16_t dy)
{
    kvm_hid_intent_t in;
    memset(&in, 0, sizeof(in));
    in.type = KVM_HID_MOUSE_MOVE;
    in.data.mouse_move.dx = dx;
    in.data.mouse_move.dy = dy;
    return in;
}

static inline kvm_hid_intent_t fh_btn(kvm_hid_action_type_t t, uint8_t button)
{
    kvm_hid_intent_t in;
    memset(&in, 0, sizeof(in));
    in.type = t;
    in.data.mouse_button.button_id = button;
    return in;
}

static inline kvm_hid_intent_t fh_wheel(int16_t h, int16_t v)
{
    kvm_hid_intent_t in;
    memset(&in, 0, sizeof(in));
    in.type = KVM_HID_MOUSE_WHEEL;
    in.data.wheel.horizontal = h;
    in.data.wheel.vertical = v;
    return in;
}

static inline kvm_hid_intent_t fh_chord(uint16_t id)
{
    kvm_hid_intent_t in;
    memset(&in, 0, sizeof(in));
    in.type = KVM_HID_CHORD;
    in.data.chord.chord_id = id;
    return in;
}

static inline kvm_hid_intent_t fh_consumer(kvm_hid_action_type_t t, uint16_t usage)
{
    kvm_hid_intent_t in;
    memset(&in, 0, sizeof(in));
    in.type = t;
    in.data.consumer.usage = usage;
    return in;
}

/** Fresh core, HOST mounted (usb epoch 1), neutral start reports consumed and the log cleared. */
static inline void fh_setup(kvm_hid_core_t *core, fake_host_t *h, uint64_t *now_us)
{
    fh_init(h);
    kvm_hid_port_t port = fh_port(h);
    kvm_hid_core_init(core, &port, NULL);
    *now_us = 1000000;
    kvm_hid_core_set_usb(core, KVM_HOST_USB_MOUNTED, 1, *now_us);
    fh_pump(core, h, now_us);
    h->n = 0;
    h->nev = 0;
}
