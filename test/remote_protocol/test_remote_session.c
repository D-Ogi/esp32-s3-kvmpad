// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Session and command layer (spec 03 §6.3, §6.4, §6.6): hello, status, control request/grant/
// release, ping, session.close, the 30 s approval timer and the server->client serializer.
// Covers AT-CONN-04, AT-CONN-05, AT-CONN-06 and AT-CONN-08 at the software level.
#include "kvm_test.h"
#include "rp_harness.h"

#define SID 0x2001u

static rph_cli_t c;

static void test_hello_ack(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    CHECK(rph_last_has("\"type\":\"hello.ack\""));
    CHECK(rph_last_has("\"protocol\":1"));
    CHECK(rph_last_has("\"device_id\":\"kvm-7f3a\""));
    CHECK(rph_last_has("\"capabilities\":[\"keyboard\",\"mouse\",\"shortcuts\"]"));
    CHECK(rph_last_has("\"host\":\"USB_MOUNTED\""));
    CHECK(rph_last_has("\"role\":\"operator\""));
    CHECK(rph_last_has("\"seq\":1"));          // the server runs its own counter (03 §6.4)
    CHECK(rph_last_has("\"reply_to_seq\":1")); // correlation to the client seq
    CHECK(strlen(c.label) == 6 && c.label[0] == 'c' && c.label[1] == '-');

    // the label is unique per connection
    rph_cli_t b;
    rph_cli_open(&b, SID + 1u, true);
    CHECK(strcmp(c.label, b.label) != 0);

    // hello without a payload is valid
    rph_reset(NULL);
    kvm_rp_session_info_t info;
    memset(&info, 0, sizeof(info));
    info.session_id = SID;
    info.authenticated = true;
    CHECK(kvm_rp_session_open(&g_h.rp, &info, g_h.now) >= 0);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":1}"), KVM_RP_OK);
    CHECK(rph_last_has("\"role\":\"observer\""));

    // a hostile client name is sanitized, never rendered raw
    rph_reset(NULL);
    memset(&info, 0, sizeof(info));
    info.session_id = SID;
    info.authenticated = true;
    info.operator_role = true;
    CHECK(kvm_rp_session_open(&g_h.rp, &info, g_h.now) >= 0);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":1,\"payload\":"
                           "{\"client_name\":\"a\\u0007b\\nc\"}}"),
             KVM_RP_OK);
    g_h.approval = KVM_RP_APPROVAL_PENDING;
    CHECK_EQ(rph_feed(SID, jf("{'v':1,'type':'control.request','seq':2,'session_id':'%s'}",
                              kvm_rp_session_label(&g_h.rp, SID))),
             KVM_RP_OK);
    CHECK_EQ(strcmp(g_h.last_approval_name, "abc"), 0);
}

static void test_hello_payload_limits(void)
{
    rph_reset(NULL);
    kvm_rp_session_info_t info;
    memset(&info, 0, sizeof(info));
    info.session_id = SID;
    info.authenticated = true;
    CHECK(kvm_rp_session_open(&g_h.rp, &info, g_h.now) >= 0);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":1,\"payload\":"
                           "{\"capabilities\":[1,2]}}"),
             KVM_RP_ERR_BAD_FRAME); // not strings
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":2,\"payload\":"
                           "{\"capabilities\":\"hid_v1\"}}"),
             KVM_RP_ERR_BAD_FRAME); // not an array
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":3,\"payload\":{\"evil\":1}}"),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":4,\"payload\":"
                           "{\"capabilities\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\","
                           "\"i\"]}}"),
             KVM_RP_ERR_BAD_FRAME); // more than 8 entries
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"hello\",\"seq\":5,\"payload\":"
                           "{\"capabilities\":[\"hid_v1\",\"future_thing\"]}}"),
             KVM_RP_OK); // an unknown capability is ignored, the device advertises its own
}

