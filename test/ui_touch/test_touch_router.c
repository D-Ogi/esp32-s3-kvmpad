// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-060E touch router: CHROME/PAD/BLOCKED classification at TOUCH_DOWN, per-contact capture,
// the 07 §6 conflict rules, cancel without a click, and the PAD stream fed to the real gesture FSM
// (AT-UIFW-04: pad + tab bar at the same time -> no phantom click, no stuck button).
#include <string.h>

#include "gesture_engine.h"
#include "kvm_test.h"
#include "local_contacts.h"
#include "touch_router.h"

#define W 1024
#define H 600
static const touch_rect_t PAD = {.x1 = 128, .y1 = 72, .x2 = 1007, .y2 = 511};
#define CHROME_X 50  // navigation rail
#define CHROME_Y 150
#define PAD_X 500
#define PAD_Y 300

static uint32_t s_seq;
static uint64_t s_t;

typedef struct {
    uint8_t n;
    kvm_touch_point_t p[KVM_TOUCH_MAX_POINTS];
} pts_t;

static kvm_touch_sample_t sample(const pts_t *p)
{
    kvm_touch_sample_t s;
    memset(&s, 0, sizeof(s));
    s.timestamp_us = (s_t += 10000);
    s.sample_seq = ++s_seq;
    s.valid = true;
    s.point_count = p ? p->n : 0;
    for (uint8_t i = 0; p && i < p->n; i++) {
        s.point[i] = p->p[i];
    }
    return s;
}

static pts_t one(uint8_t id, int16_t x, int16_t y)
{
    pts_t p = {.n = 1};
    p.p[0] = (kvm_touch_point_t){.track_id = id, .x_px = x, .y_px = y};
    return p;
}

static pts_t two(uint8_t ida, int16_t xa, int16_t ya, uint8_t idb, int16_t xb, int16_t yb)
{
    pts_t p = {.n = 2};
    p.p[0] = (kvm_touch_point_t){.track_id = ida, .x_px = xa, .y_px = ya};
    p.p[1] = (kvm_touch_point_t){.track_id = idb, .x_px = xb, .y_px = yb};
    return p;
}

static touch_router_t r;
static touch_route_out_t o;

static void route(const pts_t *p)
{
    kvm_touch_sample_t s = sample(p);
    touch_router_route(&r, &s, &o);
}

static void reset(bool pad_enabled)
{
    touch_router_init(&r);
    touch_router_set_pad(&r, &PAD, pad_enabled);
}

static void test_chrome_tap(void)
{
    reset(true);
    pts_t p = one(0, CHROME_X, CHROME_Y);
    route(&p);
    CHECK(o.ptr_pressed);
    CHECK_EQ(o.ptr_x, CHROME_X);
    CHECK_EQ(o.pad.point_count, 0);
    CHECK_EQ(touch_router_target(&r, 0), TOUCH_TARGET_CHROME);
    route(NULL);
    CHECK(!o.ptr_pressed);
    CHECK(!o.ptr_cancel); // a normal release: LVGL may click
    CHECK_EQ(o.ptr_x, CHROME_X); // release point kept
    CHECK_EQ(touch_router_target(&r, 0), TOUCH_TARGET_NONE);
}

static void test_pad_drag_no_pointer(void)
{
    reset(true);
    for (int i = 0; i < 20; i++) {
        pts_t p = one(3, (int16_t)(PAD_X + i * 10), PAD_Y);
        route(&p);
        CHECK(!o.ptr_pressed);
        CHECK_EQ(o.pad.point_count, 1);
        CHECK_EQ(o.pad.point[0].track_id, 3);
    }
    route(NULL);
    CHECK_EQ(o.pad.point_count, 0);
    CHECK(o.pad.valid);
    CHECK_EQ(r.st.pad_downs, 1);
    CHECK_EQ(r.st.chrome_downs, 0);
}

static void test_pad_disabled_routes_to_chrome(void)
{
    reset(false); // keyboard view / modal: the pad area is ordinary chrome
    pts_t p = one(0, PAD_X, PAD_Y);
    route(&p);
    CHECK(o.ptr_pressed);
    CHECK_EQ(o.pad.point_count, 0);
    touch_router_set_pad(&r, NULL, true); // no rectangle: stays disabled
    CHECK(!r.pad_enabled);
}

