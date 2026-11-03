// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-060B: bounded UI mailbox — level semantics, late (stale) publishes, invalid values,
// coalescing, the event ring and its overflow/resync, lock pairing. AT-UIFW-03 (store level).
#include <string.h>

#include "kvm_test.h"
#include "ui_mailbox.h"

static int s_locks, s_unlocks, s_depth, s_max_depth;

static void lk(void *ctx)
{
    (void)ctx;
    s_locks++;
    s_depth++;
    if (s_depth > s_max_depth) {
        s_max_depth = s_depth;
    }
}

static void ulk(void *ctx)
{
    (void)ctx;
    s_unlocks++;
    s_depth--;
}

static ui_hid_slice_t hid(uint8_t usb, uint8_t owner, uint32_t epoch)
{
    ui_hid_slice_t s = {.host_usb_state = usb, .owner_kind = owner, .owner_epoch = epoch};
    return s;
}

static void test_initial_unknown(void)
{
    ui_mailbox_t mb;
    ui_mailbox_init(&mb, NULL);
    uint32_t seen = 0;
    ui_state_snapshot_t out;
    bool rs = true;
    CHECK(!ui_mailbox_take(&mb, &seen, &out, &rs)); // nothing published: no snapshot to apply
    CHECK(!rs);
    CHECK_EQ(mb.state.sources, 0);
}

static void test_publish_take_version(void)
{
    ui_mailbox_t mb;
    ui_mailbox_init(&mb, NULL);
    ui_hid_slice_t h = hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 3);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &h, 1), UI_PUB_APPLIED);
    uint32_t seen = 0;
    ui_state_snapshot_t out;
    CHECK(ui_mailbox_take(&mb, &seen, &out, NULL));
    CHECK_EQ(out.version, 1);
    CHECK_EQ(seen, 1);
    CHECK_EQ(out.sources, UI_SRC_HID);
    CHECK_EQ(out.host_usb_state, KVM_HOST_USB_MOUNTED);
    CHECK_EQ(out.owner_kind, OWNER_LOCAL);
    CHECK_EQ(out.owner_epoch, 3);
    CHECK(!ui_mailbox_take(&mb, &seen, &out, NULL)); // same version: nothing new

    // identical content: coalesced, version kept
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &h, 2), UI_PUB_UNCHANGED);
    CHECK(!ui_mailbox_take(&mb, &seen, &out, NULL));

    // other sources merge without touching the HID fields
    ui_net_slice_t n = {.client_state = UI_CLIENT_DISABLED, .wifi_connected = false};
    CHECK_EQ(ui_mailbox_publish_net(&mb, &n, 1), UI_PUB_APPLIED); // first publish of a source always applies
    ui_touch_slice_t t = {.touch_healthy = true};
    CHECK_EQ(ui_mailbox_publish_touch(&mb, &t, 7), UI_PUB_APPLIED);
    CHECK(ui_mailbox_take(&mb, &seen, &out, NULL));
    CHECK_EQ(out.sources, UI_SRC_HID | UI_SRC_NET | UI_SRC_TOUCH);
    CHECK_EQ(out.version, 3);
    CHECK_EQ(out.owner_epoch, 3);
    CHECK(out.touch_healthy);
    CHECK(!(out.sources & UI_SRC_GESTURE)); // drag lock still unknown
}

static void test_stale_and_wrap(void)
{
    ui_mailbox_t mb;
    ui_mailbox_init(&mb, NULL);
    ui_hid_slice_t a = hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 10);
    ui_hid_slice_t late = hid(KVM_HOST_USB_DETACHED, OWNER_NONE, 9);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &a, 100), UI_PUB_APPLIED);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &late, 99), UI_PUB_STALE);  // older domain sequence
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &late, 100), UI_PUB_STALE); // same sequence
    CHECK_EQ(mb.state.host_usb_state, KVM_HOST_USB_MOUNTED);
    CHECK_EQ(mb.state.owner_epoch, 10);
    CHECK_EQ(mb.c.stale, 2);

    // sequence wrap: 0xFFFFFFFF -> 0 -> 1 is newer
    ui_mailbox_init(&mb, NULL);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &a, 0xFFFFFFFFu), UI_PUB_APPLIED);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &late, 0), UI_PUB_APPLIED);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &a, 0xFFFFFFF0u), UI_PUB_STALE);
    CHECK_EQ(mb.state.owner_epoch, 9);

    // sequences are per source: a NET sequence does not affect HID
    ui_net_slice_t n = {.client_state = UI_CLIENT_LISTENING};
    CHECK_EQ(ui_mailbox_publish_net(&mb, &n, 0), UI_PUB_APPLIED);
}