static void test_status_snapshot(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    CHECK_EQ(rph_cmd(&c, "status.get", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"type\":\"status.snapshot\""));
    CHECK(rph_last_has("\"host\":{\"state\":\"USB_MOUNTED\",\"transport\":\"usb_hid\"}"));
    CHECK(rph_last_has("\"client\":{\"state\":\"CLIENT_CONNECTED\",\"role\":\"operator\"}"));
    CHECK(rph_last_has("\"control\":{\"owner\":\"NONE\",\"lease_epoch\":0}"));
    CHECK(rph_last_has("\"wifi\":\"CONNECTED\""));
    CHECK(rph_last_has("\"maintenance\":false"));
    CHECK(rph_last_has("\"fw\":\"1.0.0\""));
    CHECK(rph_last_has("\"host_layout\":\"pl(dvorak)\"")); // ADR-024 / FR-745, read-only
    CHECK(rph_last_has("\"server_timestamp_us\":"));

    // ownership and host state are reflected, never guessed
    (void)rph_cli_grant(&c);
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_SUSPENDED, true, false, g_h.now);
    CHECK_EQ(rph_cmd(&c, "status.get", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"state\":\"USB_SUSPENDED\""));
    CHECK(rph_last_has("\"owner\":\"REMOTE\""));
    CHECK(rph_last_has("\"wifi\":\"DISCONNECTED\""));
    CHECK(rph_last_has("\"maintenance\":true"));

    // AT-CONN-04: wifi up but no authenticated session -> the client is simply not there
    CHECK(!rph_any_has("\"state\":\"CLIENT_CONNECTED\",\"role\":\"operator\",\"wifi\""));

    // a free-form layout id never reaches the wire (FR-745)
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_MOUNTED, false, true, g_h.now);
    CHECK_EQ(rph_cmd(&c, "status.get", "'extra':1"), KVM_RP_ERR_BAD_FRAME);
}

static void test_control_flow_with_approval(void)
{
    // AT-CONN-06: grant remote with physical approval
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    g_h.approval = KVM_RP_APPROVAL_PENDING;
    CHECK_EQ(rph_cmd(&c, "control.request", "'reason':'remote_support'"), KVM_RP_OK);
    CHECK_EQ(g_h.n_approval_begin, 1);
    CHECK_EQ(strcmp(g_h.last_approval_reason, "remote_support"), 0);
    CHECK(rph_last_has("\"type\":\"control.pending\""));
    CHECK(rph_last_has("\"approval_required\":true"));
    CHECK(rph_last_has("\"expires_in_s\":30"));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE); // nothing granted before the tap

    // input is still refused while the prompt is open
    CHECK_EQ(rph_input_ep(&c, 0, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 0);

    kvm_rp_approval_result(&g_h.rp, SID, true, g_h.now);
    CHECK(rph_last_has("\"type\":\"control.granted\""));
    CHECK(rph_last_has("\"expires_in_ms\":4000"));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_REMOTE);
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK(ep > 0);
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':3,'dy':4"), KVM_RP_OK);
    CHECK_EQ(g_h.n_intent, 1);
    CHECK_EQ(g_h.intent[0].epoch, ep);
}

static void test_approval_timeout(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    g_h.approval = KVM_RP_APPROVAL_PENDING;
    CHECK_EQ(rph_cmd(&c, "control.request", ""), KVM_RP_OK);
    g_h.now += 29000000u;
    kvm_rp_poll(&g_h.rp, g_h.now);
    CHECK_EQ(g_h.n_approval_cancel, 0);
    g_h.now += 1500000u; // past 30 s
    kvm_rp_poll(&g_h.rp, g_h.now);
    CHECK_EQ(g_h.n_approval_cancel, 1);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->approval_timeouts, 1);
    CHECK(rph_last_has("ERR_CONTROL_DENIED"));
    CHECK(rph_last_has("\"retryable\":true"));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    // a late tap no longer grants anything
    kvm_rp_approval_result(&g_h.rp, SID, true, g_h.now);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    CHECK_EQ(g_h.n_intent, 0);
}

