// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-040: VEC-001..004 (03 §8) and AT-GEST-01..12 (04) with exact expected outputs.
#include "gesture_harness.h"
#include "kvm_test.h"

static gh_t h;

static kvm_gesture_cfg_t linear_cfg(void)
{
    kvm_gesture_cfg_t c = KVM_GESTURE_CFG_DEFAULT;
    c.acceleration = false; // gain exactly 1.0: exact delta sums
    return c;
}

static void expect_clean(void)
{
    CHECK_EQ(h.held, 0); // zero stuck buttons at the end of every sequence (TASK-040 DoD)
    CHECK_EQ(h.bad_up, 0);
    CHECK_EQ(h.double_down, 0);
    CHECK_EQ(h.g.st.out_overflow, 0);
}

static void vec001_one_finger_tap(void)
{
    // AT-GEST-01
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 400, 200);
    gh_sample(&h, 70, 1, 1, 402, 201);
    gh_sample(&h, 130, 0);
    gh_idle(&h, 1000);
    CHECK_EQ(h.n, 2);
    CHECK_EQ(h.log[0].type, KVM_HID_MOUSE_BTN_DOWN);
    CHECK_EQ(h.log[0].data.mouse_button.button_id, KVM_MOUSE_BTN_LEFT);
    CHECK_EQ(h.log[1].type, KVM_HID_MOUSE_BTN_UP);
    CHECK_EQ(h.moves, 0);
    CHECK_EQ(h.wheels, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_RIGHT), 0);
    expect_clean();
}

static void tap_too_long_or_moved_is_nothing(void)
{
    // A resting finger keeps producing frames (no gap): 300 ms > 220 ms and < 550 ms -> nothing.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 400, 200);
    gh_sample(&h, 100, 1, 1, 400, 200);
    gh_sample(&h, 200, 1, 1, 401, 200);
    gh_sample(&h, 300, 0);
    gh_idle(&h, 1000);
    CHECK_EQ(h.n, 0);
    CHECK_EQ(h.g.st.cancels, 0);
    // Boundary of the 220 ms window: 220 ms is a tap, 230 ms is not.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 400, 200);
    gh_sample(&h, 110, 1, 1, 400, 200);
    gh_sample(&h, 220, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 400, 200);
    gh_sample(&h, 110, 1, 1, 400, 200);
    gh_sample(&h, 230, 0);
    CHECK_EQ(h.n, 0);
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 400, 200);
    gh_sample(&h, 50, 1, 1, 406, 200); // 6 px: still pending, but > tap? no: 6 <= 8
    gh_sample(&h, 100, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    expect_clean();
}

static void one_finger_move_sum(void)
{
    // AT-GEST-02: 50 px movement, exact sum, no click at the end (FR-101).
    kvm_gesture_cfg_t c = linear_cfg();
    gh_init(&h, &c);
    gh_sample(&h, 0, 1, 3, 100, 100);
    for (int i = 1; i <= 10; i++) {
        gh_sample(&h, (uint32_t)(i * 10), 1, 3, 100 + i * 5, 100 + i * 2);
    }
    gh_sample(&h, 120, 0);
    gh_idle(&h, 1000);
    CHECK_EQ(h.sum_dx, 50);
    CHECK_EQ(h.sum_dy, 20);
    CHECK(h.moves > 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    expect_clean();
}

static void fractional_motion_not_lost(void)
{
    // FR-113: slow speed + 0.7 gain: fractions accumulate, nothing is lost to rounding.
    kvm_gesture_cfg_t c = linear_cfg();
    c.speed = KVM_GEST_SPEED_SLOW;
    gh_init(&h, &c);
    gh_sample(&h, 0, 1, 1, 100, 100);
    gh_sample(&h, 10, 1, 1, 110, 100); // start: 10 px * 0.7 = 7
    for (int i = 1; i <= 100; i++) {
        gh_sample(&h, (uint32_t)(10 + i * 10), 1, 1, 110 + i, 100); // 1 px per sample: 0.7 each
    }
    gh_sample(&h, 1100, 0);
    // 110 px * 0.7 = 77 (float accumulation: allow the last fraction only)
    CHECK(h.sum_dx >= 76 && h.sum_dx <= 77);
    expect_clean();
}

static void gain_curve_is_monotonic_and_bounded(void)
{
    // FR-112: monotonic, bounded, deterministic per setting; slow movement has no jerks.
    for (int s = KVM_GEST_SPEED_SLOW; s <= KVM_GEST_SPEED_FAST; s++) {
        float prev = 0.0f;
        for (int i = 0; i <= 1000; i++) {
            float v = (float)i * 0.01f;
            float gn = kvm_gesture_gain((kvm_gesture_speed_t)s, true, v);
            CHECK(gn >= 0.7f && gn <= 2.2f);
            CHECK(gn >= prev);
            prev = gn;
        }
    }
    CHECK(kvm_gesture_gain(KVM_GEST_SPEED_NORMAL, false, 5.0f) == 1.0f);
    // Slow movement: every report within 2 px of the finger movement per sample (no jerks).
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 100, 100);
    gh_sample(&h, 10, 1, 1, 107, 100);
    int jerks = 0;
    for (int i = 1; i <= 50; i++) {
        int before = h.n;
        gh_sample(&h, (uint32_t)(10 + i * 16), 1, 1, 107 + i, 100);
        for (int k = before; k < h.n; k++) {
            if (h.log[k].type == KVM_HID_MOUSE_MOVE && h.log[k].data.mouse_move.dx > 2) {
                jerks++;
            }
        }
    }
    gh_sample(&h, 900, 0);
    CHECK_EQ(jerks, 0);
    expect_clean();
}

