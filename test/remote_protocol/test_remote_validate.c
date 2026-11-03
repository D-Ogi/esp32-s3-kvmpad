// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// RemoteProtocolValidator (TECH-043): one test per row of spec 03 §6.5, with the error codes of
// 03 §6.4/§7. Also covers AT-SEC-02 (no lease -> 0 HID), AT-SEC-03 (replay / stale epoch),
// AT-SEC-04 (malformed and oversized frames), AT-SEC-07 (no OTA/reset/exec surface) and
// AT-EXT-19 (`settings.*` refused) at the software level.
#include "kvm_test.h"
#include "rp_harness.h"

#define SID 0x1001u

static rph_cli_t c;

/** Owner session with a valid lease, host mounted. */
static void setup_owner(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    (void)rph_cli_grant(&c);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_REMOTE);
}

// ---- row: v != 1 ---------------------------------------------------------------------------

static void test_version(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    CHECK_EQ(rph_feed(SID, jf("{'v':2,'type':'ping','seq':5,'session_id':'%s','payload':{}}",
                              c.label)),
             KVM_RP_ERR_VERSION);
    CHECK(rph_any_has("ERR_VERSION"));
    CHECK_EQ(g_h.n_close, 1); // 03 §6.5: disconnect, never a silent downgrade
    CHECK_EQ(g_h.n_intent, 0);

    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    CHECK_EQ(rph_feed(SID, jf("{'v':0,'type':'ping','seq':5,'session_id':'%s'}", c.label)),
             KVM_RP_ERR_VERSION);
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    CHECK_EQ(rph_feed(SID, jf("{'v':'1','type':'ping','seq':5,'session_id':'%s'}", c.label)),
             KVM_RP_ERR_BAD_FRAME); // a string is not version 1
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    CHECK_EQ(rph_feed(SID, jf("{'type':'ping','seq':5,'session_id':'%s'}", c.label)),
             KVM_RP_ERR_BAD_FRAME); // missing v
}

// ---- row: >1024 B, malformed JSON, large depth ---------------------------------------------

static void test_bad_frame(void)
{
    setup_owner();
    char big[2048];
    memset(big, 'x', sizeof(big));
    CHECK_EQ(kvm_rp_on_frame(&g_h.rp, SID, big, 1025, g_h.now), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(kvm_rp_on_frame(&g_h.rp, SID, big, 0, g_h.now), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->frames_oversize, 2);

    static const char *const bad[] = {
        "{",
        "not json",
        "[1,2,3]",                                    // root is not an object
        "{'v':1,'type':'ping','seq':3,'extra':1}",    // unknown envelope member
        "{'v':1,'type':'ping','seq':3,'session_id':'%s','payload':5}", // not an object
        "{'v':1,'type':'ping','seq':0}",              // seq must be >= 1
        "{'v':1,'type':'ping','seq':-4}",
        "{'v':1,'type':'ping','seq':1.5}",
        "{'v':1,'type':'ping'}",                      // missing seq
        "{'v':1,'seq':3}",                            // missing type
        "{'v':1,'type':5,'seq':3}",                   // type is not a string
        "{'v':1,'type':'ping','seq':9007199254740992}", // beyond 2^53-1
        NULL,
    };
    for (int i = 0; bad[i] != NULL; i++) {
        rph_reset(NULL);
        rph_cli_open(&c, SID, true);
        (void)rph_cli_grant(&c);
        int before = g_h.n_intent;
        CHECK_EQ(rph_feed(SID, jf(bad[i], c.label)), KVM_RP_ERR_BAD_FRAME);
        CHECK_EQ(g_h.n_intent, before); // no HID from an invalid frame
    }

    // deep nesting inside payload
    setup_owner();
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'input','seq':9,'session_id':'%s','lease_epoch':%u,"
                              "'payload':{'kind':{'a':{'b':{'c':1}}}}}",
                              c.label, g_h.rp.owner_epoch)),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(g_h.n_intent, 0);
}

