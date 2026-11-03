// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Normative test vectors of spec 03 §8 at the protocol layer: VEC-005, VEC-007, VEC-008, VEC-009.
// Plus AT-CONN-07, AT-CONN-09 and AT-FAULT-06. The ledger itself belongs to hid_core; what is
// asserted here is that no intent escapes the protocol layer after a loss of lease, host or
// session, and that every loss path asks for RELEASE_ALL with the right reason.
#include "kvm_test.h"
#include "rp_harness.h"

#define SID 0x5001u

static rph_cli_t c;

static int count_type(kvm_hid_action_type_t t)
{
    int n = 0;
    int limit = (g_h.n_intent < RPH_MAX_INTENTS) ? g_h.n_intent : RPH_MAX_INTENTS;
    for (int i = 0; i < limit; i++) {
        if (g_h.intent[i].type == t) {
            n++;
        }
    }
    return n;
}

static long sum_dx(void)
{
    long s = 0;
    int limit = (g_h.n_intent < RPH_MAX_INTENTS) ? g_h.n_intent : RPH_MAX_INTENTS;
    for (int i = 0; i < limit; i++) {
        if (g_h.intent[i].type == KVM_HID_MOUSE_MOVE) {
            s += g_h.intent[i].data.mouse_move.dx;
        }
    }
    return s;
}

// VEC-005: disconnect during Ctrl DOWN
//   host=mounted; owner=remote(epoch=E)
//   input(seq=1, usage=0xE0, down=true) -> Ctrl DOWN
//   session timeout -> epoch=E+1, RELEASE_ALL
//   input(seq=2, lease_epoch=E, usage=0x04, down=true) -> ERR_STALE_EPOCH
//   Expected: no "Ctrl+A" on the HOST, no stuck Ctrl.
static void test_vec005_disconnect_during_ctrl_down(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    uint32_t ep = rph_cli_grant(&c);

    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK_EQ(g_h.intent[0].type, KVM_HID_KEY_DOWN);
    CHECK_EQ(g_h.intent[0].data.key.usage_id, KVM_KEY_LEFT_CTRL);

    // the heartbeat stops: 3 s later the lease is gone, RELEASE_ALL is asked for and the epoch moves
    g_h.now += 3100000u;
    kvm_rp_poll(&g_h.rp, g_h.now);
    CHECK(rph_released_with(KVM_RA_LEASE_TIMEOUT));
    CHECK(g_h.rp.owner_epoch > ep);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    CHECK(rph_any_has("client_stale"));

    // the next key from the old epoch is refused: no 'A' is ever pressed, so no Ctrl+A
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK_EQ(count_type(KVM_HID_KEY_DOWN), 1);
    // and the client cannot "finish" the old press either
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':false"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(g_h.n_intent, 1);
}

// VEC-007: saturated queue
//   300 pointer moves + Ctrl DOWN + release + Ctrl UP
//   Expected: pointer deltas coalesced, the release cannot be dropped, the final state empty,
//   the overflow counted.
static void test_vec007_saturated_queue(void)
{
    // A saturated queue, not an attack: the flood threshold is raised so the vector exercises
    // coalescing instead of the revoke-on-flood rule, which test_rate_flood_revokes covers.
    kvm_rp_config_t cfg = KVM_RP_CONFIG_DEFAULT;
    cfg.input_flood_limit = 4000u;
    rph_reset(&cfg);
    rph_cli_open(&c, SID, true);
    uint32_t ep = rph_cli_grant(&c);

    for (int i = 0; i < 300; i++) {
        kvm_rp_err_t e = rph_input_ep(&c, ep, "'kind':'mouse.move','dx':3,'dy':0");
        CHECK(e == KVM_RP_OK); // movement is never rejected, only merged
    }
    CHECK_EQ(g_h.rp.owner_kind, OWNER_REMOTE); // merging, not revoking
    CHECK(kvm_rp_counters(&g_h.rp)->input_coalesced > 0);
    CHECK(g_h.n_intent < 300); // actually coalesced

    // drain the accumulator, keeping the lease alive, and verify the sum survived (03 §3 rule 3)
    for (int k = 0; k < 12; k++) {
        g_h.now += 200000u;
        rph_ping(&c);
        kvm_rp_poll(&g_h.rp, g_h.now);
    }
    CHECK_EQ(sum_dx(), 900);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_REMOTE);

    // the window has rolled, so an ordinary key press is admitted again
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);

    // the release path cannot be dropped, whatever the queue looks like
    CHECK_EQ(rph_cmd(&c, "control.release", ""), KVM_RP_OK);
    CHECK(rph_released_with(KVM_RA_REMOTE_RELEASE));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    // the trailing Ctrl UP from the old epoch changes nothing: the ledger was already neutralized
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':false"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(count_type(KVM_HID_RELEASE_ALL), 0); // the release is a reason, never a forged intent

    // a saturated arbiter queue is reported, not hidden
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    ep = rph_cli_grant(&c);
    g_h.sink_result = KVM_RP_ERR_HID_QUEUE_OVERFLOW;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':1,'dy':1"),
             KVM_RP_ERR_HID_QUEUE_OVERFLOW);
    CHECK(rph_any_has("ERR_HID_QUEUE_OVERFLOW"));
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->errors[KVM_RP_ERR_HID_QUEUE_OVERFLOW], 1);
}