static void test_approval_denied_and_auto(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    g_h.approval = KVM_RP_APPROVAL_DENIED;
    CHECK_EQ(rph_cmd(&c, "control.request", ""), KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);

    // ADR-023 `control.approval` = auto/trusted: the policy answers immediately, no prompt
    g_h.approval = KVM_RP_APPROVAL_GRANTED;
    CHECK_EQ(rph_cmd(&c, "control.request", ""), KVM_RP_OK);
    CHECK_EQ(g_h.rp.owner_kind, OWNER_REMOTE);
    CHECK(rph_last_has("control.granted"));

    // a repeated request from the current owner is answered idempotently
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_cmd(&c, "control.request", ""), KVM_RP_OK);
    CHECK_EQ(g_h.rp.owner_epoch, ep); // no new epoch, no extra release
    CHECK_EQ(g_h.n_release, 0);

    // only one prompt at a time
    rph_reset(NULL);
    rph_cli_t a;
    rph_cli_t b;
    rph_cli_open(&a, SID, true);
    rph_cli_open(&b, SID + 1u, true);
    g_h.approval = KVM_RP_APPROVAL_PENDING;
    CHECK_EQ(rph_cmd(&a, "control.request", ""), KVM_RP_OK);
    CHECK_EQ(rph_cmd(&b, "control.request", ""), KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_approval_begin, 1);
}

static void test_control_release(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    (void)rph_cli_grant(&c);
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    CHECK_EQ(rph_cmd(&c, "control.release", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"type\":\"control.revoked\""));
    CHECK(rph_last_has("\"reason\":\"client_release\""));
    CHECK(rph_last_has("\"new_epoch\":"));
    CHECK(rph_released_with(KVM_RA_REMOTE_RELEASE));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    CHECK(g_h.rp.owner_epoch > ep);
    // old events carry no weight afterwards: the epoch they claim no longer exists
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'keyboard.key','usage_id':4,'down':true"),
             KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(g_h.n_intent, 1);
    // releasing without holding the lease is refused
    CHECK_EQ(rph_cmd(&c, "control.release", ""), KVM_RP_ERR_CONTROL_DENIED);
}

static void test_session_close(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    (void)rph_cli_grant(&c);
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'left','down':true"),
             KVM_RP_OK);
    CHECK_EQ(rph_cmd(&c, "session.close", ""), KVM_RP_OK);
    CHECK(rph_released_with(KVM_RA_REMOTE_DISCONNECT));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    CHECK_EQ(kvm_rp_slot_of(&g_h.rp, SID), -1);
    // the connection is gone: later frames are not answered at all
    int frames = g_h.n_frame;
    CHECK_EQ(rph_feed(SID, "{\"v\":1,\"type\":\"ping\",\"seq\":99}"), KVM_RP_ERR_SESSION);
    CHECK_EQ(g_h.n_frame, frames);
    CHECK_EQ(g_h.n_intent, 1);
}

static void test_transport_drop(void)
{
    // AT-CONN-08 variant: the socket dies without session.close
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    (void)rph_cli_grant(&c);
    CHECK_EQ(rph_input(&c, "'kind':'keyboard.key','usage_id':224,'down':true"), KVM_RP_OK);
    kvm_rp_session_close(&g_h.rp, SID, KVM_RP_REVOKE_SESSION_CLOSED, g_h.now);
    CHECK(rph_released_with(KVM_RA_REMOTE_DISCONNECT));
    CHECK_EQ(g_h.rp.owner_kind, OWNER_NONE);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->client_revokes, 1);
    // closing twice is harmless
    kvm_rp_session_close(&g_h.rp, SID, KVM_RP_REVOKE_SESSION_CLOSED, g_h.now);
    CHECK_EQ(g_h.n_release, 1);
}

