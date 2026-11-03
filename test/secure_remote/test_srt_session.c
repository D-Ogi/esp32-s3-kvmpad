// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Secure remote transport session glue: admission, the two endpoints, the loss paths and the
// pairing run end to end over real JSON frames against the CLIENT driver.
//
// Requirements exercised here: FR-721..727, FR-732, FR-733, SEC-102..107, TECH-042..046,
// AGENTS.md prohibitions 2, 3 and 4. Tests: AT-EXT-13/14/15/16/17, AT-SEC-03/04/08, AT-CONN-04/07.
// Nothing in this file touches a socket, a TLS stack or the board.

#include <stdio.h>
#include <string.h>

#include "kvm_pairing_client.h"
#include "kvm_test.h"
#include "srt_harness.h"

static sh_t h;

static const uint8_t CLIENT_ID[KVM_PAIR_CLIENT_ID_LEN] = {1,  2,  3,  4,  5,  6,  7,  8,
                                                          9,  10, 11, 12, 13, 14, 15, 16};

// --- a full pairing run over the wire ------------------------------------------------------------
//
// The DEVICE is the kvm_srt instance and the CLIENT is kvm_pair_client_t -- the very code the
// Windows application links (ADR-029 §1). Everything between them is JSON through the transport
// codec, so this is the interop path, not a shortcut through the core.

typedef struct {
    kvm_pair_client_t cli;
    uint64_t conn;
    uint64_t seq;
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    bool have_token;
} run_t;

static void run_send(run_t *r, const char *type, const char *field, const uint8_t *data, size_t len)
{
    char b64[KVM_PAIR_ROUND1_B64_CAP];
    CHECK(kvm_pair_b64_encode(data, len, b64, sizeof(b64)));
    r->seq++;
    sh_frame(&h, r->conn,
             sh_jf("{'v':1,'type':'%s','seq':%llu,'payload':{'%s':'%s'}}", type,
                   (unsigned long long)r->seq, field, b64));
}

static bool run_pair(run_t *r, const char *code, const char *client_name,
                     const uint8_t cert[KVM_PAIR_SHA256_LEN], bool expect_accept_screen)
{
    memset(r, 0, sizeof(*r));

    kvm_srt_admission_t adm;
    if (kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm) != KVM_SRT_ADMIT_OK) {
        return false;
    }
    r->conn = 100u;
    if (kvm_srt_conn_open(&h.srt, r->conn, &adm, sh_now_us()) < 0) {
        return false;
    }

    kvm_pair_hello_t hello;
    if (kvm_pair_client_begin(&r->cli, code, client_name, CLIENT_ID, cert, &hello) != KVM_PAIR_OK) {
        return false;
    }

    char id_hex[KVM_PAIR_CLIENT_ID_HEX_CAP];
    char cert_hex[KVM_PAIR_HEX32_CAP];
    CHECK(kvm_pair_hex_encode(hello.client_id, sizeof(hello.client_id), id_hex, sizeof(id_hex)));
    CHECK(kvm_pair_hex_encode(hello.cert_sha256, sizeof(hello.cert_sha256), cert_hex,
                              sizeof(cert_hex)));

    sh_clear_wire(&h);
    r->seq = 1;
    sh_frame(&h, r->conn,
             sh_jf("{'v':1,'type':'pair.hello','seq':1,'payload':{'v':%u,'pake':'%s',"
                   "'client_name':'%s','client_id':'%s','cert_sha256':'%s'}}",
                   (unsigned)hello.v, hello.pake, client_name, id_hex, cert_hex));

    // 1. DEVICE -> CLIENT round 1 (also the acknowledgment of `pair.hello`).
    if (strcmp(sh_last_type(&h), "pair.round1") != 0) {
        return false;
    }
    uint8_t dev[KVM_PAIR_ROUND1_MAX];
    size_t dev_len = 0;
    CHECK(sh_payload_blob(&h, "blob", dev, sizeof(dev), &dev_len));

    // 2. CLIENT -> DEVICE round 1, which produces DEVICE -> CLIENT round 2.
    uint8_t cl[KVM_PAIR_ROUND1_MAX];
    size_t cl_len = 0;
    CHECK_EQ(kvm_pair_client_round1(&r->cli, dev, dev_len, cl, sizeof(cl), &cl_len), KVM_PAIR_OK);
    sh_clear_wire(&h);
    run_send(r, "pair.round1", "blob", cl, cl_len);
    if (strcmp(sh_last_type(&h), "pair.round2") != 0) {
        return false;
    }
    CHECK(sh_payload_blob(&h, "blob", dev, sizeof(dev), &dev_len));

    // 3. CLIENT -> DEVICE round 2. The DEVICE answers nothing: the CLIENT confirms first.
    CHECK_EQ(kvm_pair_client_round2(&r->cli, dev, dev_len, cl, sizeof(cl), &cl_len), KVM_PAIR_OK);
    sh_clear_wire(&h);
    run_send(r, "pair.round2", "blob", cl, cl_len);
    CHECK_EQ(h.nsent, 0);

    // 4. CLIENT -> DEVICE confirmation. A wrong code dies here, before anything is revealed.
    uint8_t tag[KVM_PAIR_CONFIRM_LEN];
    CHECK_EQ(kvm_pair_client_confirm_out(&r->cli, tag), KVM_PAIR_OK);
    h.nevents = 0;
    sh_clear_wire(&h);
    run_send(r, "pair.confirm", "mac", tag, sizeof(tag));
    if (strcmp(sh_last_type(&h), "pair.confirm") != 0 &&
        strcmp(sh_last_type(&h), "pair.accepted") != 0) {
        return false;
    }

    // With require_accept ON the DEVICE shows the Accept screen and waits (FR-725).
    if (expect_accept_screen) {
        bool screen = false;
        for (int i = 0; i < h.nevents; i++) {
            if (h.events[i].kind == KVM_PAIR_EVENT_ACCEPT_SCREEN) {
                screen = true;
                CHECK_EQ(strcmp(h.events[i].client_name, client_name), 0);
            }
        }
        CHECK(screen);
        CHECK_EQ(strcmp(sh_last_type(&h), "pair.confirm"), 0);

        uint8_t dtag[KVM_PAIR_CONFIRM_LEN];
        size_t dn = 0;
        CHECK(sh_payload_blob(&h, "mac", dtag, sizeof(dtag), &dn));
        CHECK_EQ(dn, KVM_PAIR_CONFIRM_LEN);
        CHECK_EQ(kvm_pair_client_confirm_in(&r->cli, dtag, dn), KVM_PAIR_OK);

        sh_clear_wire(&h);
        CHECK_EQ(kvm_srt_pair_accept(&h.srt, sh_now_us()), KVM_PAIR_OK);
    } else {
        // require_accept OFF: `pair.confirm` and `pair.accepted` arrive back to back (ADR-023).
        CHECK_EQ(h.nsent, 2);
        uint8_t dtag[KVM_PAIR_CONFIRM_LEN];
        size_t dn = 0;
        static kvm_json_doc_t d;
        CHECK_EQ(kvm_json_parse(&d, h.sent[0].text, h.sent[0].len), KVM_JSON_OK);
        char b64[KVM_PAIR_CONFIRM_B64_CAP];
        const int p0 = kvm_json_member(&d, kvm_json_root(&d), "payload");
        CHECK(kvm_json_str_copy(&d, kvm_json_member(&d, p0, "mac"), b64, sizeof(b64)) !=
              (size_t)-1);
        CHECK(kvm_pair_b64_decode(b64, dtag, sizeof(dtag), &dn));
        CHECK_EQ(kvm_pair_client_confirm_in(&r->cli, dtag, dn), KVM_PAIR_OK);
    }

    if (strcmp(sh_last_type(&h), "pair.accepted") != 0) {
        return false;
    }
    uint8_t sealed[KVM_PAIR_TOKEN_SEALED_LEN];
    size_t sn = 0;
    CHECK(sh_payload_blob(&h, "token_enc", sealed, sizeof(sealed), &sn));
    CHECK_EQ(sn, KVM_PAIR_TOKEN_SEALED_LEN);
    CHECK_EQ(kvm_pair_client_open_token(&r->cli, sealed, sn, r->token), KVM_PAIR_OK);
    r->have_token = true;
    return true;
}