static void test_unknown_type_and_settings(void)
{
    // SEC-107 / AT-EXT-19: protocol v1 has no settings.* command at all
    static const char *const types[] = {"settings.set",  "settings.get", "ota.start", "exec",
                                        "factory.reset", "input.ack",    "pong",      "hello.ack",
                                        "status.snapshot", "", NULL};
    for (int i = 0; types[i] != NULL; i++) {
        rph_reset(NULL);
        rph_cli_open(&c, SID, true);
        CHECK_EQ(rph_cmd(&c, types[i], ""), KVM_RP_ERR_BAD_FRAME);
        CHECK_EQ(g_h.n_intent, 0);
    }
    // AT-SEC-07: no API exists for OTA / reset / exec, so nothing can be permitted either
    CHECK(!rph_any_has("ota"));
}

// ---- row: dx/dy outside [-4096,4096] -------------------------------------------------------

static void test_range(void)
{
    setup_owner();
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':4096,'dy':-4096"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK_EQ(g_h.intent[0].data.mouse_move.dx, 4096);
    CHECK_EQ(g_h.intent[0].data.mouse_move.dy, -4096);

    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':4097,'dy':0"), KVM_RP_ERR_RANGE);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':0,'dy':-4097"), KVM_RP_ERR_RANGE);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':99999999,'dy':0"), KVM_RP_ERR_RANGE);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1.5,'dy':0"), KVM_RP_ERR_RANGE);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':'4','dy':0"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1"), KVM_RP_ERR_BAD_FRAME); // dy missing
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1,'dy':2,'dz':3"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(g_h.n_intent, 1); // 03 §6.5: no hidden clipping, and no HID either
    CHECK(rph_any_has("ERR_RANGE"));

    // the wheel carries the same per-event bound (ADR-030)
    CHECK_EQ(rph_input(&c, "'kind':'mouse.wheel','vertical':1"), KVM_RP_OK);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.wheel','vertical':-4096,'horizontal':4096"), KVM_RP_OK);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.wheel','vertical':4097"), KVM_RP_ERR_RANGE);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.wheel'"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(g_h.n_intent, 3);
    CHECK_EQ(g_h.intent[2].type, KVM_HID_MOUSE_WHEEL);
    CHECK_EQ(g_h.intent[2].data.wheel.vertical, -4096);
    CHECK_EQ(g_h.intent[2].data.wheel.horizontal, 4096);
}

// ---- row: usage_id outside 0..255 / unknown key ---------------------------------------------

