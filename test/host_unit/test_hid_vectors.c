// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// InputArbiter: 03 §4 invariants and the reference vectors VEC-005..009, plus the core part of
// AT-SEC-02/03 and AT-CONN-05..09 (authentication and the transport are outside hid_core).
#include <string.h>

#include "fake_hid_host.h"
#include "kvm_test.h"

static kvm_hid_core_t core;
static fake_host_t host;
static uint64_t now;

#define SESSION 0x5151u

static uint32_t grant(uint64_t session)
{
    uint32_t le = 0;
    CHECK_EQ(kvm_hid_core_remote_grant(&core, session, now, &le), KVM_HID_OK);
    fh_pump(&core, &host, &now);
    host.n = 0;
    host.nev = 0;
    return le;
}

static void remote_without_lease_sends_nothing(void)
{
    // AT-SEC-02 / AT-CONN-05: an authenticated session without a lease cannot inject.
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, 0, &a, now), KVM_HID_ERR_STALE_EPOCH);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, 1, &a, now), KVM_HID_ERR_STALE_EPOCH);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
    CHECK_EQ(kvm_hid_core_counters(&core)->rejected_epoch, 2);
}

static void remote_with_lease_works_and_checks_epoch_and_session(void)
{
    // AT-CONN-06 / AT-SEC-03.
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_REMOTE);
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    kvm_hid_intent_t u = fh_key(KVM_HID_KEY_UP, KVM_KEY_A);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &a, now), KVM_HID_OK);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &u, now), KVM_HID_OK);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION + 1, le, &a, now), KVM_HID_ERR_SESSION);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le - 1, &a, now), KVM_HID_ERR_STALE_EPOCH);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 2);
    CHECK_EQ(host.key_presses[KVM_KEY_A], 1);
}

static void remote_range_and_consumer_rules(void)
{
    // 03 §6.5: |dx| > 4096 -> ERR_RANGE, no hidden clipping; TECH-075: raw Consumer only by chord.
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    kvm_hid_intent_t m = fh_move(4097, 0);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &m, now), KVM_HID_ERR_RANGE);
    m = fh_move(-4096, 4096);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &m, now), KVM_HID_OK);
    kvm_hid_intent_t w = fh_wheel(0, -4097);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &w, now), KVM_HID_ERR_RANGE);
    kvm_hid_intent_t c = fh_consumer(KVM_HID_CONSUMER_DOWN, KVM_CONSUMER_VOLUME_UP);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &c, now), KVM_HID_ERR_USAGE);
    kvm_hid_intent_t ch = fh_chord(KVM_CHORD_MEDIA_VOLUME_UP);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &ch, now), KVM_HID_OK);
    kvm_hid_intent_t ka;
    memset(&ka, 0, sizeof(ka));
    ka.type = KVM_HID_KEEPALIVE;
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &ka, now), KVM_HID_ERR_BAD_INTENT);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.sum_dx, -4096);
    CHECK_EQ(host.sum_dy, 4096);
    CHECK_EQ(host.consumer_presses, 1);
}

static void vec005_disconnect_during_ctrl_down(void)
{
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    kvm_hid_intent_t ctrl = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_LEFT_CTRL);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &ctrl, now), KVM_HID_OK);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.mods, 0x01); // report Ctrl DOWN
    now += 4100000;            // no heartbeat: the 4 s lease expires
    kvm_hid_core_poll(&core, now);
    CHECK_EQ(kvm_hid_core_owner(&core)->epoch, le + 1); // epoch 7 -> 8
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_REMOTE_REVOKED), 1);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.mods, 0); // neutral, Ctrl UP
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &a, now), KVM_HID_ERR_STALE_EPOCH);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.key_presses[KVM_KEY_A], 0);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(kvm_hid_core_counters(&core)->release_by_reason[KVM_RA_LEASE_TIMEOUT], 1);
}

static void stale_heartbeat_blocks_before_lease_expiry(void)
{
    // 03 §4.3 / AT-CONN-08: no sending 3 s after the last heartbeat; revoke at the lease end.
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    kvm_hid_intent_t b = fh_btn(KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &b, now), KVM_HID_OK);
    fh_pump(&core, &host, &now);
    now += 3100000;
    kvm_hid_intent_t m = fh_move(1, 0);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &m, now), KVM_HID_ERR_CLIENT_STALE);
    now += 1000000;
    kvm_hid_core_poll(&core, now);
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK(s.ledger_empty);
    CHECK_EQ(s.owner_kind, OWNER_NONE);
}

static void heartbeat_extends_lease(void)
{
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    for (int i = 0; i < 10; i++) {
        now += 2000000;
        kvm_hid_core_remote_heartbeat(&core, SESSION, now);
        kvm_hid_core_poll(&core, now);
    }
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_REMOTE);
    kvm_hid_intent_t m = fh_move(3, 3);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &m, now), KVM_HID_OK);
    kvm_hid_core_remote_heartbeat(&core, SESSION + 9, now + 3900000); // foreign session: ignored
    now += 4000000;
    kvm_hid_core_poll(&core, now);
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_NONE);
}