static void test_invalid(void)
{
    ui_mailbox_t mb;
    ui_mailbox_init(&mb, NULL);
    ui_hid_slice_t ok = hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 1);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &ok, 1), UI_PUB_APPLIED);
    ui_hid_slice_t bad_usb = hid(KVM_HOST_USB_ERROR + 1, OWNER_LOCAL, 2);
    ui_hid_slice_t bad_owner = hid(KVM_HOST_USB_MOUNTED, OWNER_REMOTE + 1, 2);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &bad_usb, 2), UI_PUB_INVALID);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &bad_owner, 3), UI_PUB_INVALID);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, NULL, 4), UI_PUB_INVALID);
    ui_net_slice_t bad_net = {.client_state = UI_CLIENT_COUNT};
    CHECK_EQ(ui_mailbox_publish_net(&mb, &bad_net, 1), UI_PUB_INVALID);
    CHECK_EQ(mb.state.host_usb_state, KVM_HOST_USB_MOUNTED);
    CHECK_EQ(mb.state.owner_epoch, 1);
    CHECK_EQ(mb.state.version, 1);
    CHECK(!(mb.state.sources & UI_SRC_NET));
    CHECK_EQ(mb.c.invalid, 4);
    // an invalid publish does not consume the sequence: seq 2 is still accepted afterwards
    ui_hid_slice_t next = hid(KVM_HOST_USB_SUSPENDED, OWNER_LOCAL, 1);
    CHECK_EQ(ui_mailbox_publish_hid(&mb, &next, 2), UI_PUB_APPLIED);
}

static void test_caps_needs_known(void)
{
    ui_mailbox_t mb;
    ui_mailbox_init(&mb, NULL);
    ui_hid_slice_t h = hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 1);
    h.caps_led_known = false;
    h.caps_led_active = true; // producer bug: active without a report
    ui_mailbox_publish_hid(&mb, &h, 1);
    CHECK(!mb.state.caps_led_active);
}

static void test_events_ring(void)
{
    ui_mailbox_t mb;
    ui_mailbox_init(&mb, NULL);
    for (uint32_t i = 1; i <= UI_MAILBOX_EVENT_CAP; i++) {
        ui_event_t e = {.kind = UI_EV_ACTION_RESULT, .request_id = i, .code = 0};
        CHECK(ui_mailbox_post_event(&mb, &e));
    }
    CHECK_EQ(mb.c.event_high_water, UI_MAILBOX_EVENT_CAP);
    // overflow: the oldest (1, 2) are dropped, a resync is due
    ui_event_t e17 = {.kind = UI_EV_ACTION_RESULT, .request_id = 17};
    ui_event_t e18 = {.kind = UI_EV_RELEASE_ALL, .request_id = 0, .code = KVM_RA_VIEW_CHANGE};
    CHECK(!ui_mailbox_post_event(&mb, &e17));
    CHECK(!ui_mailbox_post_event(&mb, &e18));
    CHECK_EQ(mb.c.events_lost, 2);
    ui_event_t bad = {.kind = UI_EV_COUNT};
    CHECK(!ui_mailbox_post_event(&mb, &bad));

    uint32_t seen = 0;
    ui_state_snapshot_t out;
    bool rs = false;
    CHECK(ui_mailbox_take(&mb, &seen, &out, &rs)); // version unchanged, but the resync forces a take
    CHECK(rs);
    CHECK(!ui_mailbox_take(&mb, &seen, &out, &rs)); // one resync per loss
    CHECK(!rs);

    ui_event_t got;
    uint32_t expect = 3;
    int n = 0;
    while (ui_mailbox_take_event(&mb, &got)) {
        if (n < UI_MAILBOX_EVENT_CAP - 1) {
            CHECK_EQ(got.request_id, expect++);
        } else {
            CHECK_EQ(got.kind, UI_EV_RELEASE_ALL);
            CHECK_EQ(got.code, KVM_RA_VIEW_CHANGE);
        }
        n++;
    }
    CHECK_EQ(n, UI_MAILBOX_EVENT_CAP);
}

