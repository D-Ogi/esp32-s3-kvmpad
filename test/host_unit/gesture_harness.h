// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Gesture test harness: builds samples, feeds the engine with a 10 ms tick between samples and
// records every emitted intent together with a model of the buttons held on the HOST.
#pragma once

#include <stdarg.h>
#include <string.h>

#include "gesture_engine.h"

#define GH_LOG_MAX 8192

typedef struct {
    kvm_gesture_t g;
    kvm_hid_intent_t log[GH_LOG_MAX];
    int n;
    uint32_t seq;
    uint64_t now_us;
    // HOST model from the emitted intents
    uint8_t held;          // button bits
    int downs[4], ups[4];  // per button id
    int bad_up;            // UP without DOWN
    int double_down;       // DOWN while already down
    long sum_dx, sum_dy, sum_wheel, sum_pan;
    int moves, wheels;
} gh_t;

static inline void gh_record(gh_t *h, const kvm_gesture_out_t *o)
{
    for (int i = 0; i < o->n; i++) {
        const kvm_hid_intent_t *in = &o->ev[i];
        if (h->n < GH_LOG_MAX) {
            h->log[h->n++] = *in;
        }
        uint8_t b = in->data.mouse_button.button_id;
        switch (in->type) {
        case KVM_HID_MOUSE_BTN_DOWN:
            if (h->held & (1u << b)) {
                h->double_down++;
            }
            h->held = (uint8_t)(h->held | (1u << b));
            h->downs[b & 3]++;
            break;
        case KVM_HID_MOUSE_BTN_UP:
            if (!(h->held & (1u << b))) {
                h->bad_up++;
            }
            h->held = (uint8_t)(h->held & ~(1u << b));
            h->ups[b & 3]++;
            break;
        case KVM_HID_MOUSE_MOVE:
            h->sum_dx += in->data.mouse_move.dx;
            h->sum_dy += in->data.mouse_move.dy;
            h->moves++;
            break;
        case KVM_HID_MOUSE_WHEEL:
            h->sum_wheel += in->data.wheel.vertical;
            h->sum_pan += in->data.wheel.horizontal;
            h->wheels++;
            break;
        default:
            break;
        }
    }
}

static inline void gh_init(gh_t *h, const kvm_gesture_cfg_t *cfg)
{
    memset(h, 0, sizeof(*h));
    kvm_gesture_init(&h->g, cfg);
    h->now_us = 1000000;
}

/** Ticks every 10 ms from the current time up to `t_us` (exclusive). */
static inline void gh_tick_to(gh_t *h, uint64_t t_us)
{
    kvm_gesture_out_t o;
    while (h->now_us + 10000 < t_us) {
        h->now_us += 10000;
        kvm_gesture_tick(&h->g, h->now_us, &o);
        gh_record(h, &o);
    }
}

/**
 * Feed a sample at `t_ms` (relative to 1 s) with `n` points given as id, x, y triplets.
 * Ticks run in between, like the firmware's 10 ms timer.
 */
static inline void gh_sample(gh_t *h, uint32_t t_ms, int n, ...)
{
    kvm_touch_sample_t s;
    memset(&s, 0, sizeof(s));
    s.timestamp_us = 1000000u + (uint64_t)t_ms * 1000u;
    gh_tick_to(h, s.timestamp_us);
    s.sample_seq = ++h->seq;
    s.valid = true;
    s.point_count = (uint8_t)n;
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && i < KVM_TOUCH_MAX_POINTS; i++) {
        s.point[i].track_id = (uint8_t)va_arg(ap, int);
        s.point[i].x_px = (int16_t)va_arg(ap, int);
        s.point[i].y_px = (int16_t)va_arg(ap, int);
    }
    va_end(ap);
    kvm_gesture_out_t o;
    kvm_gesture_feed(&h->g, &s, &o);
    gh_record(h, &o);
    if (s.timestamp_us > h->now_us) {
        h->now_us = s.timestamp_us;
    }
}

static inline void gh_invalid(gh_t *h, uint32_t t_ms)
{
    kvm_touch_sample_t s;
    memset(&s, 0, sizeof(s));
    s.timestamp_us = 1000000u + (uint64_t)t_ms * 1000u;
    gh_tick_to(h, s.timestamp_us);
    s.sample_seq = ++h->seq;
    s.valid = false;
    kvm_gesture_out_t o;
    kvm_gesture_feed(&h->g, &s, &o);
    gh_record(h, &o);
    h->now_us = s.timestamp_us;
}

/** Let time pass with ticks only (no samples). */
static inline void gh_idle(gh_t *h, uint32_t until_ms)
{
    gh_tick_to(h, 1000000u + (uint64_t)until_ms * 1000u);
}

static inline int gh_count(const gh_t *h, kvm_hid_action_type_t t, uint8_t button)
{
    int c = 0;
    for (int i = 0; i < h->n; i++) {
        if (h->log[i].type == t &&
            (button == 0 || (t != KVM_HID_MOUSE_BTN_DOWN && t != KVM_HID_MOUSE_BTN_UP) ||
             h->log[i].data.mouse_button.button_id == button)) {
            c++;
        }
    }
    return c;
}