static void test_usage(void)
{
    setup_owner();
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':4,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.intent[0].type, KVM_HID_KEY_DOWN);
    CHECK_EQ(g_h.intent[0].data.key.usage_id, 0x04);
    CHECK_EQ(g_h.intent[0].origin, KVM_ORIGIN_REMOTE);

    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':256,'down':true"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':-1,'down':true"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':0,'down':true"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':3,'down':true"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':165,'down':true"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':223,'down':true"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':4"), KVM_RP_ERR_BAD_FRAME); // no down
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':4,'down':'yes'"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK(rph_any_has("ERR_USAGE"));

    // modifiers and the top of the regular range are valid
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':231,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':164,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 4);

    // buttons
    CHECK_EQ(rph_input(&c, "'kind':'mouse.button','button':'left','down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.intent[4].data.mouse_button.button_id, KVM_MOUSE_BTN_LEFT);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.button','button':'middle','down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.intent[5].data.mouse_button.button_id, KVM_MOUSE_BTN_MIDDLE);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.button','button':'x1','down':true"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.button','button':4,'down':true"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(g_h.n_intent, 6);
}

static void test_shortcut_allowlist(void)
{
    setup_owner();
    // 03 §3 rule 2 / TECH-075: a command id from the compiled allowlist, never a key array
    CHECK_EQ(rph_input(&c, "'kind':'shortcut','command_id':'ctrl_alt_del'"), KVM_RP_OK);
    CHECK_EQ(g_h.intent[0].type, KVM_HID_CHORD);
    CHECK_EQ(g_h.intent[0].data.chord.chord_id, KVM_CHORD_CTRL_ALT_DEL);
    CHECK_EQ(rph_input(&c, "'kind':'shortcut','command_id':'media_volume_up'"), KVM_RP_OK);
    CHECK_EQ(g_h.intent[1].data.chord.chord_id, KVM_CHORD_MEDIA_VOLUME_UP);

    CHECK_EQ(rph_input(&c, "'kind':'shortcut','command_id':'rm_-rf'"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'shortcut','command_id':''"), KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_input(&c, "'kind':'shortcut','command_id':1"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(rph_input(&c, "'kind':'shortcut','keys':[224,4]"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(g_h.n_intent, 2);
}

// ---- row: seq <= last_seq ------------------------------------------------------------------

static void test_replay(void)
{
    setup_owner();
    uint64_t good = c.q + 1u;
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                              "'payload':{'kind':'keyboard.key','usage_id':4,'down':true}}",
                              (unsigned long long)good, c.label, g_h.rp.owner_epoch)),
             KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);
    // the identical frame again
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                              "'payload':{'kind':'keyboard.key','usage_id':4,'down':true}}",
                              (unsigned long long)good, c.label, g_h.rp.owner_epoch)),
             KVM_RP_ERR_REPLAY);
    // an older seq
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'ping','seq':2,'session_id':'%s'}", c.label)),
             KVM_RP_ERR_REPLAY);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK(rph_any_has("ERR_REPLAY"));
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->errors[KVM_RP_ERR_REPLAY], 2);
    // the counter is not advanced by a rejected frame, so the next legitimate seq still works
    c.q = good;
    CHECK_EQ(rph_cmd(&c, "ping", ""), KVM_RP_OK);
}

static void test_replay_after_reconnect(void)
{
    // A reconnect resets the seq counter (03 §6.2), so replay protection comes from the new
    // session label and the stale lease epoch (03 §6.4 note).
    setup_owner();
    char old_label[KVM_RP_LABEL_MAX];
    snprintf(old_label, sizeof(old_label), "%s", c.label);
    uint32_t old_epoch = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':4,'down':true"), KVM_RP_OK);
    uint64_t replayed = c.q;

    kvm_rp_session_close(&g_h.rp, SID, KVM_RP_REVOKE_SESSION_CLOSED, g_h.now);
    CHECK(rph_released_with(KVM_RA_REMOTE_DISCONNECT));
    kvm_rp_set_owner(&g_h.rp, OWNER_NONE, 0, g_h.epoch, g_h.now);

    rph_cli_t c2;
    rph_cli_open(&c2, SID + 1u, true);
    int before = g_h.n_intent;
    // the old frame, replayed verbatim on the new connection
    CHECK_EQ(rph_feed(SID + 1u,
                      jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                         "'payload':{'kind':'keyboard.key','usage_id':4,'down':true}}",
                         (unsigned long long)replayed, old_label, old_epoch)),
             KVM_RP_ERR_SESSION);
    // the same frame with the new label but the old epoch and no lease
    CHECK_EQ(rph_feed(SID + 1u,
                      jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                         "'payload':{'kind':'keyboard.key','usage_id':4,'down':true}}",
                         (unsigned long long)(replayed + 1u), c2.label, old_epoch)),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(g_h.n_intent, before);
}

// ---- row: lease_epoch != owner.epoch -------------------------------------------------------

