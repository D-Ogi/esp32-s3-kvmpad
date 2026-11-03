// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-040 property test: >= 100k random touch sequences (fingers 0..5, track id reuse and
// reordering, jitter, gaps, invalid frames, external cancels, drag lock toggles). Invariants per
// sequence: no stuck button at the end, no UP without DOWN, no double DOWN, never a click together
// with a scroll, no pointer movement in a scroll, nothing after 3+ fingers until all are up.
// The seed is printed; pass one as argv[1] to reproduce.
#include <inttypes.h>
#include <stdlib.h>

#include "gesture_harness.h"
#include "kvm_test.h"

static gh_t h;
static uint64_t rng;

static uint32_t rnd(void)
{
    uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 32);
}

static uint32_t below(uint32_t n)
{
    return rnd() % n;
}

typedef struct {
    int id, x, y;
} finger_t;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int main(int argc, char **argv)
{
    uint64_t seed = 0x6765737475726573ull; // "gestures"
    int sequences = 100000;
    if (argc > 1) {
        seed = strtoull(argv[1], NULL, 0);
    }
    if (argc > 2) {
        sequences = atoi(argv[2]);
    }
    printf("gesture_test_property seed=0x%" PRIx64 " sequences=%d\n", seed, sequences);
    rng = seed;
    kvm_gesture_cfg_t cfg = KVM_GESTURE_CFG_DEFAULT;
    gh_init(&h, &cfg);
    int stuck = 0, click_and_scroll = 0, move_in_scroll = 0, after_three = 0;
    long total_events = 0;
    uint32_t t = 0;
    for (int seqn = 0; seqn < sequences; seqn++) {
        if (h.n > GH_LOG_MAX - 512) {
            h.n = 0; // keep the log bounded; the HOST model continues
        }
        const int start = h.n;
        finger_t f[5];
        int nf = 0, next_id = (int)below(10);
        int steps = 1 + (int)below(40);
        kvm_gesture_out_t o;
        if (below(20) == 0) {
            kvm_gesture_set_drag_lock(&h.g, below(2), h.now_us, &o);
            gh_record(&h, &o);
        }
        for (int s = 0; s < steps; s++) {
            uint32_t r = below(100);
            if (r < 15 && nf < 5) {
                f[nf].id = next_id++ % 16;
                for (int k = 0; k < nf; k++) {
                    if (f[k].id == f[nf].id) {
                        f[nf].id = (f[nf].id + 7) % 16; // unique within the sample
                    }
                }
                f[nf].x = (int)below(cfg.h_res);
                f[nf].y = (int)below(cfg.v_res);
                nf++;
            } else if (r < 25 && nf > 0) {
                int k = (int)below((uint32_t)nf);
                f[k] = f[nf - 1];
                nf--;
            } else if (r < 85) {
                int amp = below(3) == 0 ? 40 : 4;
                for (int k = 0; k < nf; k++) {
                    f[k].x = clampi(f[k].x + (int)below((uint32_t)(2 * amp + 1)) - amp, 0, cfg.h_res - 1);
                    f[k].y = clampi(f[k].y + (int)below((uint32_t)(2 * amp + 1)) - amp, 0, cfg.v_res - 1);
                }
            } else if (r < 88) {
                kvm_gesture_cancel(&h.g, KVM_GEST_CANCEL_EXTERNAL, &o);
                gh_record(&h, &o);
            } else if (r < 90) {
                t += 1 + below(30);
                gh_invalid(&h, t);
                continue;
            }
            uint32_t dt = below(10) == 0 ? 100 + below(200) : 5 + below(25); // occasional gaps
            t += dt;
            // Random array order: the engine must follow track ids, not positions.
            finger_t tmp[5];
            for (int k = 0; k < nf; k++) {
                tmp[k] = f[k];
            }
            for (int k = nf - 1; k > 0; k--) {
                int j = (int)below((uint32_t)(k + 1));
                finger_t x = tmp[k];
                tmp[k] = tmp[j];
                tmp[j] = x;
            }
            switch (nf) {
            case 0: gh_sample(&h, t, 0); break;
            case 1: gh_sample(&h, t, 1, tmp[0].id, tmp[0].x, tmp[0].y); break;
            case 2: gh_sample(&h, t, 2, tmp[0].id, tmp[0].x, tmp[0].y, tmp[1].id, tmp[1].x, tmp[1].y); break;
            case 3:
                gh_sample(&h, t, 3, tmp[0].id, tmp[0].x, tmp[0].y, tmp[1].id, tmp[1].x, tmp[1].y, tmp[2].id, tmp[2].x,
                          tmp[2].y);
                break;
            case 4:
                gh_sample(&h, t, 4, tmp[0].id, tmp[0].x, tmp[0].y, tmp[1].id, tmp[1].x, tmp[1].y, tmp[2].id, tmp[2].x,
                          tmp[2].y, tmp[3].id, tmp[3].x, tmp[3].y);
                break;
            default:
                gh_sample(&h, t, 5, tmp[0].id, tmp[0].x, tmp[0].y, tmp[1].id, tmp[1].x, tmp[1].y, tmp[2].id, tmp[2].x,
                          tmp[2].y, tmp[3].id, tmp[3].x, tmp[3].y, tmp[4].id, tmp[4].x, tmp[4].y);
                break;
            }
        }
        // End of sequence: all fingers up, then enough idle time for every timer (drag lock 10 s).
        t += 10 + below(40);
        gh_sample(&h, t, 0);
        t += 11000;
        gh_idle(&h, t);
        kvm_gesture_set_drag_lock(&h.g, false, h.now_us, &o);
        gh_record(&h, &o);
        // ---- invariants of this free-form sequence (several gestures may follow each other) ----
        // No stuck button after all-up + timers; UP-without-DOWN and double DOWN are counted by the
        // harness for the whole run. Per-gesture rules are checked in the single-gesture loop below.
        stuck += h.held != 0;
        total_events += h.n - start;
    }
    // Single-gesture property: one landing, random motion (optionally a 3rd finger), one lift.
    // Here a click together with a scroll, a pointer move in a 2-finger gesture, or any event after
    // the 3rd finger other than a release is a violation.
    for (int k = 0; k < sequences / 10; k++) {
        const int start = h.n;
        if (h.n > GH_LOG_MAX - 512) {
            h.n = 0;
            continue;
        }
        int two = (int)below(2);
        int x = 100 + (int)below(500), y = 100 + (int)below(300);
        t += 400; // outside the double-tap window
        if (two) {
            gh_sample(&h, t, 2, 1, x, y, 2, x + 80, y);
        } else {
            gh_sample(&h, t, 1, 1, x, y);
        }
        int steps = (int)below(30);
        bool three = false;
        int after3 = -1;
        for (int s = 0; s < steps; s++) {
            t += 5 + below(20);
            x = clampi(x + (int)below(21) - 10, 0, 700);
            y = clampi(y + (int)below(21) - 10, 0, 470);
            if (below(25) == 0) {
                three = true;
            }
            if (three) {
                if (after3 < 0) {
                    after3 = h.n;
                }
                gh_sample(&h, t, 3, 1, x, y, 2, x + 80, y, 3, x + 90, y + 5);
            } else if (two) {
                gh_sample(&h, t, 2, 1, x, y, 2, x + 80, y);
            } else {
                gh_sample(&h, t, 1, 1, x, y);
            }
        }
        t += 10;
        gh_sample(&h, t, 0);
        t += 1000;
        gh_idle(&h, t);
        int downs = 0, wheels = 0, moves = 0;
        for (int i = start; i < h.n; i++) {
            downs += h.log[i].type == KVM_HID_MOUSE_BTN_DOWN;
            wheels += h.log[i].type == KVM_HID_MOUSE_WHEEL;
            moves += h.log[i].type == KVM_HID_MOUSE_MOVE;
        }
        click_and_scroll += downs > 0 && wheels > 0;
        move_in_scroll += two && moves > 0;
        if (after3 >= 0) {
            for (int i = after3; i < h.n; i++) {
                after_three += h.log[i].type != KVM_HID_MOUSE_BTN_UP;
            }
        }
        stuck += h.held != 0;
    }
    CHECK_EQ(stuck, 0);
    CHECK_EQ(h.bad_up, 0);
    CHECK_EQ(h.double_down, 0);
    CHECK_EQ(click_and_scroll, 0);
    CHECK_EQ(move_in_scroll, 0);
    CHECK_EQ(after_three, 0);
    CHECK_EQ(h.g.st.out_overflow, 0);
    const kvm_gesture_stats_t *st = &h.g.st;
    printf("  samples %" PRIu32 ", events %ld, taps %" PRIu32 ", right %" PRIu32 ", drags %" PRIu32 ", scrolls %" PRIu32
           ", cancels %" PRIu32 " (invalid %" PRIu32 ", malformed %" PRIu32 ", gap %" PRIu32 ", fingers %" PRIu32
           ", extra %" PRIu32 ", external %" PRIu32 "), drag-lock timeouts %" PRIu32 "\n",
           st->samples, total_events, st->taps, st->right_clicks, st->drags, st->scrolls, st->cancels,
           st->cancel_by_reason[KVM_GEST_CANCEL_INVALID], st->cancel_by_reason[KVM_GEST_CANCEL_MALFORMED],
           st->cancel_by_reason[KVM_GEST_CANCEL_GAP], st->cancel_by_reason[KVM_GEST_CANCEL_FINGERS],
           st->cancel_by_reason[KVM_GEST_CANCEL_EXTRA_FINGER], st->cancel_by_reason[KVM_GEST_CANCEL_EXTERNAL],
           st->drag_lock_timeouts);
    TEST_MAIN_END("gesture_test_property");
}
