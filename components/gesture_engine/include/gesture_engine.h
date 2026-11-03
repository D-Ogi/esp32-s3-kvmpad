// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Deterministic gesture engine (TASK-040; 01 §3 FR-101..116, 03 §2/§2.1, VEC-001..004, ADR-007).
// One instance per physical panel. Input: kvm_touch_sample_t of the touchpad area plus a periodic
// tick; output: kvm_hid_intent_t mouse intents (LOCAL) for InputArbiter. Pure C: no RTOS, no I/O,
// no allocation; time is the sample timestamp / the tick argument (TECH-006).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kvm_hid_intent.h"
#include "kvm_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_GEST_IDLE = 0,
    KVM_GEST_ONE_PENDING,
    KVM_GEST_ONE_MOVING,
    KVM_GEST_RIGHT_FIRED,   // long press fired: nothing more until every finger is up
    KVM_GEST_TWO_PENDING,
    KVM_GEST_TWO_SCROLLING,
    KVM_GEST_WAIT_RELEASE,  // gesture finished (scroll 2 -> 1, extra finger); waits for touch_count = 0
    KVM_GEST_CANCELLED,     // invalid / gap / 3+ fingers / external cancel; waits for touch_count = 0
} kvm_gesture_state_t;

typedef enum { KVM_GEST_SPEED_SLOW = 0, KVM_GEST_SPEED_NORMAL, KVM_GEST_SPEED_FAST } kvm_gesture_speed_t;

typedef enum {
    KVM_GEST_CANCEL_INVALID = 0, // valid = false (controller error)
    KVM_GEST_CANCEL_MALFORMED,   // count > 5, out of bounds, duplicate track id, older sample_seq
    KVM_GEST_CANCEL_GAP,         // > gap_cancel_us without a sample while touching (TECH-064)
    KVM_GEST_CANCEL_FINGERS,     // 3+ fingers (FR-109)
    KVM_GEST_CANCEL_EXTRA_FINGER,// extra finger during a 1-finger movement or drag (01 §3.2 item 3)
    KVM_GEST_CANCEL_EXTERNAL,    // mode switch, owner lost, navigation (FR-110)
    KVM_GEST_CANCEL_COUNT,
} kvm_gesture_cancel_t;

/** Thresholds: 01 §3.1 starting UX values (not hardware facts; tuned only through an ADR). */
typedef struct {
    uint16_t h_res, v_res;           // coordinate bounds of the samples
    uint16_t move_start_px;          // 6: ONE_PENDING -> ONE_MOVING
    uint16_t tap_max_move_px;        // 8: FR-102
    uint16_t long_press_max_move_px; // 10: FR-103
    uint16_t two_max_move_px;        // 10: FR-104 per finger
    uint16_t scroll_start_px;        // 10: FR-105 centroid travel
    uint16_t dtd_max_dist_px;        // 32: double-tap-and-drag second contact distance
    uint16_t dtd_move_px;            // 8: FR-106 second contact movement
    uint32_t tap_max_us;             // 220 ms
    uint32_t long_press_us;          // 550 ms
    uint32_t two_join_us;            // 100 ms: the second finger joins within
    uint32_t two_tap_max_us;         // 250 ms: both up within (from the first contact)
    uint32_t dtd_window_us;          // 300 ms: tap up -> next contact
    uint32_t gap_cancel_us;          // 120 ms (03 §2, TECH-064)
    uint32_t drag_lock_idle_us;      // 10 s, capped at 60 s (FR-115)
    uint16_t scroll_px_per_step;     // centroid travel per wheel step
    bool scroll_natural;             // true: fingers down -> content follows (wheel +)
    bool two_finger_enabled;         // false = degraded multi-touch (FR-116): 2 fingers cancel
    kvm_gesture_speed_t speed;       // FR-112 Slow / Normal / Fast
    bool acceleration;               // FR-112 speed curve; false = constant gain
} kvm_gesture_cfg_t;