static void test_stale_epoch(void)
{
    setup_owner();
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep - 1u, "'kind':'keyboard.key','usage_id':5,'down':true"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(rph_input_ep(&c, ep + 1u, "'kind':'keyboard.key','usage_id':5,'down':true"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(rph_input_ep(&c, 0, "'kind':'keyboard.key','usage_id':5,'down':true"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK(rph_any_has("ERR_STALE_EPOCH"));

    // lease_epoch is mandatory for input and must be a uint32
    c.q++;
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s',"
                              "'payload':{'kind':'mouse.move','dx':1,'dy':1}}",
                              (unsigned long long)c.q, c.label)),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(rph_input_ep(&c, 0, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_STALE_EPOCH);
    c.q++;
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s',"
                              "'lease_epoch':4294967296,'payload':{'kind':'mouse.move','dx':1,"
                              "'dy':1}}",
                              (unsigned long long)c.q, c.label)),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(g_h.n_intent, 1);
}

// ---- row: session_id not matching the connection -------------------------------------------

static void test_session_mismatch(void)
{
    setup_owner();
    c.q++;
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'ping','seq':%llu,'session_id':'c-ffff'}",
                              (unsigned long long)c.q)),
             KVM_RP_ERR_SESSION);
    c.q++;
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'ping','seq':%llu}", (unsigned long long)c.q)),
             KVM_RP_ERR_SESSION); // missing label after hello
    c.q++;
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'ping','seq':%llu,'session_id':5}",
                              (unsigned long long)c.q)),
             KVM_RP_ERR_SESSION);
    CHECK(rph_any_has("ERR_SESSION"));
    CHECK_EQ(g_h.n_intent, 0);

    // an unknown connection gets no reply at all
    int frames = g_h.n_frame;
    CHECK_EQ(rph_feed(0x9999u, "{\"v\":1,\"type\":\"ping\",\"seq\":1}"), KVM_RP_ERR_SESSION);
    CHECK_EQ(g_h.n_frame, frames);

    // commands before hello are refused
    rph_reset(NULL);
    kvm_rp_session_info_t info;
    memset(&info, 0, sizeof(info));
    info.session_id = SID;
    info.authenticated = true;
    info.operator_role = true;
    CHECK(kvm_rp_session_open(&g_h.rp, &info, g_h.now) >= 0);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"status.get\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_SESSION);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":2,\"payload\":{}}"), KVM_RP_OK);
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'hello','seq':3,'session_id':'%s','payload':{}}",
                              kvm_rp_session_label(&g_h.rp, SID))),
             KVM_RP_ERR_BAD_FRAME); // a second hello
}

// ---- row: invalid auth ---------------------------------------------------------------------

static void test_unauthenticated(void)
{
    rph_reset(NULL);
    kvm_rp_session_info_t info;
    memset(&info, 0, sizeof(info));
    info.session_id = SID;
    info.authenticated = false;
    info.operator_role = true;
    CHECK(kvm_rp_session_open(&g_h.rp, &info, g_h.now) >= 0);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_AUTH);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->client_auth_denials, 1); // TECH-061
    CHECK_EQ(g_h.n_intent, 0);
    // no token or payload is echoed back (TECH-046, AT-SEC-08)
    CHECK(rph_any_has("ERR_AUTH"));
    CHECK(!rph_any_has("hello"));
}

// ---- row: no USB_MOUNTED -------------------------------------------------------------------

static void test_host_offline(void)
{
    setup_owner();
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_DETACHED, false, true, g_h.now);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_HOST_OFFLINE);
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_SUSPENDED, false, true, g_h.now);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_USB_SUSPENDED);
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_ENUMERATING, false, true, g_h.now);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_HOST_OFFLINE);
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_MOUNTED, true, true, g_h.now);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_OTA_IN_PROGRESS);
    CHECK_EQ(g_h.n_intent, 0); // INV-005: no simulated confirmation of an action
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_MOUNTED, false, true, g_h.now);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);
}

// ---- row: no control lease (AT-SEC-02, AT-CONN-05) ------------------------------------------

