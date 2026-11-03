// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Gesture classifier of 03 §2.1 with the 01 §3.2 ambiguity rules. Decisions:
// - a click is emitted only once the classification is final (FR-102/103/104), as DOWN then UP;
// - fingers are followed by track_id, never by array position (VEC-003, 01 §3.2 item 2);
// - every path that leaves the engine with LEFT held (double-tap-and-drag, drag lock) has a release:
//   finger up, extra finger, cancel, gap, drag-lock off or inactivity (FR-110, FR-115, AT-GEST-12).
#include "gesture_engine.h"

#include <math.h>
#include <string.h>

// ---- output --------------------------------------------------------------------------------
static void emit(kvm_gesture_t *g, kvm_gesture_out_t *out, kvm_hid_action_type_t type, int16_t a, int16_t b)
{
    if (out->n >= KVM_GESTURE_MAX_OUT) {
        g->st.out_overflow++;
        return;
    }
    kvm_hid_intent_t *in = &out->ev[out->n++];
    memset(in, 0, sizeof(*in));
    in->type = type;
    in->origin = KVM_ORIGIN_LOCAL;
    in->timestamp_us = g->last_sample_us;
    switch (type) {
    case KVM_HID_MOUSE_MOVE:
        in->data.mouse_move.dx = a;
        in->data.mouse_move.dy = b;
        break;
    case KVM_HID_MOUSE_WHEEL:
        in->data.wheel.horizontal = a;
        in->data.wheel.vertical = b;
        break;
    default:
        in->data.mouse_button.button_id = (uint8_t)a;
        break;
    }
}

static void click(kvm_gesture_t *g, kvm_gesture_out_t *out, uint8_t button)
{
    emit(g, out, KVM_HID_MOUSE_BTN_DOWN, button, 0);
    emit(g, out, KVM_HID_MOUSE_BTN_UP, button, 0);
}

static void press_left(kvm_gesture_t *g, kvm_gesture_out_t *out)
{
    if (!g->left_held) {
        emit(g, out, KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT, 0);
        g->left_held = true;
        g->st.drags++;
    }
}

static void release_left(kvm_gesture_t *g, kvm_gesture_out_t *out)
{
    if (g->left_held) {
        emit(g, out, KVM_HID_MOUSE_BTN_UP, KVM_MOUSE_BTN_LEFT, 0);
        g->left_held = false;
    }
}

// ---- geometry ------------------------------------------------------------------------------
static int find(const kvm_touch_sample_t *s, uint8_t id)
{
    for (int i = 0; i < s->point_count; i++) {
        if (s->point[i].track_id == id) {
            return i;
        }
    }
    return -1;
}

static kvm_gesture_pt_t pt(const kvm_touch_point_t *p)
{
    kvm_gesture_pt_t r = {.x = p->x_px, .y = p->y_px, .id = p->track_id};
    return r;
}

static uint16_t cheb(kvm_gesture_pt_t p, kvm_gesture_pt_t q)
{
    int dx = p.x - q.x, dy = p.y - q.y;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    return (uint16_t)(dx > dy ? dx : dy);
}

static uint16_t max16(uint16_t a, uint16_t b)
{
    return a > b ? a : b;
}

float kvm_gesture_gain(kvm_gesture_speed_t speed, bool acceleration, float speed_px_per_ms)
{
    static const float base[] = {0.7f, 1.0f, 1.3f};
    float g = base[(unsigned)speed <= KVM_GEST_SPEED_FAST ? speed : KVM_GEST_SPEED_NORMAL];
    if (acceleration && speed_px_per_ms > 0.0f) {
        g += 0.35f * speed_px_per_ms; // FR-112: gain = clamp(base + 0.35 * speed_norm, 0.7, 2.2)
    }
    if (g < 0.7f) {
        g = 0.7f;
    } else if (g > 2.2f) {
        g = 2.2f;
    }
    return g;
}

static int16_t take_int(float *acc)
{
    float v = *acc;
    if (v > 32767.0f) {
        v = 32767.0f;
    } else if (v < -32767.0f) {
        v = -32767.0f;
    }
    int16_t i = (int16_t)v; // truncation toward zero; the remainder stays (FR-113)
    *acc -= (float)i;
    return i;
}