static void test_local_override_notification(void)
{
    // ADR-011 / 03 §4.3 on_local_input: the arbiter revokes, the protocol only informs the client
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    (void)rph_cli_grant(&c);
    uint32_t ep = g_h.rp.owner_epoch;
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.button','button':'left','down':true"),
             KVM_RP_OK);
    // the arbiter took control away locally and bumped the epoch
    kvm_rp_set_owner(&g_h.rp, OWNER_LOCAL, 0, ep + 1u, g_h.now);
    kvm_rp_notify_revoked(&g_h.rp, SID, KVM_RP_REVOKE_LOCAL_OVERRIDE, ep + 1u, g_h.now);
    CHECK(rph_last_has("\"type\":\"control.revoked\""));
    CHECK(rph_last_has("\"reason\":\"local_override\""));
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->revokes_by_reason[KVM_RP_REVOKE_LOCAL_OVERRIDE], 1);
    // no REMOTE event is accepted after the override, with either epoch
    CHECK_EQ(rph_input_ep(&c, ep, "'kind':'mouse.move','dx':9,'dy':9"), KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(rph_input_ep(&c, ep + 1u, "'kind':'mouse.move','dx':9,'dy':9"),
             KVM_RP_ERR_CONTROL_DENIED);
    CHECK_EQ(g_h.n_intent, 1);
    // the session itself survives and can still read status
    CHECK_EQ(rph_cmd(&c, "status.get", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"owner\":\"LOCAL\""));
}