static void test_no_lease(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    // authenticated, no lease: status works, input does not
    CHECK_EQ(rph_cmd(&c, "status.get", ""), KVM_RP_OK);
    CHECK(rph_last_has("status.snapshot"));
    CHECK_EQ(rph_input_ep(&c, 0, "'kind':'keyboard.key','usage_id':4,'down':true"),
             KVM_RP_ERR_CONTROL_DENIED);
    // an epoch nobody granted is stale, whoever asks for it
    CHECK_EQ(rph_input_ep(&c, 7, "'kind':'mouse.move','dx':5,'dy':5"), KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(g_h.n_intent, 0);

    // an observer may never ask for or send input (FR-404)
    rph_reset(NULL);
    rph_cli_open(&c, SID, false);
    CHECK_EQ(rph_cmd(&c, "control.request", ""), KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_approval_begin, 0);
    CHECK_EQ(rph_cmd(&c, "status.get", ""), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, 0, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 0);

    // a second operator cannot take the lease while the first holds it (03 §6.5: max 1 operator)
    rph_reset(NULL);
    rph_cli_t a;
    rph_cli_t b;
    rph_cli_open(&a, SID, true);
    rph_cli_open(&b, SID + 1u, true);
    (void)rph_cli_grant(&a);
    g_h.request_result = KVM_RP_ERR_CONTROL_DENIED; // the arbiter refuses: a lease exists
    CHECK_EQ(rph_cmd(&b, "control.request", ""), KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(rph_input_ep(&b, g_h.rp.owner_epoch, "'kind':'mouse.move','dx':1,'dy':1"),
             KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 0);
}

// ---- row: heartbeat / session stale --------------------------------------------------------

static void test_heartbeat_stale(void)
{
    setup_owner();
    uint32_t ep = g_h.rp.owner_epoch;
    g_h.now += 2900000u; // still inside the 3 s window
    kvm_rp_poll(&g_h.rp, g_h.now);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_REMOTE);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_OK);

    rph_ping(&c); // 03 §4.3: a heartbeat renews the lease
    CHECK_EQ(g_h.n_heartbeat, 1);
    CHECK(rph_last_has("pong"));
    g_h.now += 2900000u;
    kvm_rp_poll(&g_h.rp, g_h.now);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_REMOTE);

    g_h.now += 200000u; // 3.1 s without a heartbeat
    kvm_rp_poll(&g_h.rp, g_h.now);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    CHECK(rph_any_has("control.revoked"));
    CHECK(rph_any_has("client_stale"));
    CHECK(rph_released_with(KVM_RA_LEASE_TIMEOUT)); // the right RELEASE_ALL reason
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->client_revokes, 1);
    // zero trust for old events afterwards
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(rph_input_ep(&c, g_h.rp.owner_epoch, "'kind':'mouse.move','dx':1,'dy':1"),
             KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 1);
}

static void test_stale_detected_on_input(void)
{
    // The lease is also checked on the input path, not only by the timer.
    setup_owner();
    uint32_t ep = g_h.rp.owner_epoch;
    g_h.now += 3100000u;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"),
             KVM_RP_ERR_CLIENT_STALE);
    CHECK_EQ(g_h.n_intent, 0);
    CHECK(rph_released_with(KVM_RA_LEASE_TIMEOUT));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
}

// ---- row: >120 input msg/s -----------------------------------------------------------------

static void test_rate_limit(void)
{
    setup_owner();
    uint32_t ep = g_h.rp.owner_epoch;
    // 120 pointer moves inside one bucket are all accepted
    for (int i = 0; i < 120; i++) {
        CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':1,'dy':0"), KVM_RP_OK);
    }
    CHECK_EQ(g_h.n_intent, 120);
    // the next moves are coalesced instead of rejected (03 §6.5)
    for (int i = 0; i < 40; i++) {
        CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':2,'dy':0"), KVM_RP_OK);
    }
    CHECK_EQ(g_h.n_intent, 120);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->input_coalesced, 40);
    // a non-idempotent DOWN over the limit is rejected, not silently dropped
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"),
             KVM_RP_ERR_RATE_LIMIT);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->input_throttled, 1);
    CHECK(rph_any_has("ERR_RATE_LIMIT"));
    CHECK_EQ(g_h.n_intent, 120);

    // the coalesced movement is flushed with its sum preserved
    g_h.now += 1000000u;
    kvm_rp_poll(&g_h.rp, g_h.now);
    CHECK_EQ(g_h.n_intent, 121);
    CHECK_EQ(g_h.intent[120].data.mouse_move.dx, 80); // 40 * 2
    CHECK_EQ(g_h.intent[120].data.mouse_move.dy, 0);
}