// Relative pointer from consecutive samples of the same finger (FR-111..113).
static void pointer_move(kvm_gesture_t *g, kvm_gesture_out_t *out, int dx, int dy, uint64_t dt_us)
{
    float dt_ms = (float)dt_us / 1000.0f;
    if (dt_ms < 1.0f) {
        dt_ms = 1.0f;
    }
    float v = sqrtf((float)(dx * dx + dy * dy)) / dt_ms;
    float gain = kvm_gesture_gain(g->cfg.speed, g->cfg.acceleration, v);
    g->accx += (float)dx * gain;
    g->accy += (float)dy * gain;
    int16_t ix = take_int(&g->accx), iy = take_int(&g->accy);
    if (ix != 0 || iy != 0) {
        emit(g, out, KVM_HID_MOUSE_MOVE, ix, iy);
        g->st.moves++;
    }
}

// Two-finger scroll from the centroid (FR-105, FR-114). `dx2/dy2` are in half pixels.
static void scroll_by(kvm_gesture_t *g, kvm_gesture_out_t *out, int32_t dx2, int32_t dy2)
{
    float dx = (float)dx2 / 2.0f, dy = (float)dy2 / 2.0f;
    if (g->scroll_axis == 0) {
        g->scroll_axis = fabsf(dy) >= fabsf(dx) ? 1 : 2;
    }
    float along = g->scroll_axis == 1 ? dy : dx;
    float cross = g->scroll_axis == 1 ? dx : dy;
    g->axis_travel += fabsf(along);
    g->cross_travel += fabsf(cross);
    // Hysteresis: the axis changes only on a clear, sustained cross movement, never on a reversal.
    if (g->cross_travel > 40.0f && g->cross_travel > 2.0f * g->axis_travel) {
        g->scroll_axis = g->scroll_axis == 1 ? 2 : 1;
        g->axis_travel = g->cross_travel;
        g->cross_travel = 0.0f;
        g->sx = g->sy = 0.0f;
        along = cross;
    }
    const float step = (float)(g->cfg.scroll_px_per_step ? g->cfg.scroll_px_per_step : 1);
    float *acc = g->scroll_axis == 1 ? &g->sy : &g->sx;
    *acc += along;
    int steps = (int)(*acc / step);
    if (steps == 0) {
        return;
    }
    *acc -= (float)steps * step;
    if (steps > 127) {
        steps = 127;
    } else if (steps < -127) {
        steps = -127;
    }
    if (g->scroll_axis == 1) {
        // Fingers down with natural scrolling: content follows, i.e. wheel up (+).
        emit(g, out, KVM_HID_MOUSE_WHEEL, 0, (int16_t)(g->cfg.scroll_natural ? steps : -steps));
    } else {
        // AC Pan: + = right. Fingers right with natural scrolling: content follows, i.e. pan left.
        emit(g, out, KVM_HID_MOUSE_WHEEL, (int16_t)(g->cfg.scroll_natural ? -steps : steps), 0);
    }
    g->st.scrolls++;
}

// ---- state helpers -------------------------------------------------------------------------
static void enter_cancel(kvm_gesture_t *g, kvm_gesture_cancel_t reason, kvm_gesture_out_t *out)
{
    g->st.cancels++;
    if ((unsigned)reason < KVM_GEST_CANCEL_COUNT) {
        g->st.cancel_by_reason[reason]++;
    }
    release_left(g, out); // FR-115: drag lock also releases on cancellation
    g->tap_valid = false;
    g->dtd_armed = false;
    g->state = g->last_count == 0 ? KVM_GEST_IDLE : KVM_GEST_CANCELLED;
}

static void start_one(kvm_gesture_t *g, const kvm_touch_point_t *p, uint64_t ts)
{
    g->start_us = ts;
    g->a0 = g->a = pt(p);
    g->max_a = 0;
    g->accx = g->accy = 0.0f; // FR-111: reset on every new gesture
    g->dtd_armed = g->tap_valid && ts - g->tap_up_us <= g->cfg.dtd_window_us &&
                   cheb(g->a0, g->tap_pt) <= g->cfg.dtd_max_dist_px;
    g->tap_valid = false;
    g->state = KVM_GEST_ONE_PENDING;
}

static void start_two(kvm_gesture_t *g, kvm_gesture_pt_t a, kvm_gesture_pt_t b, uint64_t ts, bool tap_ok)
{
    g->a0 = g->a = a;
    g->b0 = g->b = b;
    g->join_us = ts;
    g->b_present = true;
    g->max_b = 0;
    g->two_tap_ok = tap_ok;
    g->cx0 = g->cx = a.x + b.x;
    g->cy0 = g->cy = a.y + b.y;
    g->sx = g->sy = 0.0f;
    g->scroll_axis = 0;
    g->axis_travel = g->cross_travel = 0.0f;
    g->tap_valid = false; // TapSequenceTracker is cancelled by a two-finger contact
    g->dtd_armed = false;
    g->state = KVM_GEST_TWO_PENDING;
}