static void test_capture_across_edge(void)
{
    reset(true);
    // pad contact that slides over the rail stays PAD
    pts_t p = one(1, PAD.x1 + 5, PAD_Y);
    route(&p);
    p = one(1, CHROME_X, PAD_Y);
    route(&p);
    CHECK_EQ(touch_router_target(&r, 1), TOUCH_TARGET_PAD);
    CHECK(!o.ptr_pressed);
    CHECK_EQ(o.pad.point_count, 1);
    CHECK_EQ(o.pad.point[0].x_px, CHROME_X);
    route(NULL);
    // chrome contact that slides into the pad stays CHROME (no HID)
    p = one(2, CHROME_X, CHROME_Y);
    route(&p);
    p = one(2, PAD_X, PAD_Y);
    route(&p);
    CHECK_EQ(touch_router_target(&r, 2), TOUCH_TARGET_CHROME);
    CHECK(o.ptr_pressed);
    CHECK_EQ(o.pad.point_count, 0);
    CHECK_EQ(o.ptr_x, PAD_X);
}

static void test_pad_held_then_chrome_blocked(void)
{
    reset(true);
    pts_t p = one(0, PAD_X, PAD_Y);
    route(&p);
    pts_t q = two(0, PAD_X + 5, PAD_Y, 1, CHROME_X, CHROME_Y);
    route(&q);
    CHECK_EQ(touch_router_target(&r, 1), TOUCH_TARGET_BLOCKED);
    CHECK(!o.ptr_pressed); // no phantom click while mousing
    CHECK_EQ(o.pad.point_count, 1);
    // pad finger lifts, the blocked finger is still held: it stays blocked
    pts_t b = one(1, CHROME_X, CHROME_Y);
    route(&b);
    CHECK(!o.ptr_pressed);
    CHECK_EQ(o.pad.point_count, 0);
    route(NULL);
    CHECK_EQ(r.st.blocked_downs, 1);
}

static void test_chrome_held_then_pad_blocked(void)
{
    reset(true);
    pts_t p = one(0, CHROME_X, CHROME_Y);
    route(&p);
    pts_t q = two(0, CHROME_X, CHROME_Y, 1, PAD_X, PAD_Y);
    route(&q);
    CHECK_EQ(touch_router_target(&r, 1), TOUCH_TARGET_BLOCKED);
    CHECK(o.ptr_pressed);
    CHECK_EQ(o.pad.point_count, 0); // no HID from a finger that started while a button was held
}

static void test_second_chrome_blocked(void)
{
    reset(true);
    pts_t p = one(0, CHROME_X, CHROME_Y);
    route(&p);
    pts_t q = two(0, CHROME_X, CHROME_Y, 1, 900, 560);
    route(&q);
    CHECK_EQ(touch_router_target(&r, 1), TOUCH_TARGET_BLOCKED);
    CHECK_EQ(o.ptr_x, CHROME_X);
    pts_t b = one(1, 900, 560); // first lifts: pointer released, the second stays blocked
    route(&b);
    CHECK(!o.ptr_pressed);
    CHECK(!o.ptr_cancel);
}

static void test_invalid_cancels_without_click(void)
{
    reset(true);
    pts_t p = one(0, CHROME_X, CHROME_Y);
    route(&p);
    kvm_touch_sample_t bad = sample(NULL);
    bad.valid = false;
    touch_router_route(&r, &bad, &o);
    CHECK(!o.ptr_pressed);
    CHECK(o.ptr_cancel);
    CHECK(!o.pad.valid);
    CHECK_EQ(r.st.pointer_cancels, 1);
    // the finger is still there after the error: it is a new contact, classified again
    route(&p);
    CHECK(o.ptr_pressed);
    CHECK(!o.ptr_cancel);
    // over-long sample is treated as invalid too
    kvm_touch_sample_t big = sample(NULL);
    big.point_count = KVM_TOUCH_MAX_POINTS + 1;
    touch_router_route(&r, &big, &o);
    CHECK(o.ptr_cancel);
    CHECK(!o.pad.valid);
}