static void test_lock_pairing(void)
{
    s_locks = s_unlocks = s_depth = s_max_depth = 0;
    ui_mailbox_lock_t l = {.lock = lk, .unlock = ulk, .ctx = NULL};
    ui_mailbox_t mb;
    ui_mailbox_init(&mb, &l);
    ui_hid_slice_t h = hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 1);
    ui_hid_slice_t bad = hid(9, 9, 9);
    ui_net_slice_t n = {.client_state = UI_CLIENT_CONNECTED, .wifi_connected = true};
    ui_touch_slice_t t = {.touch_healthy = false};
    ui_gesture_slice_t g = {.drag_lock_active = true};
    ui_event_t e = {.kind = UI_EV_ACTION_RESULT, .request_id = 1};
    ui_mailbox_publish_hid(&mb, &h, 1);
    ui_mailbox_publish_hid(&mb, &h, 1); // stale
    ui_mailbox_publish_hid(&mb, &bad, 2);
    ui_mailbox_publish_net(&mb, &n, 1);
    ui_mailbox_publish_touch(&mb, &t, 1);
    ui_mailbox_publish_gesture(&mb, &g, 1);
    ui_mailbox_post_event(&mb, &e);
    uint32_t seen = 0;
    ui_state_snapshot_t out;
    ui_mailbox_take(&mb, &seen, &out, NULL);
    ui_event_t got;
    ui_mailbox_take_event(&mb, &got);
    ui_mailbox_counters_t c;
    ui_mailbox_counters(&mb, &c);
    CHECK_EQ(s_locks, s_unlocks);
    CHECK_EQ(s_max_depth, 1); // never nested
    CHECK(s_locks >= 10);
    CHECK_EQ(c.published, 6);
    CHECK_EQ(c.applied, 4);
    CHECK_EQ(c.stale, 1);
    CHECK_EQ(c.invalid, 1);
    CHECK(out.drag_lock_active);
    CHECK_EQ(out.sources, UI_SRC_ALL);
}