static void test_rate_limit_never_drops_an_up(void)
{
    // Priority 2 of AGENTS §7: an UP may never be thrown away, or a key stays pressed.
    setup_owner();
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'left','down':true"), KVM_RP_OK);
    for (int i = 0; i < 130; i++) {
        (void)rph_input_ep(&c, ep, "'kind':'mouse.move','dx':1,'dy':0");
    }
    int before = g_h.n_intent;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':false"), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'left','down':false"),
             KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, before + 2);
    CHECK_EQ(g_h.intent[before].type, KVM_HID_KEY_UP);
    CHECK_EQ(g_h.intent[before + 1].type, KVM_HID_MOUSE_BTN_UP);
}

static void test_rate_flood_revokes(void)
{
    setup_owner();
    uint32_t ep = g_h.rp.owner_epoch;
    kvm_rp_err_t last = KVM_RP_OK;
    for (int i = 0; i < 400; i++) {
        last = rph_input_ep(&c, ep, "'kind':'mouse.wheel','vertical':1");
        if (last == KVM_RP_ERR_RATE_LIMIT && g_h.rp.owner_kind == OWNER_NONE) {
            break;
        }
    }
    CHECK_EQ(last, KVM_RP_ERR_RATE_LIMIT);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE); // 03 §6.5: revoke on flood
    CHECK(rph_released_with(KVM_RA_REMOTE_DISCONNECT));
    CHECK(rph_any_has("rate_flood"));
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->input_floods, 1);
    CHECK(g_h.n_close >= 1);
}

// ---- rows: key already down / up without down ----------------------------------------------

static void test_key_idempotence(void)
{
    setup_owner();
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);
    // already down -> idempotent no-op, nothing repeated to USB
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->idempotent_key_down, 2);

    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':false"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 2);
    CHECK_EQ(g_h.intent[1].type, KVM_HID_KEY_UP);
    // up without down -> no-op + metric, no reset of the whole keyboard
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':false"), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':5,'down':false"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 2);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->keyup_without_down, 2);
    for (int i = 0; i < g_h.n_intent; i++) {
        CHECK(g_h.intent[i].type != KVM_HID_RELEASE_ALL);
    }
    // the same rule for mouse buttons
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'right','down':true"),
             KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'right','down':true"),
             KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 3);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'middle','down':false"),
             KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 3);

    // a refused DOWN must not mark the key as pressed, or the next DOWN would be swallowed
    g_h.sink_result = KVM_RP_ERR_HID_QUEUE_OVERFLOW;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':6,'down':true"),
             KVM_RP_ERR_HID_QUEUE_OVERFLOW);
    g_h.sink_result = KVM_RP_OK;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':6,'down':true"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 4);
}

// ---- pairing-only sessions (SEC-103) -------------------------------------------------------