static void test_cancel_blocks_until_lift(void)
{
    reset(true);
    pts_t q = two(0, CHROME_X, CHROME_Y, 1, 600, 560);
    route(&q);
    touch_router_cancel(&r);
    route(&q);
    CHECK(o.ptr_cancel);
    CHECK(!o.ptr_pressed);
    CHECK_EQ(touch_router_target(&r, 0), TOUCH_TARGET_BLOCKED);
    route(&q);
    CHECK(!o.ptr_cancel); // reported once
    CHECK(!o.ptr_pressed);
    route(NULL);
    pts_t p = one(0, CHROME_X, CHROME_Y); // after the lift a new press works again
    route(&p);
    CHECK(o.ptr_pressed);
    // cancel with only a pad contact: no pointer cancel, the contact is blocked
    reset(true);
    pts_t pad = one(4, PAD_X, PAD_Y);
    route(&pad);
    touch_router_cancel(&r);
    route(&pad);
    CHECK(!o.ptr_cancel);
    CHECK_EQ(o.pad.point_count, 0);
}

static void test_cancelled_ids_survive_controller_fault(void)
{
    reset(true);
    r.require_lift_after_error = true;
    pts_t held = two(0, CHROME_X, CHROME_Y, 1, PAD_X, PAD_Y);
    route(&held);
    touch_router_cancel(&r);
    kvm_touch_sample_t fault = sample(NULL);
    fault.valid = false;
    touch_router_route(&r, &fault, &o);
    CHECK(o.ptr_cancel && !o.ptr_pressed);
    touch_router_route(&r, &fault, &o); // repeated errors cannot manufacture a physical lift
    route(&held);
    CHECK(!o.ptr_pressed && o.pad.point_count == 0);
    pts_t second = one(1, PAD_X, PAD_Y);
    route(&second);
    CHECK_EQ(touch_router_target(&r, 1), TOUCH_TARGET_BLOCKED);
    route(&held); // ID 0 lifted in the previous valid frame and is now a fresh press
    CHECK(o.ptr_pressed && !o.ptr_cancel && o.pad.point_count == 0);
}

// AT-UIFW-04 with the real gesture FSM: a two-finger tap on the pad gives exactly one right click,
// a rail press during a pad drag never presses the LVGL pointer, and every button goes back up.
static void feed(kvm_gesture_t *g, int *down, int *up, int *right)
{
    kvm_gesture_out_t go;
    memset(&go, 0, sizeof(go));
    kvm_gesture_feed(g, &o.pad, &go);
    for (uint8_t i = 0; i < go.n; i++) {
        if (go.ev[i].type == KVM_HID_MOUSE_BTN_DOWN) {
            (*down)++;
            if (go.ev[i].data.mouse_button.button_id == KVM_MOUSE_BTN_RIGHT) {
                (*right)++;
            }
        } else if (go.ev[i].type == KVM_HID_MOUSE_BTN_UP) {
            (*up)++;
        }
    }
}

static void test_gesture_integration(void)
{
    kvm_gesture_cfg_t cfg = KVM_GESTURE_CFG_DEFAULT;
    cfg.h_res = W;
    cfg.v_res = H;
    kvm_gesture_t g;
    kvm_gesture_init(&g, &cfg);
    reset(true);
    int down = 0, up = 0, right = 0, ptr_presses = 0;

    // two-finger tap on the pad
    pts_t a = one(0, PAD_X, PAD_Y);
    route(&a);
    feed(&g, &down, &up, &right);
    pts_t b = two(0, PAD_X, PAD_Y, 1, PAD_X + 60, PAD_Y);
    route(&b);
    feed(&g, &down, &up, &right);
    route(&b);
    feed(&g, &down, &up, &right);
    route(NULL);
    feed(&g, &down, &up, &right);
    CHECK_EQ(right, 1);
    CHECK_EQ(down, up);

    // one-finger drag on the pad with a rail press in the middle
    s_t += 1000000; // well past the double-tap window
    for (int i = 0; i < 30; i++) {
        pts_t p = one(2, (int16_t)(PAD_X + i * 6), PAD_Y);
        if (i >= 10 && i < 20) {
            p = two(2, (int16_t)(PAD_X + i * 6), PAD_Y, 3, CHROME_X, CHROME_Y);
        }
        route(&p);
        ptr_presses += o.ptr_pressed;
        feed(&g, &down, &up, &right);
    }
    route(NULL);
    feed(&g, &down, &up, &right);
    CHECK_EQ(ptr_presses, 0);
    CHECK_EQ(down, up);
    CHECK(!kvm_gesture_left_held(&g));
    CHECK_EQ(g.st.out_overflow, 0);
}