// VEC-008 / AT-CONN-09: host unplugged and reconnected
//   active Ctrl DOWN; USB umount -> ledger zero; USB mount -> neutral report only
//   Expected: no replayed Ctrl DOWN, no phantom shortcut.
static void test_vec008_host_umount_mount(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    uint32_t ep = rph_cli_grant(&c);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);

    // USB disappears: the arbiter neutralizes and bumps the epoch; the protocol is told
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_DETACHED, false, true, g_h.now);
    kvm_rp_notify_revoked(&g_h.rp, SID, KVM_RP_REVOKE_HOST_OFFLINE, ep + 1u, g_h.now);
    CHECK(rph_any_has("host_offline"));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);

    // nothing can be sent while the HOST is away
    CHECK_EQ(rph_input_ep(&c, ep + 1u, "'kind':'keyboard.key','usage_id':4,'down':true"),
             KVM_RP_ERR_HOST_OFFLINE);
    CHECK_EQ(rph_input_ep(&c, ep + 1u, "'kind':'shortcut','command_id':'ctrl_alt_del'"),
             KVM_RP_ERR_HOST_OFFLINE);

    // the HOST comes back: the client has no lease, so no old DOWN and no phantom chord
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_MOUNTED, false, true, g_h.now);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':true"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(rph_input_ep(&c, ep + 1u, "'kind':'keyboard.key','usage_id':224,'down':true"),
             KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK_EQ(count_type(KVM_HID_CHORD), 0);

    // after a fresh grant the shadow state starts empty: the first DOWN is delivered, not swallowed
    uint32_t ep2 = rph_cli_grant(&c);
    CHECK(ep2 > ep);
    CHECK_EQ(rph_input_ep(&c, ep2, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 2);
}

// VEC-009 / AT-CONN-07: local takeover while remote dragging
//   REMOTE LEFT DOWN, REMOTE MOVE; LOCAL touch input
//   Expected: REMOTE revoke + LEFT UP, then local input; no REMOTE post-override events; epoch +1.
static void test_vec009_local_takeover_during_drag(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    uint32_t ep = rph_cli_grant(&c);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'left','down':true"),
             KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':20,'dy':5"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 2);

    // the arbiter performs revoke + release + epoch bump for the local action (ADR-011)
    uint32_t new_ep = ep + 1u;
    kvm_rp_set_owner(&g_h.rp, OWNER_LOCAL, 0, new_ep, g_h.now);
    kvm_rp_notify_revoked(&g_h.rp, SID, KVM_RP_REVOKE_LOCAL_OVERRIDE, new_ep, g_h.now);
    CHECK(rph_any_has("local_override"));
    CHECK_EQ(kvm_rp_release_reason(KVM_RP_REVOKE_LOCAL_OVERRIDE), KVM_RA_LOCAL_OVERRIDE);
    CHECK_EQ(g_h.rp.owner_epoch, new_ep);

    // no REMOTE event is accepted after the override, with the old or the new epoch
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':50,'dy':0"), KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(rph_input_ep(&c, new_ep, "'kind':'mouse.move','dx':50,'dy':0"),
             KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(rph_input_ep(&c, new_ep, "'kind':'mouse.button','button':'left','down':false"),
             KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 2);

    // a queued coalesced move cannot leak out after the override either
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    ep = rph_cli_grant(&c);
    for (int i = 0; i < 140; i++) {
        (void)rph_input_ep(&c, ep, "'kind':'mouse.move','dx':7,'dy':0");
    }
    CHECK(kvm_rp_counters(&g_h.rp)->input_coalesced > 0);
    int before = g_h.n_intent;
    kvm_rp_set_owner(&g_h.rp, OWNER_LOCAL, 0, ep + 1u, g_h.now);
    for (int k = 0; k < 5; k++) {
        g_h.now += 1000000u;
        kvm_rp_poll(&g_h.rp, g_h.now);
    }
    CHECK_EQ(g_h.n_intent, before); // the accumulator was dropped with the lease
}