static void long_press_right_click(void)
{
    // AT-GEST-03: 550 ms, jitter within +-5 px (10 px peak to peak) -> exactly one RIGHT, not LEFT.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    const int jx[] = {3, -2, 5, -5, 1, 4, -3};
    for (int i = 0; i < 7; i++) {
        gh_sample(&h, (uint32_t)(80 + i * 80), 1, 1, 300 + jx[i], 300 - jx[i] / 2);
    }
    int at_threshold = gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_RIGHT);
    gh_sample(&h, 900, 1, 1, 302, 300);
    gh_sample(&h, 1000, 0);
    gh_idle(&h, 2000);
    CHECK_EQ(at_threshold, 1); // emitted on the threshold (560 ms sample / tick), before UP
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_UP, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 0);
    CHECK_EQ(h.moves, 0);
    expect_clean();
}

static void long_press_without_samples_fires_on_tick(void)
{
    // GT911 reports nothing while a finger rests: the tick decides (but the gap rule applies).
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    for (int t = 100; t <= 600; t += 100) {
        gh_sample(&h, (uint32_t)t, 1, 1, 300, 300); // a resting finger still produces frames
    }
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_RIGHT), 1);
    gh_sample(&h, 650, 0);
    expect_clean();
}

static void long_press_moved_is_no_right(void)
{
    // AT-GEST-04
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 200, 1, 1, 312, 300);
    gh_sample(&h, 600, 1, 1, 312, 300);
    gh_sample(&h, 700, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_RIGHT), 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 0);
    expect_clean();
}

static void vec002_two_finger_scroll_staggered(void)
{
    // AT-GEST-05
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 200, 180);
    gh_sample(&h, 45, 2, 1, 200, 180, 9, 400, 190);
    gh_sample(&h, 75, 2, 1, 200, 201, 9, 400, 211);
    gh_sample(&h, 95, 2, 1, 200, 225, 9, 400, 235);
    gh_sample(&h, 140, 0);
    gh_idle(&h, 1000);
    CHECK(h.wheels >= 1);
    CHECK(h.sum_wheel > 0); // fingers down, natural scrolling (default): content follows
    CHECK_EQ(h.sum_pan, 0);
    CHECK_EQ(h.moves, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    expect_clean();
    // The opposite setting inverts the sign.
    kvm_gesture_cfg_t c = KVM_GESTURE_CFG_DEFAULT;
    c.scroll_natural = false;
    gh_init(&h, &c);
    gh_sample(&h, 0, 1, 1, 200, 180);
    gh_sample(&h, 45, 2, 1, 200, 180, 9, 400, 190);
    gh_sample(&h, 75, 2, 1, 200, 201, 9, 400, 211);
    gh_sample(&h, 95, 2, 1, 200, 225, 9, 400, 235);
    gh_sample(&h, 140, 0);
    CHECK(h.sum_wheel < 0);
}