/** The `Authorization` header value a run's token produces (ADR-028 §2). */
static const char *run_header(const run_t *r)
{
    static char header[80];
    char enc[KVM_SRT_TOKEN_B64URL_CAP];
    CHECK(kvm_srt_b64url_encode(r->token, KVM_PAIR_TOKEN_LEN, enc, sizeof(enc)));
    (void)snprintf(header, sizeof(header), "Bearer %s", enc);
    return header;
}

// --- tests ---------------------------------------------------------------------------------------

static void t_pairing_full_run(void)
{
    CHECK_EQ(sh_init(&h, 0x51u), KVM_PAIR_OK);
    sh_online(&h);

    // The pairing endpoint is shut until a window is open (SEC-103, FR-723).
    CHECK(!kvm_srt_pair_endpoint_open(&h.srt));
    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm),
             KVM_SRT_ADMIT_FORBIDDEN);

    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);
    CHECK(kvm_srt_pair_endpoint_open(&h.srt));
    const char *code = kvm_srt_pair_code(&h.srt);
    CHECK(code != NULL);
    CHECK_EQ(strlen(code), KVM_PAIR_CODE_LEN);
    CHECK(kvm_srt_pair_code_remaining_ms(&h.srt) > 0);

    char typed[KVM_PAIR_CODE_CAP];
    (void)snprintf(typed, sizeof(typed), "%s", code);

    run_t r;
    CHECK(run_pair(&r, typed, "Desk1", h.cfg.cert_sha256, true));

    // FR-726 / SEC-105: the register holds the name and the digest of the token, never the token.
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 1);
    kvm_pair_record_t rec;
    CHECK(kvm_srt_clients_get(&h.srt, 0, &rec));
    CHECK_EQ(strcmp(rec.name, "Desk1"), 0);
    CHECK_EQ(memcmp(rec.token_sha256, r.token, KVM_PAIR_TOKEN_LEN) == 0, false);

    // The window is over, so the endpoint closes again and the socket is asked to go.
    CHECK(!kvm_srt_pair_endpoint_open(&h.srt));

    // AT-EXT-14 / ADR-028 §2: the token the CLIENT opened admits a control session.
    kvm_srt_conn_close(&h.srt, r.conn, KVM_SRT_LOSS_CLIENT_CLOSE, sh_now_us());
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, run_header(&r), sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);
    CHECK_EQ(adm.client_index, 0);
    CHECK(adm.operator_role);

    kvm_pair_client_free(&r.cli);
    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_pairing_require_accept_off(void)
{
    CHECK_EQ(sh_init(&h, 0x52u), KVM_PAIR_OK);
    h.cfg.pair_require_accept = false;
    kvm_srt_set_policy(&h.srt, false, KVM_CONTROL_APPROVAL_ALWAYS);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);

    char typed[KVM_PAIR_CODE_CAP];
    (void)snprintf(typed, sizeof(typed), "%s", kvm_srt_pair_code(&h.srt));
    run_t r;
    CHECK(run_pair(&r, typed, "Desk2", h.cfg.cert_sha256, false));
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 1);

    kvm_pair_client_free(&r.cli);
    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_pairing_wrong_code(void)
{
    CHECK_EQ(sh_init(&h, 0x53u), KVM_PAIR_OK);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);

    // A code that is not the one on the screen must fail at key confirmation and store nothing.
    char wrong[KVM_PAIR_CODE_CAP];
    const char *real = kvm_srt_pair_code(&h.srt);
    (void)snprintf(wrong, sizeof(wrong), "%s", real);
    wrong[0] = (char)(((wrong[0] - '0' + 1) % 10) + '0');

    run_t r;
    CHECK(!run_pair(&r, wrong, "Desk1", h.cfg.cert_sha256, true));
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 0);
    // The refusal is ERR_CONTROL_DENIED (ADR-029 §5) and carries no detail.
    CHECK(sh_sent_contains(&h, "ERR_CONTROL_DENIED"));
    CHECK(!sh_sent_contains(&h, real));
    // No HID left the device at any point during a pairing run.
    CHECK_EQ(h.nintents, 0);

    kvm_pair_client_free(&r.cli);
    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_pairing_cert_mismatch(void)
{
    CHECK_EQ(sh_init(&h, 0x54u), KVM_PAIR_OK);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);

    // SEC-102: a CLIENT that saw a different certificate is refused before a run starts, so a
    // substitution probe cannot burn the legitimate user's three attempts.
    uint8_t other[KVM_PAIR_SHA256_LEN];
    sh_cert_hash(other);
    other[0] ^= 0xFFu;

    char typed[KVM_PAIR_CODE_CAP];
    (void)snprintf(typed, sizeof(typed), "%s", kvm_srt_pair_code(&h.srt));
    run_t r;
    CHECK(!run_pair(&r, typed, "Desk1", other, true));
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 0);
    CHECK(sh_sent_contains(&h, "ERR_CONTROL_DENIED"));
    CHECK_EQ(kvm_srt_pair_counters(&h.srt)->pair_attempts, 0);

    kvm_pair_client_free(&r.cli);
    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_pairing_endpoint_limits(void)
{
    CHECK_EQ(sh_init(&h, 0x55u), KVM_PAIR_OK);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);

    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);
    CHECK(kvm_srt_conn_open(&h.srt, 100u, &adm, sh_now_us()) >= 0);

    // FR-722: one run at a time. A second peer does not get to race the first.
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm),
             KVM_SRT_ADMIT_BUSY);

    // SEC-103: the pairing endpoint carries `pair.*` and nothing else. Status, input and control
    // are not "unknown here", they are refused as malformed, and no intent is ever produced.
    sh_clear_wire(&h);
    sh_frame(&h, 100u, sh_jf("{'v':1,'type':'status.get','seq':2,'payload':{}}"));
    CHECK(sh_sent_contains(&h, "ERR_BAD_FRAME"));
    sh_clear_wire(&h);
    sh_frame(&h, 100u,
             sh_jf("{'v':1,'type':'input','seq':3,'lease_epoch':1,"
                   "'payload':{'kind':'keyboard.key','usage_id':4,'down':true}}"));
    CHECK(sh_sent_contains(&h, "ERR_BAD_FRAME"));
    sh_clear_wire(&h);
    sh_frame(&h, 100u, sh_jf("{'v':1,'type':'control.request','seq':4,'payload':{}}"));
    CHECK(sh_sent_contains(&h, "ERR_BAD_FRAME"));
    CHECK_EQ(h.nintents, 0);
    CHECK_EQ(h.nreleases, 0);

    // A replayed `seq` is refused (03 §6.5, AT-SEC-03). The first frame has to be a well-formed
    // `pair.*` message, because that is what moves the replay guard forward; a frame rejected by
    // the envelope codec never becomes the high-water mark.
    sh_clear_wire(&h);
    sh_frame(&h, 100u, sh_jf("{'v':1,'type':'pair.round1','seq':10,'payload':{'blob':'QQ=='}}"));
    CHECK(!sh_sent_contains(&h, "ERR_REPLAY"));
    sh_clear_wire(&h);
    sh_frame(&h, 100u, sh_jf("{'v':1,'type':'pair.round1','seq':10,'payload':{'blob':'QQ=='}}"));
    CHECK(sh_sent_contains(&h, "ERR_REPLAY"));
    sh_clear_wire(&h);
    sh_frame(&h, 100u, sh_jf("{'v':1,'type':'pair.round1','seq':9,'payload':{'blob':'QQ=='}}"));
    CHECK(sh_sent_contains(&h, "ERR_REPLAY"));

    // A session label that is not the one the DEVICE assigned is refused.
    sh_clear_wire(&h);
    sh_frame(&h, 100u,
             sh_jf("{'v':1,'type':'pair.round1','seq':20,'session_id':'p-ffff',"
                   "'payload':{'blob':'QQ=='}}"));
    CHECK(sh_sent_contains(&h, "ERR_SESSION"));

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_pairing_bad_frame_budget(void)
{
    CHECK_EQ(sh_init(&h, 0x56u), KVM_PAIR_OK);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);
    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);
    CHECK(kvm_srt_conn_open(&h.srt, 100u, &adm, sh_now_us()) >= 0);

    // 03 §6.5: count, then close on abuse. A peer that cannot produce a valid frame is dropped.
    sh_clear_wire(&h);
    for (int i = 0; i < 20; i++) {
        sh_frame(&h, 100u, "{not json");
    }
    CHECK(sh_was_closed(&h, 100u));
    CHECK(kvm_srt_counters(&h.srt)->pair_frames_rejected >= 8u);
    // Still no HID and no stored client out of a flood of garbage (AT-EXT-17).
    CHECK_EQ(h.nintents, 0);
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 0);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_pairing_preconditions(void)
{
    CHECK_EQ(sh_init(&h, 0x57u), KVM_PAIR_OK);

    // SEC-106: no window without Remote ON and Wi-Fi connected.
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_ERR_PRECONDITION);
    kvm_srt_set_remote_enabled(&h.srt, true, sh_now_us());
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_ERR_PRECONDITION);

    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);
    CHECK(kvm_srt_pair_endpoint_open(&h.srt));

    // Losing Wi-Fi mid-window closes it at once and wipes the code; it is not left to expire.
    kvm_srt_set_host(&h.srt, KVM_HOST_USB_MOUNTED, false, false, sh_now_us());
    CHECK(!kvm_srt_pair_endpoint_open(&h.srt));
    CHECK(kvm_srt_pair_code(&h.srt) == NULL);

    // The same for switching Remote off.
    kvm_srt_set_host(&h.srt, KVM_HOST_USB_MOUNTED, false, true, sh_now_us());
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);
    kvm_srt_set_remote_enabled(&h.srt, false, sh_now_us());
    CHECK(!kvm_srt_pair_endpoint_open(&h.srt));

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_pairing_accept_timeout(void)
{
    CHECK_EQ(sh_init(&h, 0x58u), KVM_PAIR_OK);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);
    char typed[KVM_PAIR_CODE_CAP];
    (void)snprintf(typed, sizeof(typed), "%s", kvm_srt_pair_code(&h.srt));

    run_t r;
    memset(&r, 0, sizeof(r));
    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);
    r.conn = 100u;
    CHECK(kvm_srt_conn_open(&h.srt, r.conn, &adm, sh_now_us()) >= 0);

    // Drive the run up to the Accept screen, then let it time out (FR-725, 30 s).
    kvm_pair_hello_t hello;
    CHECK_EQ(kvm_pair_client_begin(&r.cli, typed, "Desk1", CLIENT_ID, h.cfg.cert_sha256, &hello),
             KVM_PAIR_OK);
    char id_hex[KVM_PAIR_CLIENT_ID_HEX_CAP];
    char cert_hex[KVM_PAIR_HEX32_CAP];
    CHECK(kvm_pair_hex_encode(hello.client_id, sizeof(hello.client_id), id_hex, sizeof(id_hex)));
    CHECK(kvm_pair_hex_encode(hello.cert_sha256, sizeof(hello.cert_sha256), cert_hex,
                              sizeof(cert_hex)));
    r.seq = 1;
    sh_frame(&h, r.conn,
             sh_jf("{'v':1,'type':'pair.hello','seq':1,'payload':{'v':%u,'pake':'%s',"
                   "'client_name':'Desk1','client_id':'%s','cert_sha256':'%s'}}",
                   (unsigned)hello.v, hello.pake, id_hex, cert_hex));
    uint8_t dev[KVM_PAIR_ROUND1_MAX];
    uint8_t cl[KVM_PAIR_ROUND1_MAX];
    size_t dn = 0;
    size_t cn = 0;
    CHECK(sh_payload_blob(&h, "blob", dev, sizeof(dev), &dn));
    CHECK_EQ(kvm_pair_client_round1(&r.cli, dev, dn, cl, sizeof(cl), &cn), KVM_PAIR_OK);
    sh_clear_wire(&h);
    run_send(&r, "pair.round1", "blob", cl, cn);
    CHECK(sh_payload_blob(&h, "blob", dev, sizeof(dev), &dn));
    CHECK_EQ(kvm_pair_client_round2(&r.cli, dev, dn, cl, sizeof(cl), &cn), KVM_PAIR_OK);
    run_send(&r, "pair.round2", "blob", cl, cn);
    uint8_t tag[KVM_PAIR_CONFIRM_LEN];
    CHECK_EQ(kvm_pair_client_confirm_out(&r.cli, tag), KVM_PAIR_OK);
    sh_clear_wire(&h);
    run_send(&r, "pair.confirm", "mac", tag, sizeof(tag));
    CHECK_EQ(strcmp(sh_last_type(&h), "pair.confirm"), 0);

    sh_clear_wire(&h);
    det_clock_advance(KVM_PAIR_ACCEPT_TIMEOUT_MS + 500u);
    kvm_srt_tick(&h.srt, sh_now_us());
    CHECK_EQ(strcmp(sh_last_type(&h), "pair.rejected"), 0);
    CHECK(sh_sent_contains(&h, "\"reason\":\"timeout\""));
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 0);

    kvm_pair_client_free(&r.cli);
    kvm_srt_shutdown(&h.srt, sh_now_us());
}