// AT-FAULT-06: session close in mid-keyboard chord -> release all, no resumed old input
static void test_at_fault_06_close_during_chord(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    uint32_t ep = rph_cli_grant(&c);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'shortcut','command_id':'alt_tab'"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 2);

    // the connection dies while the chord phases are still being played out by the scheduler
    kvm_rp_session_close(&g_h.rp, SID, KVM_RP_REVOKE_SESSION_CLOSED, g_h.now);
    CHECK(rph_released_with(KVM_RA_REMOTE_DISCONNECT));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);

    // the client reconnects and tries to continue the old sequence
    rph_cli_t again;
    rph_cli_open(&again, SID + 1u, true);
    CHECK_EQ(rph_input_ep(&again, ep, "'kind':'keyboard.key','usage_id':43,'down':true"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(rph_input_ep(&again, g_h.rp.owner_epoch, "'kind':'keyboard.key','usage_id':224,"
                                                      "'down':false"),
             KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 2); // nothing resumed
}

// Out-of-order and duplicated frames inside one session (03 §6.4 note)
static void test_out_of_order_frames(void)
{
    // The abuse limit is raised here so the whole replay series can be observed; the close path
    // itself is covered by test_abuse_closes_session.
    kvm_rp_config_t cfg = KVM_RP_CONFIG_DEFAULT;
    cfg.bad_frame_limit = 64u;
    rph_reset(&cfg);
    rph_cli_open(&c, SID, true);
    uint32_t ep = rph_cli_grant(&c);
    uint64_t base = c.q;

    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                              "'payload':{'kind':'mouse.move','dx':1,'dy':1}}",
                              (unsigned long long)(base + 10u), c.label, ep)),
             KVM_RP_OK);
    // everything at or below the high-water mark is a replay from now on
    for (uint64_t k = 0; k <= 10u; k++) {
        CHECK_EQ(rph_feed(SID,
                          jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                             "'payload':{'kind':'mouse.move','dx':1,'dy':1}}",
                             (unsigned long long)(base + k), c.label, ep)),
                 KVM_RP_ERR_REPLAY);
    }
    CHECK_EQ(g_h.n_intent, 1);
    // a gap in the sequence is legal: WSS is ordered, the application may simply skip numbers
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                              "'payload':{'kind':'mouse.move','dx':1,'dy':1}}",
                              (unsigned long long)(base + 1000u), c.label, ep)),
             KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 2);
}

// Every loss path asks for RELEASE_ALL with the reason the arbiter expects (03 §4.2, TECH-005)
static void test_every_loss_path_releases(void)
{
    struct {
        kvm_rp_revoke_reason_t reason;
        kvm_release_reason_t expect;
    } cases[] = {
        {KVM_RP_REVOKE_CLIENT_RELEASE, KVM_RA_REMOTE_RELEASE},
        {KVM_RP_REVOKE_CLIENT_STALE, KVM_RA_LEASE_TIMEOUT},
        {KVM_RP_REVOKE_SESSION_CLOSED, KVM_RA_REMOTE_DISCONNECT},
        {KVM_RP_REVOKE_LOCAL_OVERRIDE, KVM_RA_LOCAL_OVERRIDE},
        {KVM_RP_REVOKE_HOST_OFFLINE, KVM_RA_USB_LOSS},
        {KVM_RP_REVOKE_MAINTENANCE, KVM_RA_MAINTENANCE},
        {KVM_RP_REVOKE_RATE_FLOOD, KVM_RA_REMOTE_DISCONNECT},
        {KVM_RP_REVOKE_PROTOCOL_ABUSE, KVM_RA_REMOTE_DISCONNECT},
        {KVM_RP_REVOKE_UNPAIRED, KVM_RA_REMOTE_DISCONNECT},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK_EQ(kvm_rp_release_reason(cases[i].reason), cases[i].expect);
        rph_reset(NULL);
        rph_cli_open(&c, SID, true);
        (void)rph_cli_grant(&c);
        kvm_rp_session_close(&g_h.rp, SID, cases[i].reason, g_h.now);
        CHECK_EQ(g_h.n_release, 1);
        CHECK_EQ(g_h.release[0], cases[i].expect);
        CHECK_EQ(kvm_rp_counters(&g_h.rp)->release_requests[cases[i].expect], 1);
        CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    }
    // an observer session closing never asks for a release: it never held the lease
    rph_reset(NULL);
    rph_cli_open(&c, SID, false);
    kvm_rp_session_close(&g_h.rp, SID, KVM_RP_REVOKE_SESSION_CLOSED, g_h.now);
    CHECK_EQ(g_h.n_release, 0);
}

int main(void)
{
    printf("remote_vectors\n");
    RUN(test_vec005_disconnect_during_ctrl_down);
    RUN(test_vec007_saturated_queue);
    RUN(test_vec008_host_umount_mount);
    RUN(test_vec009_local_takeover_during_drag);
    RUN(test_at_fault_06_close_during_chord);
    RUN(test_out_of_order_frames);
    RUN(test_every_loss_path_releases);
    TEST_MAIN_END("remote_vectors");
}