static void test_ping_pong(void)
{
    rph_reset(NULL);
    rph_cli_open(&c, SID, true);
    CHECK_EQ(rph_cmd(&c, "ping", "'nonce':101"), KVM_RP_OK);
    CHECK(rph_last_has("\"type\":\"pong\""));
    CHECK(rph_last_has("\"nonce\":101"));
    CHECK_EQ(g_h.n_heartbeat, 0); // not the owner: nothing to renew
    CHECK_EQ(rph_cmd(&c, "ping", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"payload\":{}"));
    CHECK_EQ(rph_cmd(&c, "ping", "'nonce':-7"), KVM_RP_OK);
    CHECK(rph_last_has("\"nonce\":-7"));
    CHECK_EQ(rph_cmd(&c, "ping", "'nonce':'x'"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(rph_cmd(&c, "ping", "'nonce':1,'extra':2"), KVM_RP_ERR_BAD_FRAME);
    (void)rph_cli_grant(&c);
    CHECK_EQ(rph_cmd(&c, "ping", "'nonce':1"), KVM_RP_OK);
    CHECK_EQ(g_h.n_heartbeat, 1);
}

static void test_session_table(void)
{
    rph_reset(NULL);
    for (int i = 0; i < KVM_RP_MAX_SESSIONS; i++) {
        kvm_rp_session_info_t info;
        memset(&info, 0, sizeof(info));
        info.session_id = (uint64_t)(0x3000 + i);
        info.authenticated = true;
        info.label_entropy = (uint32_t)i;
        CHECK(kvm_rp_session_open(&g_h.rp, &info, g_h.now) >= 0);
    }
    kvm_rp_session_info_t extra;
    memset(&extra, 0, sizeof(extra));
    extra.session_id = 0x4000;
    extra.authenticated = true;
    CHECK_EQ(kvm_rp_session_open(&g_h.rp, &extra, g_h.now), -1);
    CHECK_EQ(kvm_rp_counters(&g_h.rp)->sessions_rejected, 1);
    // a duplicate id and id 0 are refused
    extra.session_id = 0x3000;
    CHECK_EQ(kvm_rp_session_open(&g_h.rp, &extra, g_h.now), -1);
    extra.session_id = 0;
    CHECK_EQ(kvm_rp_session_open(&g_h.rp, &extra, g_h.now), -1);
    CHECK_EQ(kvm_rp_session_open(&g_h.rp, NULL, g_h.now), -1);
    // a slot is reusable after a close
    kvm_rp_session_close(&g_h.rp, 0x3000, KVM_RP_REVOKE_SESSION_CLOSED, g_h.now);
    extra.session_id = 0x4000;
    CHECK(kvm_rp_session_open(&g_h.rp, &extra, g_h.now) >= 0);
    CHECK_EQ(strcmp(kvm_rp_session_label(&g_h.rp, 0x9999), ""), 0);
}

static void test_input_ack_option(void)
{
    kvm_rp_config_t cfg = KVM_RP_CONFIG_DEFAULT;
    cfg.send_input_ack = true;
    cfg.send_reply_to_seq = false;
    rph_reset(&cfg);
    rph_cli_open(&c, SID, true);
    (void)rph_cli_grant(&c);
    CHECK_EQ(rph_input(&c, "'kind':'mouse.move','dx':1,'dy':1"), KVM_RP_OK);
    CHECK(rph_last_has("\"type\":\"input.ack\""));
    CHECK(!rph_last_has("reply_to_seq"));
}

static void test_error_codes_and_names(void)
{
    CHECK_EQ(strcmp(kvm_rp_err_name(KVM_RP_ERR_BAD_FRAME), "ERR_BAD_FRAME"), 0);
    CHECK_EQ(strcmp(kvm_rp_err_name(KVM_RP_ERR_STALE_EPOCH), "ERR_STALE_EPOCH"), 0);
    CHECK_EQ(strcmp(kvm_rp_err_name(KVM_RP_ERR_PAIRING_EXPIRED), "ERR_PAIRING_EXPIRED"), 0);
    for (int e = 0; e < KVM_RP_ERR_COUNT; e++) {
        CHECK(kvm_rp_err_name((kvm_rp_err_t)e) != NULL);
        CHECK(strlen(kvm_rp_err_name((kvm_rp_err_t)e)) > 1);
    }
    for (int r = 0; r < KVM_RP_REVOKE_COUNT; r++) {
        CHECK(kvm_rp_revoke_reason_name((kvm_rp_revoke_reason_t)r) != NULL);
        CHECK(kvm_rp_release_reason((kvm_rp_revoke_reason_t)r) < KVM_RA_COUNT);
    }
    // the arbiter mapping is the component's, not the integrator's, to invent
    CHECK_EQ(kvm_rp_err_from_hid(KVM_HID_OK), KVM_RP_OK);
    CHECK_EQ(kvm_rp_err_from_hid(KVM_HID_ERR_STALE_EPOCH), KVM_RP_ERR_STALE_EPOCH);
    CHECK_EQ(kvm_rp_err_from_hid(KVM_HID_ERR_HOST_OFFLINE), KVM_RP_ERR_HOST_OFFLINE);
    CHECK_EQ(kvm_rp_err_from_hid(KVM_HID_ERR_HID_QUEUE_OVERFLOW), KVM_RP_ERR_HID_QUEUE_OVERFLOW);
    CHECK_EQ(kvm_rp_err_from_hid(KVM_HID_ERR_BAD_INTENT), KVM_RP_ERR_BAD_FRAME);
    CHECK(!kvm_rp_err_retryable(KVM_RP_ERR_REPLAY));
    CHECK(kvm_rp_err_retryable(KVM_RP_ERR_HOST_OFFLINE));
}

static void test_footprint(void)
{
    // 02 §11.2 budget: static, bounded, no per-message allocation
    printf("    sizeof(kvm_rp_t)=%zu sizeof(kvm_rp_session_t)=%zu sizeof(kvm_json_doc_t)=%zu\n",
           sizeof(kvm_rp_t), sizeof(kvm_rp_session_t), sizeof(kvm_json_doc_t));
    CHECK(sizeof(kvm_rp_session_t) <= 256);
    CHECK(sizeof(kvm_json_doc_t) <= 1024);
    CHECK(sizeof(kvm_rp_t) <= 4096);
}

int main(void)
{
    printf("remote_session\n");
    RUN(test_hello_ack);
    RUN(test_hello_payload_limits);
    RUN(test_status_snapshot);
    RUN(test_control_flow_with_approval);
    RUN(test_approval_timeout);
    RUN(test_approval_denied_and_auto);
    RUN(test_control_release);
    RUN(test_session_close);
    RUN(test_transport_drop);
    RUN(test_local_override_notification);
    RUN(test_ping_pong);
    RUN(test_session_table);
    RUN(test_input_ack_option);
    RUN(test_error_codes_and_names);
    RUN(test_footprint);
    TEST_MAIN_END("remote_session");
}