#define KVM_GESTURE_CFG_DEFAULT                                                                              \
    {                                                                                                        \
        .h_res = 800, .v_res = 480, .move_start_px = 6, .tap_max_move_px = 8, .long_press_max_move_px = 10, \
        .two_max_move_px = 10, .scroll_start_px = 10, .dtd_max_dist_px = 32, .dtd_move_px = 8,               \
        .tap_max_us = 220000u, .long_press_us = 550000u, .two_join_us = 100000u, .two_tap_max_us = 250000u,  \
        .dtd_window_us = 300000u, .gap_cancel_us = 120000u, .drag_lock_idle_us = 10000000u,                  \
        .scroll_px_per_step = 24, .scroll_natural = true, .two_finger_enabled = true,                        \
        .speed = KVM_GEST_SPEED_NORMAL, .acceleration = true,                                                \
    }

#define KVM_GESTURE_MAX_OUT 6
#define KVM_GESTURE_DRAG_LOCK_MAX_US 60000000u

typedef struct {
    kvm_hid_intent_t ev[KVM_GESTURE_MAX_OUT];
    uint8_t n;
} kvm_gesture_out_t;

typedef struct {
    uint32_t samples;
    uint32_t taps;
    uint32_t right_clicks;
    uint32_t drags;
    uint32_t scrolls;
    uint32_t moves;
    uint32_t cancels;
    uint32_t cancel_by_reason[KVM_GEST_CANCEL_COUNT];
    uint32_t drag_lock_timeouts;
    uint32_t out_overflow; // must stay 0
} kvm_gesture_stats_t;

typedef struct {
    int16_t x, y;
    uint8_t id;
} kvm_gesture_pt_t;

typedef struct {
    kvm_gesture_cfg_t cfg;
    kvm_gesture_state_t state;
    bool have_seq;
    uint32_t last_seq;
    uint64_t last_sample_us;
    uint8_t last_count;

    // current sequence
    uint64_t start_us;          // first contact of the sequence
    kvm_gesture_pt_t a0, a;     // primary finger: start and last position
    kvm_gesture_pt_t b0, b;     // second finger (TWO_*)
    uint64_t join_us;           // when the second finger arrived
    bool b_present;
    uint16_t max_a, max_b;      // max displacement from the start (px, Chebyshev)
    bool two_tap_ok;            // still eligible for the 2-finger right click
    int32_t cx0, cy0;           // centroid at TWO_PENDING start / last centroid (x2 for half pixels)
    int32_t cx, cy;
    float accx, accy;           // FR-113 fractional pointer accumulators
    float sx, sy;               // FR-114 fractional scroll accumulators
    int8_t scroll_axis;         // 0 none, 1 vertical, 2 horizontal
    float axis_travel, cross_travel;

    // double-tap-and-drag tracker
    bool tap_valid;
    uint64_t tap_up_us;
    kvm_gesture_pt_t tap_pt;
    bool dtd_armed;

    // buttons this engine holds on the HOST (it must always release them)
    bool left_held;
    bool drag_lock;             // UI toggle (FR-107)
    uint64_t last_activity_us;  // drag-lock inactivity timer

    kvm_gesture_stats_t st;
} kvm_gesture_t;

void kvm_gesture_init(kvm_gesture_t *g, const kvm_gesture_cfg_t *cfg);

/** Feed one sample (empty frames included: point_count = 0 is the release information). */
void kvm_gesture_feed(kvm_gesture_t *g, const kvm_touch_sample_t *s, kvm_gesture_out_t *out);

/** Time without a sample: long press, gap cancel (> 120 ms), drag-lock inactivity. Call every ~10 ms. */
void kvm_gesture_tick(kvm_gesture_t *g, uint64_t now_us, kvm_gesture_out_t *out);

/** Mode switch, owner lost, navigation (FR-110): release what is held and wait for all fingers up. */
void kvm_gesture_cancel(kvm_gesture_t *g, kvm_gesture_cancel_t reason, kvm_gesture_out_t *out);

/** Drag lock toggle (FR-107/115). Turning it off releases LEFT if held. */
void kvm_gesture_set_drag_lock(kvm_gesture_t *g, bool on, uint64_t now_us, kvm_gesture_out_t *out);

/** FR-112 sensitivity curve: monotonic, bounded to [0.7, 2.2]. `speed_px_per_ms` >= 0. */
float kvm_gesture_gain(kvm_gesture_speed_t speed, bool acceleration, float speed_px_per_ms);

static inline bool kvm_gesture_left_held(const kvm_gesture_t *g)
{
    return g->left_held;
}

const char *kvm_gesture_state_name(kvm_gesture_state_t s);

#ifdef __cplusplus
}
#endif