static bool sample_ok(const kvm_gesture_t *g, const kvm_touch_sample_t *s)
{
    if (s->point_count > KVM_TOUCH_MAX_POINTS) {
        return false;
    }
    if (g->have_seq && ((int32_t)(s->sample_seq - g->last_seq) <= 0 || s->timestamp_us < g->last_sample_us)) {
        return false; // no older (or repeated) sample
    }
    for (int i = 0; i < s->point_count; i++) {
        const kvm_touch_point_t *p = &s->point[i];
        if (p->x_px < 0 || p->y_px < 0 || p->x_px >= (int)g->cfg.h_res || p->y_px >= (int)g->cfg.v_res) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (s->point[j].track_id == p->track_id) {
                return false;
            }
        }
    }
    return true;
}

static void fire_long_press(kvm_gesture_t *g, kvm_gesture_out_t *out, uint64_t now)
{
    if (g->state == KVM_GEST_ONE_PENDING && !g->left_held && now - g->start_us >= g->cfg.long_press_us &&
        g->max_a <= g->cfg.long_press_max_move_px) {
        click(g, out, KVM_MOUSE_BTN_RIGHT); // FR-103: on the threshold, nothing on UP
        g->st.right_clicks++;
        g->state = KVM_GEST_RIGHT_FIRED;
    }
}

// ---- public API ----------------------------------------------------------------------------
void kvm_gesture_init(kvm_gesture_t *g, const kvm_gesture_cfg_t *cfg)
{
    memset(g, 0, sizeof(*g));
    const kvm_gesture_cfg_t def = KVM_GESTURE_CFG_DEFAULT;
    g->cfg = cfg != NULL ? *cfg : def;
    if (g->cfg.drag_lock_idle_us == 0 || g->cfg.drag_lock_idle_us > KVM_GESTURE_DRAG_LOCK_MAX_US) {
        g->cfg.drag_lock_idle_us = KVM_GESTURE_DRAG_LOCK_MAX_US; // safety limit 60 s (FR-115)
    }
    g->state = KVM_GEST_IDLE;
}