// --- control endpoint ----------------------------------------------------------------------------

/** Put one client in the register without running a PAKE; returns its Bearer header. */
static const char *seed_client(sh_t *s, const char *name, uint8_t fill)
{
    static char header[80];
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    memset(token, fill, sizeof(token));
    kvm_pair_record_t rec;
    memset(&rec, 0, sizeof(rec));
    (void)snprintf(rec.name, sizeof(rec.name), "%s", name);
    CHECK_EQ(kvm_pair_sha256(token, sizeof(token), rec.token_sha256), KVM_PAIR_OK);
    rec.paired_at_unix = 1700000000u;
    CHECK(fake_pair_add(&s->store, &rec) >= 0);

    char enc[KVM_SRT_TOKEN_B64URL_CAP];
    CHECK(kvm_srt_b64url_encode(token, sizeof(token), enc, sizeof(enc)));
    (void)snprintf(header, sizeof(header), "Bearer %s", enc);
    return header;
}

static void t_admission(void)
{
    CHECK_EQ(sh_init(&h, 0x60u), KVM_PAIR_OK);
    char good[80];
    (void)snprintf(good, sizeof(good), "%s", seed_client(&h, "Desk1", 0x11u));

    kvm_srt_admission_t adm;
    // TECH-040 / FR-506: Remote is off on first boot, so nothing is admitted.
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
             KVM_SRT_ADMIT_FORBIDDEN);

    sh_online(&h);
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);
    CHECK_EQ(adm.client_index, 0);

    // AGENTS.md prohibition 4 / ADR-028 §2: no token, a wrong token, a malformed token and a
    // token in a different scheme are all one answer with no detail.
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, NULL, sh_now_us(), &adm),
             KVM_SRT_ADMIT_UNAUTHORIZED);
    CHECK_EQ(adm.client_index, -1);
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, "Bearer short", sh_now_us(), &adm),
             KVM_SRT_ADMIT_UNAUTHORIZED);

    uint8_t other[KVM_PAIR_TOKEN_LEN];
    memset(other, 0x99u, sizeof(other));
    char enc[KVM_SRT_TOKEN_B64URL_CAP];
    char header[80];
    CHECK(kvm_srt_b64url_encode(other, sizeof(other), enc, sizeof(enc)));
    (void)snprintf(header, sizeof(header), "Bearer %s", enc);
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, header, sh_now_us(), &adm),
             KVM_SRT_ADMIT_UNAUTHORIZED);

    const kvm_srt_counters_t *c = kvm_srt_counters(&h.srt);
    CHECK(c->auth_absent >= 1u);
    CHECK(c->auth_bad_encoding >= 1u);
    CHECK(c->auth_unknown_token >= 1u);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_admission_cooldown(void)
{
    CHECK_EQ(sh_init(&h, 0x61u), KVM_PAIR_OK);
    char good[80];
    (void)snprintf(good, sizeof(good), "%s", seed_client(&h, "Desk1", 0x22u));
    sh_online(&h);

    // TECH-042: authentication attempts are limited. After the run of failures even a correct
    // token waits out the cooldown, and the cooldown is a refusal, never a sleep.
    kvm_srt_admission_t adm;
    for (unsigned i = 0; i < KVM_SRT_AUTH_FAIL_MAX; i++) {
        CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, "Bearer nope", sh_now_us(), &adm),
                 KVM_SRT_ADMIT_UNAUTHORIZED);
    }
    CHECK_EQ(kvm_srt_counters(&h.srt)->auth_cooldowns, 1);
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
             KVM_SRT_ADMIT_UNAUTHORIZED);
    CHECK(kvm_srt_counters(&h.srt)->auth_refused_cold >= 1u);

    det_clock_advance(KVM_SRT_AUTH_COOLDOWN_US / 1000u + 1u);
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_control_session(void)
{
    CHECK_EQ(sh_init(&h, 0x62u), KVM_PAIR_OK);
    char good[80];
    (void)snprintf(good, sizeof(good), "%s", seed_client(&h, "Desk1", 0x33u));
    sh_online(&h);

    sh_cli_t c;
    CHECK(sh_cli_open(&h, &c, good, 1u));
    CHECK(sh_sent_contains(&h, "hello.ack"));
    CHECK(c.label[0] != '\0');

    // AT-CONN-04: an authenticated session without a lease sends no input.
    sh_clear_wire(&h);
    sh_input_ep(&h, &c, 0u, "'kind':'mouse.move','dx':5,'dy':-3");
    CHECK_EQ(h.nintents, 0);

    // FR-733 `always`: the request waits for the physical approval (PROC-006 step 5).
    h.approval_answer = KVM_SRT_APPROVAL_PENDING;
    sh_clear_wire(&h);
    sh_cmd(&h, &c, "control.request", "'reason':'remote_support'");
    CHECK_EQ(h.napproval_begin, 1);
    CHECK(sh_sent_contains(&h, "control.pending"));
    CHECK_EQ(h.nintents, 0);

    sh_clear_wire(&h);
    kvm_srt_approval_result(&h.srt, c.id, true, sh_now_us());
    CHECK(sh_sent_contains(&h, "control.granted"));
    const uint32_t epoch = h.epoch;
    CHECK(epoch > 0u);
    kvm_srt_set_owner(&h.srt, OWNER_REMOTE, c.id, epoch, sh_now_us());

    // With the lease in force, input reaches the HID path as a typed intent -- and only there.
    sh_input_ep(&h, &c, epoch, "'kind':'mouse.move','dx':5,'dy':-3");
    sh_input_ep(&h, &c, epoch, "'kind':'keyboard.key','usage_id':4,'down':true");
    CHECK_EQ(h.nintents, 2);
    CHECK_EQ(h.intents[0].type, KVM_HID_MOUSE_MOVE);
    CHECK_EQ(h.intents[0].data.mouse_move.dx, 5);
    CHECK_EQ(h.intents[1].type, KVM_HID_KEY_DOWN);
    CHECK_EQ(h.intents[1].data.key.usage_id, 4);
    CHECK_EQ(h.intent_session[1], c.id);
    CHECK_EQ(h.intent_epoch[1], epoch);

    // AT-SEC-03 / VEC-005: a stale epoch is refused and produces no HID.
    const int before = h.nintents;
    sh_clear_wire(&h);
    sh_input_ep(&h, &c, epoch - 1u, "'kind':'keyboard.key','usage_id':5,'down':true");
    CHECK_EQ(h.nintents, before);
    CHECK(sh_sent_contains(&h, "ERR_STALE_EPOCH"));

    // SEC-103: `pair.*` does not exist on the control endpoint, whatever the window state.
    sh_clear_wire(&h);
    sh_cmd(&h, &c, "pair.hello", "'v':1");
    CHECK(sh_sent_contains(&h, "ERR_PAIRING_EXPIRED"));

    // AGENTS.md prohibition 3: losing the socket while holding the lease releases everything, with
    // the reason that belongs to a disconnect.
    h.nreleases = 0;
    kvm_srt_conn_close(&h.srt, c.id, KVM_SRT_LOSS_SOCKET, sh_now_us());
    CHECK_EQ(h.nreleases, 1);
    CHECK_EQ(h.last_release, KVM_RA_REMOTE_DISCONNECT);
    CHECK_EQ(kvm_srt_slot_of(&h.srt, c.id), -1);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_loss_reasons(void)
{
    // Each loss path has to arrive at the RELEASE_ALL reason the HOST will see in diagnostics
    // (03 §4.2, TECH-005). A wrong mapping hides the cause of a stuck key.
    struct {
        kvm_srt_loss_t loss;
        kvm_release_reason_t release;
    } rows[] = {
        {KVM_SRT_LOSS_CLIENT_CLOSE, KVM_RA_REMOTE_DISCONNECT},
        {KVM_SRT_LOSS_SOCKET, KVM_RA_REMOTE_DISCONNECT},
        {KVM_SRT_LOSS_TLS, KVM_RA_REMOTE_DISCONNECT},
        {KVM_SRT_LOSS_TIMEOUT, KVM_RA_LEASE_TIMEOUT},
        {KVM_SRT_LOSS_PROTOCOL, KVM_RA_REMOTE_DISCONNECT},
        {KVM_SRT_LOSS_UNPAIRED, KVM_RA_REMOTE_DISCONNECT},
        {KVM_SRT_LOSS_REMOTE_DISABLED, KVM_RA_LOCAL_OVERRIDE},
        {KVM_SRT_LOSS_MAINTENANCE, KVM_RA_MAINTENANCE},
        {KVM_SRT_LOSS_SHUTDOWN, KVM_RA_REMOTE_DISCONNECT},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        CHECK_EQ(kvm_srt_loss_release_reason(rows[i].loss), rows[i].release);
        CHECK(kvm_srt_loss_name(rows[i].loss) != NULL);
    }
    // Every loss value has a name, so a log line can never read "unknown" for a real loss.
    for (int i = 0; i < KVM_SRT_LOSS_COUNT; i++) {
        CHECK_EQ(strcmp(kvm_srt_loss_name((kvm_srt_loss_t)i), "unknown") != 0, true);
    }
}

static void t_remote_switched_off(void)
{
    CHECK_EQ(sh_init(&h, 0x63u), KVM_PAIR_OK);
    char good[80];
    (void)snprintf(good, sizeof(good), "%s", seed_client(&h, "Desk1", 0x44u));
    sh_online(&h);

    sh_cli_t c;
    CHECK(sh_cli_open(&h, &c, good, 1u));
    h.approval_answer = KVM_SRT_APPROVAL_PENDING;
    sh_cmd(&h, &c, "control.request", "'reason':'x'");
    kvm_srt_approval_result(&h.srt, c.id, true, sh_now_us());
    kvm_srt_set_owner(&h.srt, OWNER_REMOTE, c.id, h.epoch, sh_now_us());
    // 224 = 0xE0, Left Control: a modifier held down when the session is about to be dropped.
    sh_input_ep(&h, &c, h.epoch, "'kind':'keyboard.key','usage_id':224,'down':true");
    CHECK_EQ(h.nintents, 1);
    CHECK_EQ(h.intents[0].type, KVM_HID_KEY_DOWN);
    CHECK_EQ(h.intents[0].data.key.usage_id, 224);

    // Turning Remote off locally drops the session, which releases what it was holding, and the
    // reason says it was a local decision (ADR-011).
    h.nreleases = 0;
    sh_clear_wire(&h);
    kvm_srt_set_remote_enabled(&h.srt, false, sh_now_us());
    CHECK(sh_was_closed(&h, c.id));
    CHECK_EQ(h.nreleases, 1);
    CHECK_EQ(h.last_release, KVM_RA_LOCAL_OVERRIDE);
    CHECK_EQ(kvm_srt_slot_of(&h.srt, c.id), -1);
    // Nothing is admitted while it stays off.
    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
             KVM_SRT_ADMIT_FORBIDDEN);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_drop_all_release_all(void)
{
    CHECK_EQ(sh_init(&h, 0x64u), KVM_PAIR_OK);
    sh_online(&h);

    // The belt-and-braces path: REMOTE owns the HOST but no session is left to release through.
    // Nobody else would clear the ledger, so an unconditional RELEASE_ALL is asked for.
    kvm_srt_set_owner(&h.srt, OWNER_REMOTE, 7u, 3u, sh_now_us());
    kvm_srt_drop_all(&h.srt, KVM_SRT_LOSS_MAINTENANCE, true, sh_now_us());
    CHECK_EQ(h.nrelease_all, 1);
    CHECK_EQ(h.last_release_all, KVM_RA_MAINTENANCE);

    // And it does not fire when LOCAL owns: cutting into a local drag would be the bug, not the
    // fix (LOCAL > REMOTE, ADR-011).
    h.nrelease_all = 0;
    kvm_srt_set_owner(&h.srt, OWNER_LOCAL, 0u, 4u, sh_now_us());
    kvm_srt_drop_all(&h.srt, KVM_SRT_LOSS_MAINTENANCE, true, sh_now_us());
    CHECK_EQ(h.nrelease_all, 0);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_client_removal_revokes(void)
{
    CHECK_EQ(sh_init(&h, 0x65u), KVM_PAIR_OK);
    char a[80];
    char b[80];
    (void)snprintf(a, sizeof(a), "%s", seed_client(&h, "DeskA", 0x55u));
    (void)snprintf(b, sizeof(b), "%s", seed_client(&h, "DeskB", 0x66u));
    sh_online(&h);

    sh_cli_t ca;
    sh_cli_t cb;
    CHECK(sh_cli_open(&h, &ca, a, 1u));
    CHECK(sh_cli_open(&h, &cb, b, 2u));
    CHECK_EQ(kvm_srt_slot_of(&h.srt, ca.id) >= 0, true);
    CHECK_EQ(kvm_srt_slot_of(&h.srt, cb.id) >= 0, true);

    h.approval_answer = KVM_SRT_APPROVAL_PENDING;
    sh_cmd(&h, &ca, "control.request", "'reason':'x'");
    kvm_srt_approval_result(&h.srt, ca.id, true, sh_now_us());
    kvm_srt_set_owner(&h.srt, OWNER_REMOTE, ca.id, h.epoch, sh_now_us());

    // FR-726: removing a paired client takes effect immediately -- its session goes and, because it
    // held the lease, RELEASE_ALL is raised. The other client is untouched.
    h.nreleases = 0;
    sh_clear_wire(&h);
    CHECK(kvm_srt_clients_remove(&h.srt, 0, sh_now_us()));
    CHECK(sh_was_closed(&h, ca.id));
    CHECK_EQ(kvm_srt_slot_of(&h.srt, ca.id), -1);
    CHECK_EQ(h.nreleases, 1);
    CHECK_EQ(h.last_release, KVM_RA_REMOTE_DISCONNECT);
    CHECK(kvm_srt_slot_of(&h.srt, cb.id) >= 0);
    // The register compacted, so the surviving session now points at entry 0, not entry 1.
    CHECK_EQ(h.srt.conn[kvm_srt_slot_of(&h.srt, cb.id)].client_index, 0);
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 1);

    // Remove all: every control session goes.
    CHECK_EQ(kvm_srt_clients_remove_all(&h.srt, sh_now_us()), 1);
    CHECK_EQ(kvm_srt_slot_of(&h.srt, cb.id), -1);
    CHECK_EQ(kvm_srt_clients_count(&h.srt), 0);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_approval_policies(void)
{
    // FR-733. The policy may relax the prompt and nothing else: the lease, the epoch and the local
    // override are unaffected, and no CLIENT message can change the setting (SEC-107).
    CHECK_EQ(sh_init(&h, 0x66u), KVM_PAIR_OK);
    char good[80];
    (void)snprintf(good, sizeof(good), "%s", seed_client(&h, "Desk1", 0x77u));
    sh_online(&h);

    // `auto`: granted without a prompt.
    kvm_srt_set_policy(&h.srt, true, KVM_CONTROL_APPROVAL_AUTO);
    sh_cli_t c;
    CHECK(sh_cli_open(&h, &c, good, 1u));
    sh_clear_wire(&h);
    sh_cmd(&h, &c, "control.request", "'reason':'x'");
    CHECK_EQ(h.napproval_begin, 0);
    CHECK(sh_sent_contains(&h, "control.granted"));
    CHECK_EQ(kvm_srt_counters(&h.srt)->approvals_auto, 1);
    kvm_srt_conn_close(&h.srt, c.id, KVM_SRT_LOSS_CLIENT_CLOSE, sh_now_us());

    // `trusted` with the entry not marked trusted: still a prompt.
    kvm_srt_set_policy(&h.srt, true, KVM_CONTROL_APPROVAL_TRUSTED);
    h.trusted[0] = false;
    h.approval_answer = KVM_SRT_APPROVAL_PENDING;
    CHECK(sh_cli_open(&h, &c, good, 2u));
    sh_clear_wire(&h);
    sh_cmd(&h, &c, "control.request", "'reason':'x'");
    CHECK_EQ(h.napproval_begin, 1);
    CHECK(sh_sent_contains(&h, "control.pending"));
    kvm_srt_conn_close(&h.srt, c.id, KVM_SRT_LOSS_CLIENT_CLOSE, sh_now_us());

    // `trusted` with the entry marked trusted: no prompt.
    h.trusted[0] = true;
    h.napproval_begin = 0;
    CHECK(sh_cli_open(&h, &c, good, 3u));
    sh_clear_wire(&h);
    sh_cmd(&h, &c, "control.request", "'reason':'x'");
    CHECK_EQ(h.napproval_begin, 0);
    CHECK(sh_sent_contains(&h, "control.granted"));
    CHECK_EQ(kvm_srt_counters(&h.srt)->approvals_trusted, 1);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_conn_bookkeeping(void)
{
    CHECK_EQ(sh_init(&h, 0x67u), KVM_PAIR_OK);
    char good[80];
    (void)snprintf(good, sizeof(good), "%s", seed_client(&h, "Desk1", 0x88u));
    sh_online(&h);

    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);
    CHECK(kvm_srt_conn_open(&h.srt, 1u, &adm, sh_now_us()) >= 0);
    // A connection id of zero, or one that is already open, is refused rather than overwriting a
    // live session; a reused socket descriptor must not be able to collide.
    CHECK_EQ(kvm_srt_conn_open(&h.srt, 1u, &adm, sh_now_us()), -1);
    CHECK_EQ(kvm_srt_conn_open(&h.srt, 0u, &adm, sh_now_us()), -1);
    CHECK_EQ(kvm_srt_conn_kind(&h.srt, 1u), KVM_SRT_CONN_CONTROL);
    CHECK_EQ(kvm_srt_conn_kind(&h.srt, 999u), KVM_SRT_CONN_KINDS);

    // The session table is finite; a further peer is told the device is busy, not let in.
    for (uint64_t id = 2u; id <= (uint64_t)KVM_SRT_MAX_CONTROL; id++) {
        CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
                 KVM_SRT_ADMIT_OK);
        CHECK(kvm_srt_conn_open(&h.srt, id, &adm, sh_now_us()) >= 0);
    }
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_CONTROL, good, sh_now_us(), &adm),
             KVM_SRT_ADMIT_BUSY);

    // Closing an unknown connection is a no-op, not a crash or a spurious release.
    h.nreleases = 0;
    kvm_srt_conn_close(&h.srt, 4242u, KVM_SRT_LOSS_SOCKET, sh_now_us());
    CHECK_EQ(h.nreleases, 0);

    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_no_secrets_on_the_wire(void)
{
    // SEC-105 / TECH-046 / AT-SEC-08: a full pairing run plus a control session, then a sweep of
    // every byte that left the device for the code, the token and the certificate hash.
    CHECK_EQ(sh_init(&h, 0x68u), KVM_PAIR_OK);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);
    char typed[KVM_PAIR_CODE_CAP];
    (void)snprintf(typed, sizeof(typed), "%s", kvm_srt_pair_code(&h.srt));

    run_t r;
    CHECK(run_pair(&r, typed, "Desk1", h.cfg.cert_sha256, true));

    // Everything the transport ever sent, from the first frame of the run onwards.
    char token_b64[KVM_SRT_TOKEN_B64URL_CAP];
    CHECK(kvm_srt_b64url_encode(r.token, sizeof(r.token), token_b64, sizeof(token_b64)));
    char token_std[KVM_PAIR_TOKEN_B64_CAP];
    CHECK(kvm_pair_b64_encode(r.token, sizeof(r.token), token_std, sizeof(token_std)));
    char cert_hex[KVM_PAIR_HEX32_CAP];
    CHECK(kvm_pair_hex_encode(h.cfg.cert_sha256, KVM_PAIR_SHA256_LEN, cert_hex, sizeof(cert_hex)));

    for (int i = 0; i < h.nsent; i++) {
        // The code never travels: the user carries it from the screen to the keyboard (FR-721).
        CHECK(strstr(h.sent[i].text, typed) == NULL);
        // The token travels only sealed, so neither encoding of the plaintext may appear.
        CHECK(strstr(h.sent[i].text, token_b64) == NULL);
        CHECK(strstr(h.sent[i].text, token_std) == NULL);
        // The certificate hash is the CLIENT's pin, taken from the handshake, not told to it.
        CHECK(strstr(h.sent[i].text, cert_hex) == NULL);
    }

    kvm_pair_client_free(&r.cli);
    kvm_srt_shutdown(&h.srt, sh_now_us());
}