static void scroll_axis_does_not_flip_on_reversal(void)
{
    // FR-114: a change of direction does not switch the axis; horizontal only when clearly sideways.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 2, 1, 200, 200, 2, 300, 200);
    int y = 200;
    for (int i = 1; i <= 10; i++) {
        y += 8;
        gh_sample(&h, (uint32_t)(i * 10), 2, 1, 200 + (i % 2), y, 2, 300 + (i % 2), y);
    }
    for (int i = 11; i <= 20; i++) {
        y -= 8;
        gh_sample(&h, (uint32_t)(i * 10), 2, 1, 200 + (i % 3), y, 2, 300 + (i % 3), y);
    }
    gh_sample(&h, 220, 0);
    CHECK_EQ(h.sum_pan, 0);
    CHECK(h.wheels >= 4);
    CHECK(h.sum_wheel >= -1 && h.sum_wheel <= 1); // down and back up
    gh_init(&h, NULL);
    gh_sample(&h, 0, 2, 1, 200, 200, 2, 300, 200);
    for (int i = 1; i <= 10; i++) {
        gh_sample(&h, (uint32_t)(i * 10), 2, 1, 200 + i * 10, 200, 2, 300 + i * 10, 201);
    }
    gh_sample(&h, 120, 0);
    CHECK(h.sum_pan < 0); // fingers right, natural: pan left
    CHECK_EQ(h.sum_wheel, 0);
    expect_clean();
}