void kvm_gesture_feed(kvm_gesture_t *g, const kvm_touch_sample_t *s, kvm_gesture_out_t *out)
{
    out->n = 0;
    g->st.samples++;
    const uint64_t ts = s->timestamp_us;
    if (!s->valid) {
        g->last_count = 0; // the controller state is unknown; an empty frame must still follow
        g->last_sample_us = ts > g->last_sample_us ? ts : g->last_sample_us;
        enter_cancel(g, KVM_GEST_CANCEL_INVALID, out);
        if (g->state == KVM_GEST_IDLE) {
            g->state = KVM_GEST_CANCELLED; // wait for an explicit touch_count = 0
        }
        return;
    }
    if (!sample_ok(g, s)) {
        enter_cancel(g, KVM_GEST_CANCEL_MALFORMED, out);
        if (g->state == KVM_GEST_IDLE && s->point_count != 0) {
            g->state = KVM_GEST_CANCELLED;
        }
        return; // the malformed sample is not used, and its seq is not taken over
    }
    const bool active = g->state != KVM_GEST_IDLE && g->state != KVM_GEST_CANCELLED;
    const uint64_t prev_ts = g->last_sample_us;
    if (g->have_seq && active && g->last_count > 0 && ts - prev_ts > g->cfg.gap_cancel_us) {
        enter_cancel(g, KVM_GEST_CANCEL_GAP, out);
    }
    g->have_seq = true;
    g->last_seq = s->sample_seq;
    g->last_sample_us = ts;
    g->last_count = s->point_count;
    const uint8_t n = s->point_count;
    if (n > 0) {
        g->last_activity_us = ts;
    }

    if (n >= 3 && g->state != KVM_GEST_CANCELLED) {
        enter_cancel(g, KVM_GEST_CANCEL_FINGERS, out); // FR-109
        return;
    }

    switch (g->state) {
    case KVM_GEST_IDLE:
        if (n == 1) {
            start_one(g, &s->point[0], ts);
        } else if (n == 2) {
            if (!g->cfg.two_finger_enabled || g->left_held) {
                enter_cancel(g, g->left_held ? KVM_GEST_CANCEL_EXTRA_FINGER : KVM_GEST_CANCEL_FINGERS, out);
                break;
            }
            g->start_us = ts;
            g->max_a = 0;
            start_two(g, pt(&s->point[0]), pt(&s->point[1]), ts, true);
        }
        break;

    case KVM_GEST_ONE_PENDING: {
        if (n == 0) {
            const uint64_t dur = ts - g->start_us;
            if (dur <= g->cfg.tap_max_us && g->max_a <= g->cfg.tap_max_move_px) {
                if (g->left_held) {
                    release_left(g, out); // a tap ends a locked drag
                } else {
                    click(g, out, KVM_MOUSE_BTN_LEFT); // FR-102 (also the second click of a double click)
                    g->st.taps++;
                    g->tap_valid = true;
                    g->tap_up_us = ts;
                    g->tap_pt = g->a;
                }
            }
            g->state = KVM_GEST_IDLE;
            break;
        }
        int ia = find(s, g->a.id);
        if (ia < 0) {
            enter_cancel(g, KVM_GEST_CANCEL_MALFORMED, out); // the tracked finger vanished: no guessing
            break;
        }
        if (n == 2) {
            if (!g->cfg.two_finger_enabled || g->left_held) {
                enter_cancel(g, g->left_held ? KVM_GEST_CANCEL_EXTRA_FINGER : KVM_GEST_CANCEL_FINGERS, out);
                break;
            }
            // 01 §3.2 item 1: the pending tap / long press is dropped, nothing is emitted retroactively.
            kvm_gesture_pt_t a = pt(&s->point[ia]);
            kvm_gesture_pt_t b = pt(&s->point[ia == 0 ? 1 : 0]);
            g->max_a = max16(g->max_a, cheb(a, g->a0));
            bool tap_ok = ts - g->start_us <= g->cfg.two_join_us && g->max_a <= g->cfg.two_max_move_px;
            uint16_t keep_max_a = g->max_a;
            kvm_gesture_pt_t keep_a0 = g->a0;
            start_two(g, a, b, ts, tap_ok);
            g->a0 = keep_a0;
            g->max_a = keep_max_a;
            break;
        }
        kvm_gesture_pt_t p = pt(&s->point[ia]);
        g->max_a = max16(g->max_a, cheb(p, g->a0));
        const uint16_t threshold = g->dtd_armed ? g->cfg.dtd_move_px : g->cfg.move_start_px;
        if (cheb(p, g->a0) > threshold) {
            g->state = KVM_GEST_ONE_MOVING;
            if (g->dtd_armed || g->drag_lock) {
                press_left(g, out); // FR-106: LEFT DOWN before the first MOVE delta
            }
            pointer_move(g, out, p.x - g->a0.x, p.y - g->a0.y, ts - g->start_us);
            g->a = p;
            break;
        }
        g->a = p;
        fire_long_press(g, out, ts);
        break;
    }

    case KVM_GEST_ONE_MOVING: {
        if (n == 0) {
            if (!g->drag_lock) {
                release_left(g, out); // end of double-tap-and-drag: LEFT UP at the end
            }
            g->state = KVM_GEST_IDLE;
            break;
        }
        int ia = find(s, g->a.id);
        if (ia < 0) {
            enter_cancel(g, KVM_GEST_CANCEL_MALFORMED, out);
            break;
        }
        if (n == 2) {
            enter_cancel(g, KVM_GEST_CANCEL_EXTRA_FINGER, out); // 01 §3.2 item 3: never silently scroll
            break;
        }
        kvm_gesture_pt_t p = pt(&s->point[ia]);
        pointer_move(g, out, p.x - g->a.x, p.y - g->a.y, ts - prev_ts);
        g->a = p;
        break;
    }

    case KVM_GEST_RIGHT_FIRED:
    case KVM_GEST_WAIT_RELEASE:
    case KVM_GEST_CANCELLED:
        if (n == 0) {
            g->state = KVM_GEST_IDLE;
        }
        break;

    case KVM_GEST_TWO_PENDING:
    case KVM_GEST_TWO_SCROLLING: {
        const bool scrolling = g->state == KVM_GEST_TWO_SCROLLING;
        if (n == 0) {
            if (!scrolling && g->two_tap_ok && ts - g->start_us <= g->cfg.two_tap_max_us &&
                g->max_a <= g->cfg.two_max_move_px && g->max_b <= g->cfg.two_max_move_px) {
                click(g, out, KVM_MOUSE_BTN_RIGHT); // FR-104
                g->st.right_clicks++;
            }
            g->state = KVM_GEST_IDLE;
            break;
        }
        if (n == 1) {
            if (scrolling) {
                g->state = KVM_GEST_WAIT_RELEASE; // VEC-004: 2 -> 1 ends the gesture, no tap, no pointer
                break;
            }
            int ia = find(s, g->a.id), ib = find(s, g->b.id);
            if (ia >= 0) {
                g->a = pt(&s->point[ia]);
                g->max_a = max16(g->max_a, cheb(g->a, g->a0));
            } else if (ib >= 0) {
                g->b = pt(&s->point[ib]);
                g->max_b = max16(g->max_b, cheb(g->b, g->b0));
            } else {
                g->two_tap_ok = false;
            }
            g->b_present = false;
            break;
        }
        // n == 2
        int ia = find(s, g->a.id), ib = find(s, g->b.id);
        if (ia < 0 || ib < 0 || !g->b_present) {
            // A finger was replaced (or came back): re-base the centroid, never jump (VEC-003).
            g->two_tap_ok = false;
            g->a = pt(&s->point[0]);
            g->b = pt(&s->point[1]);
            g->a0 = g->a;
            g->b0 = g->b;
            g->cx0 = g->cx = g->a.x + g->b.x;
            g->cy0 = g->cy = g->a.y + g->b.y;
            g->b_present = true;
            break;
        }
        g->a = pt(&s->point[ia]);
        g->b = pt(&s->point[ib]);
        g->max_a = max16(g->max_a, cheb(g->a, g->a0));
        g->max_b = max16(g->max_b, cheb(g->b, g->b0));
        const int32_t cx = g->a.x + g->b.x, cy = g->a.y + g->b.y;
        if (!scrolling) {
            int32_t tx = cx - g->cx0, ty = cy - g->cy0;
            tx = tx < 0 ? -tx : tx;
            ty = ty < 0 ? -ty : ty;
            if ((tx > ty ? tx : ty) > 2 * (int32_t)g->cfg.scroll_start_px) {
                g->state = KVM_GEST_TWO_SCROLLING; // FR-105: blocks both clicks
                g->two_tap_ok = false;
                scroll_by(g, out, cx - g->cx0, cy - g->cy0);
                g->cx = cx;
                g->cy = cy;
            }
            break;
        }
        scroll_by(g, out, cx - g->cx, cy - g->cy);
        g->cx = cx;
        g->cy = cy;
        break;
    }
    }
}