static void vec006_mode_switch_under_drag(void)
{
    // Core side of VEC-006: drag lock (LEFT DOWN + moves), then the view change orders a release.
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t d = fh_btn(KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT);
    kvm_hid_intent_t m = fh_move(12, -4);
    kvm_hid_core_local(&core, &d, now);
    kvm_hid_core_local(&core, &m, now);
    fh_pump(&core, &host, &now);
    kvm_hid_core_release_all(&core, KVM_RA_VIEW_CHANGE);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.button_presses[0], 1); // exactly one LEFT DOWN
    int downs = 0, ups = 0;
    uint8_t prev = 0;
    for (int i = 0; i < host.n; i++) {
        if (host.log[i].itf != KVM_HID_ITF_MOUSE) {
            continue;
        }
        uint8_t b = host.log[i].b[0] & 1;
        downs += b && !prev;
        ups += !b && prev;
        if ((int8_t)host.log[i].b[1] != 0) {
            CHECK_EQ(b, 1); // moves carry the LEFT bit
        }
        prev = b;
    }
    CHECK_EQ(downs, 1);
    CHECK_EQ(ups, 1);
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK(s.ledger_empty);
}

static void vec007_saturated_queue(void)
{
    fh_setup(&core, &host, &now);
    host.refuse[KVM_HID_ITF_MOUSE] = true; // the HOST is slow: nothing drains during injection
    long sent_dx = 0;
    for (int i = 0; i < 300; i++) {
        kvm_hid_intent_t m = fh_move(2, 1);
        kvm_hid_core_local(&core, &m, now);
        sent_dx += 2;
    }
    CHECK_EQ(core.q_count, 1); // coalesced
    kvm_hid_intent_t cd = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_LEFT_CTRL);
    kvm_hid_core_local(&core, &cd, now);
    kvm_hid_intent_t ra;
    memset(&ra, 0, sizeof(ra));
    ra.type = KVM_HID_RELEASE_ALL;
    CHECK_EQ(kvm_hid_core_local(&core, &ra, now), KVM_HID_OK); // cannot be dropped: executed at once
    kvm_hid_intent_t cu = fh_key(KVM_HID_KEY_UP, KVM_KEY_LEFT_CTRL);
    kvm_hid_core_local(&core, &cu, now);
    host.refuse[KVM_HID_ITF_MOUSE] = false;
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.key_presses[KVM_KEY_LEFT_CTRL], 0); // the queued Ctrl DOWN was superseded
    CHECK_EQ(kvm_hid_core_counters(&core)->keyup_without_down, 1);
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK(s.ledger_empty);
    // Overflow counting with a saturated non-coalescible queue.
    host.refuse[KVM_HID_ITF_KEYBOARD] = true;
    for (int i = 0; i < KVM_HID_QUEUE_CAP; i++) {
        kvm_hid_intent_t k = fh_key((i & 1) ? KVM_HID_KEY_UP : KVM_HID_KEY_DOWN, (uint8_t)(KVM_KEY_A + 1));
        kvm_hid_core_local(&core, &k, now);
    }
    kvm_hid_intent_t m = fh_move(1, 1);
    kvm_hid_core_local(&core, &m, now); // a full queue drops pure motion (counted), never a key
    CHECK(kvm_hid_core_counters(&core)->moves_dropped >= 1);
    CHECK_EQ(kvm_hid_core_counters(&core)->queue_overflows, 0);
    host.refuse[KVM_HID_ITF_KEYBOARD] = false;
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    (void)sent_dx;
}

static void vec008_unplug_and_reconnect(void)
{
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t cd = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_LEFT_CTRL);
    kvm_hid_core_local(&core, &cd, now);
    fh_pump(&core, &host, &now);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, 1, now);
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK(s.ledger_empty); // ledger zero immediately
    fh_init(&host);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 2, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 3); // neutral report only
    CHECK_EQ(host.key_presses[KVM_KEY_LEFT_CTRL], 0); // no replayed Ctrl DOWN
    CHECK(fh_host_neutral(&host));
}