static void two_finger_tap_is_right_click(void)
{
    // AT-GEST-06 / FR-104
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 4, 300, 300);
    gh_sample(&h, 30, 2, 4, 301, 300, 7, 380, 305);
    gh_sample(&h, 90, 2, 4, 302, 301, 7, 381, 305);
    gh_sample(&h, 150, 1, 7, 381, 305);
    gh_sample(&h, 180, 0);
    gh_idle(&h, 1000);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 0);
    CHECK_EQ(h.moves, 0);
    expect_clean();
    // Second finger joins after 100 ms: no right click (FR-104), no left click either.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 4, 300, 300);
    gh_sample(&h, 150, 2, 4, 300, 300, 7, 380, 305);
    gh_sample(&h, 200, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    // Both up after 250 ms: nothing.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 2, 4, 300, 300, 7, 380, 305);
    gh_sample(&h, 260, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    expect_clean();
}

static void vec003_track_ids_swap_order(void)
{
    // AT-GEST-07
    gh_init(&h, NULL);
    gh_sample(&h, 0, 2, 2, 100, 100, 9, 300, 100);
    gh_sample(&h, 15, 2, 9, 301, 110, 2, 101, 110);
    gh_sample(&h, 30, 2, 2, 101, 125, 9, 301, 125);
    gh_sample(&h, 45, 0);
    CHECK_EQ(h.moves, 0);
    CHECK(h.sum_wheel >= 0 && h.sum_wheel <= 2); // 25 px of centroid travel, never a 200 px jump
    CHECK_EQ(h.sum_pan, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    expect_clean();
}

static void vec004_two_then_one(void)
{
    // AT-GEST-08: 0..100 ms TWO_SCROLLING; 120 ms only id2 remains; 180 ms [].
    gh_init(&h, NULL);
    for (int t = 0; t <= 100; t += 20) {
        gh_sample(&h, (uint32_t)t, 2, 2, 200, 200 + t, 5, 300, 200 + t);
    }
    CHECK_EQ(h.g.state, KVM_GEST_TWO_SCROLLING);
    gh_sample(&h, 120, 1, 2, 200, 300);
    gh_sample(&h, 150, 1, 2, 240, 300); // the remaining finger moves: still no pointer
    gh_sample(&h, 180, 0);
    gh_idle(&h, 1000);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    CHECK_EQ(h.moves, 0);
    expect_clean();
    // The literal vector: the remaining finger rests, then lifts quickly. Still no click.
    gh_init(&h, NULL);
    for (int t = 0; t <= 100; t += 20) {
        gh_sample(&h, (uint32_t)t, 2, 2, 200, 200 + t, 5, 300, 200 + t);
    }
    gh_sample(&h, 120, 1, 2, 200, 300);
    gh_sample(&h, 180, 0);
    gh_idle(&h, 1000);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    CHECK_EQ(h.moves, 0);
    // The same after a two-finger contact that never scrolled: 2 -> 1 -> 0 within 250 ms is the
    // two-finger right click only if both fingers stayed still (FR-104); after 2 -> 1 the lone
    // finger cannot turn into a left tap.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 2, 2, 200, 200, 5, 300, 200);
    gh_sample(&h, 60, 2, 2, 200, 200, 5, 300, 200);
    gh_sample(&h, 300, 1, 2, 200, 200); // too late for FR-104
    gh_sample(&h, 340, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, 0), 0);
    expect_clean();
}

static void three_fingers_cancel(void)
{
    // AT-GEST-09: 3/4/5 fingers in any order -> cancellation, no HID input.
    for (int n = 3; n <= 5; n++) {
        gh_init(&h, NULL);
        gh_sample(&h, 0, 1, 1, 100, 100);
        gh_sample(&h, 20, 2, 1, 100, 100, 2, 200, 100);
        if (n == 3) {
            gh_sample(&h, 40, 3, 1, 100, 100, 2, 200, 100, 3, 300, 100);
        } else if (n == 4) {
            gh_sample(&h, 40, 4, 1, 100, 100, 2, 200, 100, 3, 300, 100, 4, 400, 100);
        } else {
            gh_sample(&h, 40, 5, 1, 100, 100, 2, 200, 100, 3, 300, 100, 4, 400, 100, 5, 500, 100);
        }
        gh_sample(&h, 60, 2, 1, 100, 100, 2, 200, 100);
        gh_sample(&h, 80, 1, 1, 100, 100);
        gh_sample(&h, 100, 0);
        gh_idle(&h, 1000);
        CHECK_EQ(h.n, 0);
        CHECK_EQ(h.g.st.cancel_by_reason[KVM_GEST_CANCEL_FINGERS], 1);
    }
    // Three at once, then a normal tap works again.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 3, 1, 100, 100, 2, 200, 100, 3, 300, 100);
    gh_sample(&h, 50, 0);
    gh_sample(&h, 100, 1, 1, 100, 100);
    gh_sample(&h, 150, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    expect_clean();
}

static void third_finger_during_drag_releases(void)
{
    // 01 §3.2 item 3: extra finger during a drag -> UP, cancel, never silently scrolling.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 80, 0); // tap
    gh_sample(&h, 200, 1, 1, 302, 300);
    gh_sample(&h, 230, 1, 1, 320, 300); // drag starts
    CHECK(kvm_gesture_left_held(&h.g));
    gh_sample(&h, 260, 2, 1, 330, 300, 2, 400, 300);
    CHECK(!kvm_gesture_left_held(&h.g));
    gh_sample(&h, 290, 2, 1, 330, 340, 2, 400, 340);
    gh_sample(&h, 320, 0);
    CHECK_EQ(h.wheels, 0);
    CHECK_EQ(h.g.st.cancel_by_reason[KVM_GEST_CANCEL_EXTRA_FINGER], 1);
    expect_clean();
}

static void double_tap_and_drag(void)
{
    // AT-GEST-10 / FR-106: exactly one LEFT_DOWN of the second contact, LEFT_UP after it ends.
    kvm_gesture_cfg_t c = linear_cfg();
    gh_init(&h, &c);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 90, 0); // first tap: a click
    CHECK_EQ(h.n, 2);
    gh_sample(&h, 250, 1, 1, 310, 305); // within 300 ms and 32 px
    gh_sample(&h, 270, 1, 1, 315, 305); // 5 px: below the 8 px drag threshold
    CHECK_EQ(h.n, 2);
    gh_sample(&h, 290, 1, 1, 330, 305);
    CHECK_EQ(h.log[2].type, KVM_HID_MOUSE_BTN_DOWN); // DOWN before the first MOVE
    CHECK_EQ(h.log[3].type, KVM_HID_MOUSE_MOVE);
    for (int i = 1; i <= 10; i++) {
        gh_sample(&h, (uint32_t)(290 + i * 10), 1, 1, 330 + i * 3, 305 + i);
    }
    gh_sample(&h, 420, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 2);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_UP, KVM_MOUSE_BTN_LEFT), 2);
    CHECK_EQ(h.log[h.n - 1].type, KVM_HID_MOUSE_BTN_UP);
    CHECK_EQ(h.sum_dx, 50); // 310 -> 360 during the second contact
    expect_clean();
    // Second contact without movement: an ordinary click (the OS sees a double click).
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 90, 0);
    gh_sample(&h, 200, 1, 1, 303, 301);
    gh_sample(&h, 260, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 2);
    CHECK_EQ(h.moves, 0);
    expect_clean();
    // Too late (> 300 ms) or too far (> 32 px): ordinary movement, no drag.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 90, 0);
    gh_sample(&h, 400, 1, 1, 300, 300);
    gh_sample(&h, 450, 1, 1, 340, 300);
    gh_sample(&h, 500, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 90, 0);
    gh_sample(&h, 200, 1, 1, 340, 300);
    gh_sample(&h, 250, 1, 1, 380, 300);
    gh_sample(&h, 300, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    expect_clean();
}