static void t_init_fails_closed(void)
{
    // A pairing core that could not start means there is no register to verify a token against.
    // Admission then refuses everything instead of running without authentication.
    sh_t bad;
    memset(&bad, 0, sizeof(bad));
    kvm_srt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    const kvm_srt_hooks_t hooks = {.ctx = &bad};
    // No platform hooks at all: kvm_pair_session_init must refuse.
    CHECK(kvm_srt_init(&bad.srt, &cfg, &hooks) != KVM_PAIR_OK);

    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&bad.srt, KVM_SRT_CONN_CONTROL, "Bearer x", 0u, &adm),
             KVM_SRT_ADMIT_FORBIDDEN);
    CHECK_EQ(kvm_srt_admit(&bad.srt, KVM_SRT_CONN_PAIRING, NULL, 0u, &adm),
             KVM_SRT_ADMIT_FORBIDDEN);
    CHECK(!kvm_srt_pair_endpoint_open(&bad.srt));
    CHECK(kvm_srt_pair_code(&bad.srt) == NULL);
    CHECK_EQ(kvm_srt_clients_count(&bad.srt), 0);
}

int main(void)
{
    RUN(t_pairing_full_run);
    RUN(t_pairing_require_accept_off);
    RUN(t_pairing_wrong_code);
    RUN(t_pairing_cert_mismatch);
    RUN(t_pairing_endpoint_limits);
    RUN(t_pairing_bad_frame_budget);
    RUN(t_pairing_preconditions);
    RUN(t_pairing_accept_timeout);
    RUN(t_admission);
    RUN(t_admission_cooldown);
    RUN(t_control_session);
    RUN(t_loss_reasons);
    RUN(t_remote_switched_off);
    RUN(t_drop_all_release_all);
    RUN(t_client_removal_revokes);
    RUN(t_approval_policies);
    RUN(t_conn_bookkeeping);
    RUN(t_no_secrets_on_the_wire);
    RUN(t_init_fails_closed);
    TEST_MAIN_END("test_srt_session");
}