static void vec009_local_takeover_while_remote_drags(void)
{
    // AT-CONN-07.
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    kvm_hid_intent_t d = fh_btn(KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT);
    kvm_hid_intent_t m = fh_move(30, 0);
    kvm_hid_core_remote(&core, SESSION, le, &d, now);
    kvm_hid_core_remote(&core, SESSION, le, &m, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.buttons, 0x01);
    kvm_hid_core_remote(&core, SESSION, le, &m, now); // queued, not yet sent
    int before = host.n;
    kvm_hid_intent_t lm = fh_move(-5, 0);
    CHECK_EQ(kvm_hid_core_local(&core, &lm, now), KVM_HID_OK);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_REMOTE_REVOKED), 1);
    CHECK(kvm_hid_core_owner(&core)->epoch >= le + 1);
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_LOCAL);
    fh_pump(&core, &host, &now);
    // First mouse report after the override is LEFT UP; then the local move only (the queued REMOTE
    // move is gone).
    int first = -1;
    for (int i = before; i < host.n && first < 0; i++) {
        if (host.log[i].itf == KVM_HID_ITF_MOUSE) {
            first = i;
        }
    }
    CHECK(first >= 0);
    CHECK_EQ(host.log[first].b[0], 0);
    CHECK_EQ((int8_t)host.log[first].b[1], 0);
    long dx_after = 0;
    for (int i = before; i < host.n; i++) {
        if (host.log[i].itf == KVM_HID_ITF_MOUSE) {
            dx_after += (int8_t)host.log[i].b[1];
            CHECK_EQ(host.log[i].b[0], 0);
        }
    }
    CHECK_EQ(dx_after, -5);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &m, now), KVM_HID_ERR_STALE_EPOCH); // no post-override events
}

static void control_request_policy(void)
{
    // INV-003 / INV-004: LOCAL active blocks a request; a second session cannot steal a lease.
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    kvm_hid_core_local(&core, &a, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_remote_request(&core, SESSION, now), KVM_HID_ERR_CONTROL_DENIED); // key held
    kvm_hid_intent_t u = fh_key(KVM_HID_KEY_UP, KVM_KEY_A);
    kvm_hid_core_local(&core, &u, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_remote_request(&core, SESSION, now), KVM_HID_ERR_CONTROL_DENIED); // not idle yet
    now += 2100000;
    CHECK_EQ(kvm_hid_core_remote_request(&core, SESSION, now), KVM_HID_OK);
    grant(SESSION);
    CHECK_EQ(kvm_hid_core_remote_request(&core, SESSION + 1, now), KVM_HID_ERR_CONTROL_DENIED);
    uint32_t le2 = 0;
    CHECK_EQ(kvm_hid_core_remote_grant(&core, SESSION + 1, now, &le2), KVM_HID_ERR_CONTROL_DENIED);
    kvm_hid_core_set_maintenance(&core, true);
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_NONE); // maintenance takes control back
    CHECK_EQ(kvm_hid_core_remote_request(&core, SESSION, now), KVM_HID_ERR_OTA_IN_PROGRESS);
    kvm_hid_core_set_maintenance(&core, false);
}

static void remote_release_and_disconnect(void)
{
    // AT-FAULT-06: session close in the middle of a chord -> release, no resumed input.
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    kvm_hid_intent_t c = fh_chord(KVM_CHORD_CTRL_ALT_DEL);
    kvm_hid_core_remote(&core, SESSION, le, &c, now);
    now += 1000;
    kvm_hid_core_poll(&core, now); // phase 1 out
    kvm_hid_core_remote_disconnect(&core, SESSION);
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.key_presses[KVM_KEY_DELETE], 0);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_CHORD_ABORTED), 1);
    CHECK_EQ(kvm_hid_core_counters(&core)->release_by_reason[KVM_RA_REMOTE_DISCONNECT], 1);
    // Explicit release from the CLIENT.
    now += 3000000;
    le = grant(SESSION);
    kvm_hid_intent_t b = fh_btn(KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_RIGHT);
    kvm_hid_core_remote(&core, SESSION, le, &b, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.buttons, 0x02);
    kvm_hid_core_remote_release(&core, SESSION + 1); // a foreign session cannot release
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_REMOTE);
    kvm_hid_core_remote_release(&core, SESSION);
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_NONE);
}

static void remote_while_suspended_or_offline(void)
{
    fh_setup(&core, &host, &now);
    uint32_t le = grant(SESSION);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_SUSPENDED, 1, now);
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &a, now), KVM_HID_ERR_USB_SUSPENDED);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, 1, now);
    CHECK_EQ(kvm_hid_core_remote(&core, SESSION, le, &a, now), KVM_HID_ERR_HOST_OFFLINE);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
}

int main(void)
{
    printf("test_hid_vectors\n");
    RUN(remote_without_lease_sends_nothing);
    RUN(remote_with_lease_works_and_checks_epoch_and_session);
    RUN(remote_range_and_consumer_rules);
    RUN(vec005_disconnect_during_ctrl_down);
    RUN(stale_heartbeat_blocks_before_lease_expiry);
    RUN(heartbeat_extends_lease);
    RUN(vec006_mode_switch_under_drag);
    RUN(vec007_saturated_queue);
    RUN(vec008_unplug_and_reconnect);
    RUN(vec009_local_takeover_while_remote_drags);
    RUN(control_request_policy);
    RUN(remote_release_and_disconnect);
    RUN(remote_while_suspended_or_offline);
    TEST_MAIN_END("test_hid_vectors");
}