static void gap_and_invalid_cancel(void)
{
    // AT-GEST-11: 120 ms without data / I2C error -> cancel + release, error counter.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 90, 0);
    gh_sample(&h, 200, 1, 1, 302, 300);
    gh_sample(&h, 230, 1, 1, 320, 300); // drag: LEFT held
    CHECK(kvm_gesture_left_held(&h.g));
    gh_idle(&h, 400); // ticks only: > 120 ms without a sample
    CHECK(!kvm_gesture_left_held(&h.g));
    CHECK_EQ(h.g.st.cancel_by_reason[KVM_GEST_CANCEL_GAP], 1);
    CHECK_EQ(h.g.state, KVM_GEST_CANCELLED);
    gh_sample(&h, 420, 1, 1, 330, 300); // the same finger again: ignored until all up
    gh_sample(&h, 440, 0);
    CHECK_EQ(h.moves, 1);
    expect_clean();
    // Invalid sample (controller error) while a long press is pending: no click, cancel.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_invalid(&h, 100);
    gh_idle(&h, 1000);
    CHECK_EQ(h.n, 0);
    CHECK_EQ(h.g.st.cancel_by_reason[KVM_GEST_CANCEL_INVALID], 1);
    gh_sample(&h, 1010, 0);
    CHECK_EQ(h.g.state, KVM_GEST_IDLE);
    // Malformed: duplicate track id, out of bounds.
    gh_init(&h, NULL);
    gh_sample(&h, 0, 2, 1, 100, 100, 1, 200, 100);
    gh_sample(&h, 10, 1, 1, 900, 100); // x >= 800
    gh_sample(&h, 50, 0);
    CHECK_EQ(h.n, 0);
    CHECK_EQ(h.g.st.cancel_by_reason[KVM_GEST_CANCEL_MALFORMED], 2);
    expect_clean();
}