// AT-UIFW-03 (store level): random, partly erroneous publishes with takes at random points; the
// final take always equals the model of "last accepted value per source".
static uint32_t rng(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void test_random_convergence(void)
{
    const uint32_t seeds[] = {1u, 0xC0FFEEu, 0x12345678u, 0xDEADBEEFu};
    for (unsigned si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++) {
        uint32_t r = seeds[si];
        ui_mailbox_t mb;
        ui_mailbox_init(&mb, NULL);
        ui_state_snapshot_t model;
        memset(&model, 0, sizeof(model));
        uint32_t seq[4] = {0};
        bool seen_src[4] = {false};
        uint32_t seen = 0;
        ui_state_snapshot_t ui;
        memset(&ui, 0, sizeof(ui));
        for (int i = 0; i < 20000; i++) {
            uint32_t op = rng(&r) % 10;
            uint32_t src = rng(&r) % 4;
            bool late = (rng(&r) % 5) == 0;     // 20 % late (old sequence)
            bool invalid = (rng(&r) % 10) == 0; // 10 % out of range
            uint32_t s = late && seen_src[src] ? seq[src] - (rng(&r) % 3) : seq[src] + 1 + (rng(&r) % 3);
            bool accept_seq = !seen_src[src] || (int32_t)(s - seq[src]) > 0;
            if (op < 7) {
                if (src == 0) {
                    ui_hid_slice_t h = hid((uint8_t)(rng(&r) % 6), (uint8_t)(rng(&r) % 3), rng(&r) % 50);
                    h.modifiers_down = (uint8_t)rng(&r);
                    h.mods_armed = (uint8_t)rng(&r);
                    h.mods_locked = (uint8_t)rng(&r);
                    h.caps_led_known = rng(&r) & 1;
                    h.caps_led_active = h.caps_led_known && (rng(&r) & 1);
                    if (invalid) {
                        h.host_usb_state = 6 + (uint8_t)(rng(&r) % 10);
                    }
                    ui_pub_result_t pr = ui_mailbox_publish_hid(&mb, &h, s);
                    bool ok = !invalid && accept_seq;
                    CHECK(ok ? (pr == UI_PUB_APPLIED || pr == UI_PUB_UNCHANGED)
                             : (pr == UI_PUB_STALE || pr == UI_PUB_INVALID));
                    if (ok) {
                        model.host_usb_state = h.host_usb_state;
                        model.owner_kind = h.owner_kind;
                        model.owner_epoch = h.owner_epoch;
                        model.modifiers_down = h.modifiers_down;
                        model.mods_armed = h.mods_armed;
                        model.mods_locked = h.mods_locked;
                        model.caps_led_known = h.caps_led_known;
                        model.caps_led_active = h.caps_led_active;
                    }
                } else if (src == 1) {
                    ui_net_slice_t n = {.client_state = (uint8_t)(rng(&r) % UI_CLIENT_COUNT),
                                        .wifi_connected = rng(&r) & 1};
                    if (invalid) {
                        n.client_state = UI_CLIENT_COUNT + (uint8_t)(rng(&r) % 5);
                    }
                    ui_pub_result_t pr = ui_mailbox_publish_net(&mb, &n, s);
                    bool ok = !invalid && accept_seq;
                    CHECK(ok ? pr <= UI_PUB_UNCHANGED : pr >= UI_PUB_STALE);
                    if (ok) {
                        model.client_state = n.client_state;
                        model.wifi_connected = n.wifi_connected;
                    }
                } else if (src == 2) {
                    ui_touch_slice_t t = {.touch_healthy = rng(&r) & 1};
                    invalid = false;
                    ui_mailbox_publish_touch(&mb, &t, s);
                    if (accept_seq) {
                        model.touch_healthy = t.touch_healthy;
                    }
                } else {
                    ui_gesture_slice_t g = {.drag_lock_active = rng(&r) & 1};
                    invalid = false;
                    ui_mailbox_publish_gesture(&mb, &g, s);
                    if (accept_seq) {
                        model.drag_lock_active = g.drag_lock_active;
                    }
                }
                if (!invalid && accept_seq) {
                    seq[src] = s;
                    seen_src[src] = true;
                    model.sources |= 1u << src;
                }
            } else if (op < 9) {
                ui_mailbox_take(&mb, &seen, &ui, NULL); // the UI drops intermediate versions
            } else {
                ui_event_t e = {.kind = UI_EV_ACTION_RESULT, .request_id = rng(&r)};
                ui_mailbox_post_event(&mb, &e); // overflow is allowed: state stays exact
            }
        }
        ui_mailbox_take(&mb, &seen, &ui, NULL);
        ui_state_snapshot_t cur = mb.state;
        CHECK_EQ(cur.sources, model.sources);
        CHECK_EQ(ui.host_usb_state, model.host_usb_state);
        CHECK_EQ(ui.owner_kind, model.owner_kind);
        CHECK_EQ(ui.owner_epoch, model.owner_epoch);
        CHECK_EQ(ui.modifiers_down, model.modifiers_down);
        CHECK_EQ(ui.mods_armed, model.mods_armed);
        CHECK_EQ(ui.mods_locked, model.mods_locked);
        CHECK_EQ(ui.caps_led_known, model.caps_led_known);
        CHECK_EQ(ui.caps_led_active, model.caps_led_active);
        CHECK_EQ(ui.client_state, model.client_state);
        CHECK_EQ(ui.wifi_connected, model.wifi_connected);
        CHECK_EQ(ui.touch_healthy, model.touch_healthy);
        CHECK_EQ(ui.drag_lock_active, model.drag_lock_active);
        CHECK_EQ(ui.version, cur.version);
    }
}

int main(void)
{
    printf("test_ui_mailbox\n");
    RUN(test_initial_unknown);
    RUN(test_publish_take_version);
    RUN(test_stale_and_wrap);
    RUN(test_invalid);
    RUN(test_caps_needs_known);
    RUN(test_events_ring);
    RUN(test_lock_pairing);
    RUN(test_random_convergence);
    TEST_MAIN_END("test_ui_mailbox");
}