// Random samples: invariants hold for every output.
static uint32_t rng(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void test_random_invariants(void)
{
    uint32_t seed = 0xA5A5F00Du;
    reset(true);
    touch_target_t held[16];
    for (int i = 0; i < 16; i++) {
        held[i] = TOUCH_TARGET_NONE;
    }
    int violations = 0;
    for (int it = 0; it < 200000; it++) {
        pts_t p = {.n = (uint8_t)(rng(&seed) % 6)};
        uint8_t used = 0;
        for (uint8_t k = 0; k < p.n; k++) {
            uint8_t id;
            do {
                id = (uint8_t)(rng(&seed) % 8);
            } while (used & (1u << id));
            used |= (uint8_t)(1u << id);
            p.p[k] = (kvm_touch_point_t){.track_id = id, .x_px = (int16_t)(rng(&seed) % W),
                                         .y_px = (int16_t)(rng(&seed) % H)};
        }
        // ids in ascending order like touch_input delivers them
        for (uint8_t a2 = 0; a2 < p.n; a2++) {
            for (uint8_t b2 = (uint8_t)(a2 + 1); b2 < p.n; b2++) {
                if (p.p[b2].track_id < p.p[a2].track_id) {
                    kvm_touch_point_t t = p.p[a2];
                    p.p[a2] = p.p[b2];
                    p.p[b2] = t;
                }
            }
        }
        uint32_t action = rng(&seed) % 100;
        if (action == 0) {
            touch_router_cancel(&r);
        }
        kvm_touch_sample_t s = sample(&p);
        if (action == 1) {
            s.valid = false;
        }
        bool none_held = true;
        for (int i = 0; i < 16; i++) {
            none_held = none_held && held[i] == TOUCH_TARGET_NONE;
        }
        touch_router_route(&r, &s, &o);
        // liveness: a lone first finger is never blocked (PAD on the pad, CHROME elsewhere)
        if (none_held && s.valid && action != 0 && p.n == 1) {
            bool on_pad = p.p[0].x_px >= PAD.x1 && p.p[0].x_px <= PAD.x2 && p.p[0].y_px >= PAD.y1 &&
                          p.p[0].y_px <= PAD.y2;
            violations += touch_router_target(&r, p.p[0].track_id) != (on_pad ? TOUCH_TARGET_PAD : TOUCH_TARGET_CHROME);
            violations += on_pad ? o.pad.point_count != 1 : !o.ptr_pressed;
        }
        // pad output: only PAD contacts, consistent sample
        for (uint8_t k = 0; k < o.pad.point_count; k++) {
            violations += touch_router_target(&r, o.pad.point[k].track_id) != TOUCH_TARGET_PAD;
        }
        violations += !kvm_touch_sample_is_consistent(&o.pad, W, H, false, 0) && o.pad.valid;
        // pointer pressed -> exactly one CHROME contact and it is held
        int chrome = 0;
        for (int i = 0; i < KVM_TOUCH_MAX_POINTS; i++) {
            chrome += r.c[i].used && r.c[i].target == TOUCH_TARGET_CHROME;
        }
        violations += chrome > 1;
        violations += o.ptr_pressed && touch_router_target(&r, r.ptr_id) != TOUCH_TARGET_CHROME;
        // PAD and CHROME never both newly granted while the other is held
        // a held contact never changes target, except to BLOCKED by a cancel
        for (uint8_t id = 0; id < 8; id++) {
            touch_target_t now = touch_router_target(&r, id);
            if (held[id] != TOUCH_TARGET_NONE && now != TOUCH_TARGET_NONE && now != held[id] &&
                now != TOUCH_TARGET_BLOCKED && s.valid) {
                violations++;
            }
            held[id] = now;
        }
    }
    CHECK_EQ(violations, 0);
    CHECK(r.st.pad_downs > 0 && r.st.chrome_downs > 0 && r.st.blocked_downs > 0);
    printf("    200000 random samples: pad_downs=%u chrome_downs=%u blocked=%u pointer_cancels=%u\n",
           (unsigned)r.st.pad_downs, (unsigned)r.st.chrome_downs, (unsigned)r.st.blocked_downs,
           (unsigned)r.st.pointer_cancels);
}

static void queue_sample(local_contacts_queue_t *q, const pts_t *points)
{
    kvm_touch_sample_t s = sample(points);
    local_contacts_post(q, &s, (local_pointer_t){0});
}

typedef struct {
    unsigned n;
    kvm_hid_intent_t events[8 * KVM_GESTURE_MAX_OUT];
} gesture_trace_t;

static void drain_gestures(local_contacts_queue_t *q, kvm_gesture_t *gesture, uint32_t *seq, gesture_trace_t *trace)
{
    for (unsigned i = 0; i < q->used; i++) {
        const local_contacts_t *frame = &q->pending[i];
        kvm_touch_sample_t s = {.valid = frame->count <= 2, .timestamp_us = frame->timestamp_us,
                                .sample_seq = ++*seq, .point_count = frame->count <= 2 ? (uint8_t)frame->count : 0};
        memcpy(s.point, frame->points, sizeof(frame->points));
        kvm_gesture_out_t out;
        kvm_gesture_feed(gesture, &s, &out);
        if (trace && trace->n + out.n <= sizeof(trace->events)/sizeof(trace->events[0])) {
            memcpy(&trace->events[trace->n], out.ev, out.n * sizeof(out.ev[0]));
            trace->n += out.n;
        } else if (trace) {
            CHECK(0 && "gesture trace overflow");
        }
    }
    q->used = 0;
}

static void test_gestures_during_monitor_refresh(void)
{
    for (int fingers = 1; fingers <= 2; fingers++) {
        local_contacts_queue_t q = {0};
        kvm_gesture_t gesture;
        kvm_gesture_init(&gesture, NULL);
        uint32_t seq = 0;
        pts_t p = fingers == 1 ? one(1, 200, 100) : two(1, 200, 100, 2, 300, 100);
        if (fingers == 1) {
            queue_sample(&q, &p);
            queue_sample(&q, NULL); // First tap of tap-and-drag.
        }
        for (int i = 0; i <= 31; i++) {
            p.p[0].y_px += 3;
            p.p[1].y_px += 3;
            queue_sample(&q, &p);
        }
        drain_gestures(&q, &gesture, &seq, NULL);
        CHECK_EQ(gesture.st.cancels, 0);
        if (fingers == 1) {
            CHECK(gesture.left_held && gesture.st.drags == 1);
            local_contacts_expire(&q, q.last_time + KVM_TOUCH_GAP_CANCEL_US);
            CHECK(!q.used);
            local_contacts_expire(&q, q.last_time + KVM_TOUCH_GAP_CANCEL_US + 1);
            CHECK(q.blocked && q.used == 1);
            drain_gestures(&q, &gesture, &seq, NULL);
            CHECK(!gesture.left_held && gesture.state == KVM_GEST_CANCELLED);
            s_t += KVM_TOUCH_GAP_CANCEL_US + 1;
            queue_sample(&q, &p);
            CHECK(q.blocked && !q.used);
        } else {
            CHECK(gesture.st.scrolls > 0 && !gesture.left_held);
        }
        queue_sample(&q, NULL);
        drain_gestures(&q, &gesture, &seq, NULL);
        CHECK(!gesture.left_held && gesture.state == KVM_GEST_IDLE);
        CHECK(!q.blocked);
    }
}

static void test_short_reversals_during_refresh(void)
{
    enum { POINTER, SCROLL, CENTROID_TURN, OPPOSING, LEFT_TAP, RIGHT_TAP };
    // The diagonal / parallel strokes travel 36 px, initially reverse by 1 px,
    // and return to their origin before the stalled reader sees a sample.
    const int16_t steps[]={0,6,12,18,24,30,36,35,29,22,15,8,0};
    for (int kind=POINTER;kind<=RIGHT_TAP;kind++) for (int stalled=0;stalled<=1;stalled++) {
        local_contacts_queue_t q={0};
        kvm_gesture_t gesture;
        kvm_gesture_init(&gesture,NULL);
        uint32_t seq=0;
        gesture_trace_t trace={0};
        for (unsigned i=0;i<sizeof(steps)/sizeof(steps[0]);i++) {
            int16_t d=steps[i];
            if (kind==LEFT_TAP || kind==RIGHT_TAP) {
                int limit=kind==LEFT_TAP ? 6 : 10;
                d=(int16_t)(i==0 ? 0 : (i%2 ? -limit : limit)); // Jitter at the tap boundary.
            }
            pts_t points=kind==POINTER || kind==LEFT_TAP
                ? one(1,(int16_t)(200+d),(int16_t)(100+d))
                : two(1,200,(int16_t)(100+d),2,300,(int16_t)(kind==OPPOSING ? 100-d : 100+d));
            if (kind==CENTROID_TURN) {
                // Each finger moves in one direction only, but their center rises then falls.
                points.p[0].y_px=(int16_t)(100+12*(i<6 ? (int)i : 6));
                points.p[1].y_px=(int16_t)(140-12*(i>6 ? (int)i-6 : 0));
            }
            queue_sample(&q,&points);
            if (!stalled) drain_gestures(&q,&gesture,&seq,&trace);
        }
        queue_sample(&q,NULL);
        drain_gestures(&q,&gesture,&seq,&trace);
        unsigned buttons=0;
        bool outward=false, returning=false;
        for (unsigned i=0;i<trace.n;i++) {
            const kvm_hid_intent_t *e=&trace.events[i];
            if (e->type==KVM_HID_MOUSE_BTN_DOWN || e->type==KVM_HID_MOUSE_BTN_UP) buttons++;
            if (kind==POINTER && e->type==KVM_HID_MOUSE_MOVE) {
                outward |= e->data.mouse_move.dx>0 && e->data.mouse_move.dy>0;
                returning |= outward && e->data.mouse_move.dx<0 && e->data.mouse_move.dy<0;
            }
            if ((kind==SCROLL || kind==CENTROID_TURN) && e->type==KVM_HID_MOUSE_WHEEL) {
                outward |= e->data.wheel.vertical>0;
                returning |= outward && e->data.wheel.vertical<0;
            }
        }
        if (kind==POINTER || kind==SCROLL || kind==CENTROID_TURN) {
            CHECK(outward && returning);
            CHECK_EQ(buttons,0);
        } else if (kind==OPPOSING) {
            CHECK_EQ(trace.n,0); // Fixed centroid; neither a scroll nor a stationary two-finger tap.
        } else {
            unsigned button=kind==LEFT_TAP ? KVM_MOUSE_BTN_LEFT : KVM_MOUSE_BTN_RIGHT;
            CHECK_EQ(trace.n,2);
            CHECK(trace.events[0].type==KVM_HID_MOUSE_BTN_DOWN && trace.events[0].data.mouse_button.button_id==button);
            CHECK(trace.events[1].type==KVM_HID_MOUSE_BTN_UP && trace.events[1].data.mouse_button.button_id==button);
        }
        CHECK_EQ(gesture.st.cancels,0);
        CHECK(!gesture.left_held && gesture.state==KVM_GEST_IDLE);
    }
}

static void test_drag_threshold_after_queue_checkpoint(void)
{
    gesture_trace_t trace[2]={0};
    for (int stalled=0;stalled<=1;stalled++) {
        local_contacts_queue_t q={0};
        kvm_gesture_t gesture;
        kvm_gesture_init(&gesture,NULL);
        uint32_t seq=0;
        pts_t p=one(1,200,100);
        queue_sample(&q,&p);
        queue_sample(&q,NULL);
        drain_gestures(&q,&gesture,&seq,NULL);
        CHECK_EQ(gesture.st.taps,1);

        // Second touch: 130 ms of continuous samples establish a checkpoint
        // at +7. Only +9 crosses the armed tap-and-drag threshold (>8 px).
        const int16_t dx[]={0,7,7,7,7,7,7,7,7,7,7,7,7,7,8,9,8,7};
        for (unsigned i=0;i<sizeof(dx)/sizeof(dx[0]);i++) {
            p.p[0].x_px=(int16_t)(200+dx[i]);
            queue_sample(&q,&p);
            if (!stalled) drain_gestures(&q,&gesture,&seq,&trace[stalled]);
        }
        queue_sample(&q,NULL); // 180 ms after the second touch began.
        drain_gestures(&q,&gesture,&seq,&trace[stalled]);
        const kvm_hid_intent_t *e=trace[stalled].events;
        printf("    checkpoint %s: reports=%u drags=%u taps=%u\n",stalled ? "queued" : "immediate",
               trace[stalled].n,(unsigned)gesture.st.drags,(unsigned)gesture.st.taps);
        CHECK_EQ(gesture.st.drags,1);
        CHECK_EQ(gesture.st.taps,1);
        CHECK_EQ(gesture.st.cancels,0);
        CHECK(!gesture.left_held && gesture.state==KVM_GEST_IDLE);
        CHECK_EQ(trace[stalled].n,4);
        CHECK(e[0].type==KVM_HID_MOUSE_BTN_DOWN && e[0].data.mouse_button.button_id==KVM_MOUSE_BTN_LEFT);
        CHECK(e[1].type==KVM_HID_MOUSE_MOVE && e[1].data.mouse_move.dx>0 && e[1].data.mouse_move.dy==0);
        CHECK(e[2].type==KVM_HID_MOUSE_MOVE && e[2].data.mouse_move.dx<0 && e[2].data.mouse_move.dy==0);
        CHECK(e[3].type==KVM_HID_MOUSE_BTN_UP && e[3].data.mouse_button.button_id==KVM_MOUSE_BTN_LEFT);
    }
    CHECK_EQ(trace[0].events[1].data.mouse_move.dx,trace[1].events[1].data.mouse_move.dx);
    CHECK_EQ(trace[0].events[2].data.mouse_move.dx,trace[1].events[2].data.mouse_move.dx);
}

static void test_local_pair_queue(void)
{
    local_contacts_queue_t q = {0};
    pts_t pair = two(3, 400, 150, 1, 600, 150);
    for (int i = 0; i < 24; ++i) {
        pair.p[1].x_px++;
        kvm_touch_sample_t s = sample(&pair);
        local_contacts_post(&q, &s, (local_pointer_t){0});
    }
    CHECK_EQ(q.pending[0].points[0].x_px, 601);
    CHECK(q.used >= 2 && q.used < 8);
    CHECK_EQ(q.pending[q.used - 1].points[0].x_px, 624);
    CHECK_EQ(q.pending[0].points[0].track_id, 1);
    pair.p[0].track_id = 4;
    kvm_touch_sample_t changed = sample(&pair);
    local_contacts_post(&q, &changed, (local_pointer_t){0});
    CHECK(q.blocked && q.used == 1 && q.pending[0].count == 3);
    kvm_touch_sample_t lift = sample(NULL);
    local_contacts_post(&q, &lift, (local_pointer_t){0});
    CHECK(!q.blocked && q.pending[1].count == 0);
    changed = sample(&pair);
    local_contacts_post(&q, &changed, (local_pointer_t){0});
    changed.timestamp_us += KVM_TOUCH_GAP_CANCEL_US + 1;
    changed.sample_seq++;
    local_contacts_post(&q, &changed, (local_pointer_t){0});
    CHECK(q.blocked && q.used == 1);
    q = (local_contacts_queue_t){0};
    for (int i = 0; i < 9; ++i) {
        kvm_touch_sample_t s = sample(i % 2 ? NULL : &pair);
        local_contacts_post(&q, &s, (local_pointer_t){0});
    }
    CHECK(q.blocked && q.used == 1 && q.pending[0].count == 3);
}

static void test_gap_before_lift(void)
{
    local_contacts_queue_t q = {0};
    pts_t p = one(1, 200, 100);
    queue_sample(&q, &p);
    kvm_touch_sample_t lift = sample(NULL);
    lift.sample_seq++; // The missing sample may have moved beyond the tap target.
    local_contacts_post(&q, &lift, (local_pointer_t){0});
    CHECK(!q.blocked && q.used == 2);
    CHECK(q.pending[0].count == 3 && q.pending[1].count == 0);
}

int main(void)
{
    printf("test_touch_router\n");
    RUN(test_chrome_tap);
    RUN(test_local_pair_queue);
    RUN(test_gestures_during_monitor_refresh);
    RUN(test_short_reversals_during_refresh);
    RUN(test_drag_threshold_after_queue_checkpoint);
    RUN(test_gap_before_lift);
    RUN(test_pad_drag_no_pointer);
    RUN(test_pad_disabled_routes_to_chrome);
    RUN(test_capture_across_edge);
    RUN(test_pad_held_then_chrome_blocked);
    RUN(test_chrome_held_then_pad_blocked);
    RUN(test_second_chrome_blocked);
    RUN(test_invalid_cancels_without_click);
    RUN(test_cancelled_ids_survive_controller_fault);
    RUN(test_cancel_blocks_until_lift);
    RUN(test_gesture_integration);
    RUN(test_random_invariants);
    TEST_MAIN_END("test_touch_router");
}