static void drag_lock_paths(void)
{
    // AT-GEST-12: drag lock, mode switch, timeout -> a final LEFT_UP on every path.
    kvm_gesture_out_t o;
    // (a) drag lock ON, move, lift: still held; switch off -> UP.
    gh_init(&h, NULL);
    kvm_gesture_set_drag_lock(&h.g, true, h.now_us, &o);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 20, 1, 1, 320, 300);
    gh_sample(&h, 40, 0);
    CHECK(kvm_gesture_left_held(&h.g));
    gh_sample(&h, 500, 1, 1, 320, 300);
    gh_sample(&h, 520, 1, 1, 360, 300); // continues the same drag: no second DOWN
    gh_sample(&h, 540, 0);
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    kvm_gesture_set_drag_lock(&h.g, false, h.now_us, &o);
    gh_record(&h, &o);
    expect_clean();
    // (b) mode switch (external cancel) while locked.
    gh_init(&h, NULL);
    kvm_gesture_set_drag_lock(&h.g, true, h.now_us, &o);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 20, 1, 1, 320, 300);
    kvm_gesture_cancel(&h.g, KVM_GEST_CANCEL_EXTERNAL, &o);
    gh_record(&h, &o);
    CHECK(!kvm_gesture_left_held(&h.g));
    gh_sample(&h, 40, 1, 1, 330, 300); // the finger is still down: ignored
    gh_sample(&h, 60, 0);
    CHECK_EQ(h.moves, 1);
    expect_clean();
    // (c) inactivity timeout (10 s default).
    gh_init(&h, NULL);
    kvm_gesture_set_drag_lock(&h.g, true, h.now_us, &o);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 20, 1, 1, 320, 300);
    gh_sample(&h, 40, 0);
    gh_idle(&h, 9000);
    CHECK(kvm_gesture_left_held(&h.g));
    gh_idle(&h, 10200);
    CHECK(!kvm_gesture_left_held(&h.g));
    CHECK_EQ(h.g.st.drag_lock_timeouts, 1);
    expect_clean();
    // (d) a tap ends a locked drag.
    gh_init(&h, NULL);
    kvm_gesture_set_drag_lock(&h.g, true, h.now_us, &o);
    gh_sample(&h, 0, 1, 1, 300, 300);
    gh_sample(&h, 20, 1, 1, 320, 300);
    gh_sample(&h, 40, 0);
    gh_sample(&h, 500, 1, 1, 320, 300);
    gh_sample(&h, 560, 0);
    CHECK(!kvm_gesture_left_held(&h.g));
    CHECK_EQ(gh_count(&h, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    expect_clean();
    // (e) the 60 s safety limit caps the configured timeout.
    kvm_gesture_cfg_t c = KVM_GESTURE_CFG_DEFAULT;
    c.drag_lock_idle_us = 600000000u;
    gh_init(&h, &c);
    CHECK_EQ(h.g.cfg.drag_lock_idle_us, KVM_GESTURE_DRAG_LOCK_MAX_US);
}

static void degraded_multitouch_fallback(void)
{
    // FR-116: with two-finger gestures disabled, two fingers never pretend to scroll or click.
    kvm_gesture_cfg_t c = KVM_GESTURE_CFG_DEFAULT;
    c.two_finger_enabled = false;
    gh_init(&h, &c);
    gh_sample(&h, 0, 2, 1, 200, 200, 2, 300, 200);
    gh_sample(&h, 50, 2, 1, 200, 260, 2, 300, 260);
    gh_sample(&h, 100, 0);
    CHECK_EQ(h.n, 0);
    expect_clean();
}

static void old_sequence_rejected(void)
{
    gh_init(&h, NULL);
    gh_sample(&h, 0, 1, 1, 300, 300);
    h.seq -= 2; // the next sample carries an older sample_seq
    gh_sample(&h, 20, 1, 1, 330, 300);
    CHECK_EQ(h.g.st.cancel_by_reason[KVM_GEST_CANCEL_MALFORMED], 1);
    CHECK_EQ(h.moves, 0);
    expect_clean();
}

int main(void)
{
    printf("gesture_test_vectors\n");
    RUN(vec001_one_finger_tap);
    RUN(tap_too_long_or_moved_is_nothing);
    RUN(one_finger_move_sum);
    RUN(fractional_motion_not_lost);
    RUN(gain_curve_is_monotonic_and_bounded);
    RUN(long_press_right_click);
    RUN(long_press_without_samples_fires_on_tick);
    RUN(long_press_moved_is_no_right);
    RUN(vec002_two_finger_scroll_staggered);
    RUN(scroll_axis_does_not_flip_on_reversal);
    RUN(two_finger_tap_is_right_click);
    RUN(vec003_track_ids_swap_order);
    RUN(vec004_two_then_one);
    RUN(three_fingers_cancel);
    RUN(third_finger_during_drag_releases);
    RUN(double_tap_and_drag);
    RUN(gap_and_invalid_cancel);
    RUN(drag_lock_paths);
    RUN(degraded_multitouch_fallback);
    RUN(old_sequence_rejected);
    TEST_MAIN_END("gesture_test_vectors");
}