void kvm_gesture_tick(kvm_gesture_t *g, uint64_t now_us, kvm_gesture_out_t *out)
{
    out->n = 0;
    if (now_us < g->last_sample_us) {
        return;
    }
    const bool active = g->state != KVM_GEST_IDLE && g->state != KVM_GEST_CANCELLED;
    if (active && g->last_count > 0 && now_us - g->last_sample_us > g->cfg.gap_cancel_us) {
        enter_cancel(g, KVM_GEST_CANCEL_GAP, out); // TECH-064: no data for > 120 ms
        return;
    }
    fire_long_press(g, out, now_us);
    if (g->left_held && g->last_count == 0 &&
        (!g->drag_lock || now_us - g->last_activity_us > g->cfg.drag_lock_idle_us)) {
        // Drag lock inactivity (FR-115), or a held LEFT without a lock and without a finger (safety net).
        if (g->drag_lock) {
            g->st.drag_lock_timeouts++;
        }
        release_left(g, out);
    }
}

void kvm_gesture_cancel(kvm_gesture_t *g, kvm_gesture_cancel_t reason, kvm_gesture_out_t *out)
{
    out->n = 0;
    enter_cancel(g, reason, out);
}

void kvm_gesture_set_drag_lock(kvm_gesture_t *g, bool on, uint64_t now_us, kvm_gesture_out_t *out)
{
    out->n = 0;
    g->drag_lock = on;
    g->last_activity_us = now_us;
    if (!on && g->state != KVM_GEST_ONE_MOVING) {
        release_left(g, out); // immediate switch-off (FR-115)
    }
}

const char *kvm_gesture_state_name(kvm_gesture_state_t s)
{
    static const char *const k[] = {"IDLE",          "ONE_PENDING",    "ONE_MOVING", "RIGHT_FIRED",
                                    "TWO_PENDING",   "TWO_SCROLLING",  "WAIT_RELEASE", "CANCELLED"};
    return (unsigned)s < sizeof(k) / sizeof(k[0]) ? k[s] : "?";
}