static void test_pairing_scope(void)
{
    rph_reset(NULL);
    kvm_rp_session_info_t info;
    memset(&info, 0, sizeof(info));
    info.session_id = SID;
    info.authenticated = true;
    info.operator_role = true;
    info.pairing_only = true;
    CHECK(kvm_rp_session_open(&g_h.rp, &info, g_h.now) >= 0);
    // no window open: only ERR_PAIRING_EXPIRED exists
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_PAIRING_EXPIRED);
    // zero status, zero HID over an unpinned pairing connection
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"status.get\",\"seq\":2,\"payload\":{}}"),
             KVM_RP_ERR_PAIRING_EXPIRED);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":3,\"payload\":{}}"),
             KVM_RP_ERR_PAIRING_EXPIRED);
    CHECK(!rph_any_has("status.snapshot"));
    CHECK_EQ(g_h.n_intent, 0);

    // with a window open the frames are routed to secure_remote, which answers for itself
    rph_install_pair_hook();
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"pair.round1\",\"seq\":4,\"payload\":{\"a\":\"x\"}}"),
             KVM_RP_OK);
    CHECK_EQ(g_h.n_pair, 1);
    // a normal session never carries pair.* frames
    rph_cli_t n;
    rph_cli_open(&n, SID + 1u, true);
    CHECK_EQ(rph_cmd(&n, "pair.hello", ""), KVM_RP_ERR_PAIRING_EXPIRED);
    CHECK_EQ(g_h.n_pair, 1);
}

// ---- abuse accounting ----------------------------------------------------------------------

static void test_abuse_closes_session(void)
{
    setup_owner();
    for (int i = 0; i < 8; i++) {
        c.q++;
        (void)rph_feed(SID, jf("{'v':1,'type':'nope','seq':%llu,'session_id':'%s'}",
                               (unsigned long long)c.q, c.label));
    }
    CHECK_EQ(g_h.n_close, 1); // 03 §6.5: count + close on abuse
    CHECK(rph_released_with(KVM_RA_REMOTE_DISCONNECT));
    CHECK_EQ(g_h.n_intent, 0);
}

// ---- privacy (TECH-046, AT-SEC-08) ---------------------------------------------------------

static void test_no_secrets_in_output(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    (void)rph_cli_grant(&c);
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':999,'down':true"),
             KVM_RP_ERR_USAGE);
    CHECK_EQ(rph_cmd(&c, "status.get", ""), KVM_RP_OK);
    int n = (g_h.n_frame < RPH_MAX_FRAMES) ? g_h.n_frame : RPH_MAX_FRAMES;
    for (int i = 0; i < n; i++) {
        CHECK(strstr(g_h.frame[i], "usage") == NULL);   // no key events on the wire
        CHECK(strstr(g_h.frame[i], "999") == NULL);     // the offending value is not echoed
        CHECK(strstr(g_h.frame[i], "token") == NULL);
        CHECK(strstr(g_h.frame[i], "password") == NULL);
        CHECK(strstr(g_h.frame[i], "secret") == NULL);
        CHECK(strstr(g_h.frame[i], "approval") == NULL || strstr(g_h.frame[i], "pending") != NULL);
    }
    // the snapshot carries no policy values either (SEC-107)
    CHECK(rph_any_has("status.snapshot"));
    CHECK(!rph_any_has("control.approval"));
    CHECK(!rph_any_has("require_accept"));
}

int main(void)
{
    printf("remote_validate\n");
    RUN(test_version);
    RUN(test_bad_frame);
    RUN(test_unknown_type_and_settings);
    RUN(test_range);
    RUN(test_usage);
    RUN(test_shortcut_allowlist);
    RUN(test_replay);
    RUN(test_replay_after_reconnect);
    RUN(test_stale_epoch);
    RUN(test_session_mismatch);
    RUN(test_unauthenticated);
    RUN(test_host_offline);
    RUN(test_no_lease);
    RUN(test_heartbeat_stale);
    RUN(test_stale_detected_on_input);
    RUN(test_rate_limit);
    RUN(test_rate_limit_never_drops_an_up);
    RUN(test_rate_flood_revokes);
    RUN(test_key_idempotence);
    RUN(test_pairing_scope);
    RUN(test_abuse_closes_session);
    RUN(test_no_secrets_in_output);
    TEST_MAIN_END("remote_validate");
}
