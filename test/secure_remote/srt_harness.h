// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Fixture for the secure-remote transport tests: a kvm_srt_t wired to recording hooks, so a test
// can assert on what left towards the HID path, what went on the wire and which RELEASE_ALL reason
// a loss produced -- without a socket, a TLS stack or a board.
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "det_rng.h"
#include "fake_pair_store.h"
#include "kvm_pairing_wire.h"
#include "kvm_srt.h"

#define SH_FRAMES_MAX 64
#define SH_INTENTS_MAX 64
#define SH_FRAME_CAP (KVM_JSON_MAX_FRAME + 1)

typedef struct {
    uint64_t conn_id;
    kvm_srt_conn_kind_t kind;
    char text[SH_FRAME_CAP];
    size_t len;
} sh_frame_t;

typedef struct {
    kvm_srt_t srt;
    kvm_srt_config_t cfg;
    fake_pair_store_t store;

    // what the transport was asked to send / close
    sh_frame_t sent[SH_FRAMES_MAX];
    int nsent;
    uint64_t closed[SH_FRAMES_MAX];
    int nclosed;

    // what reached the HID path
    kvm_hid_intent_t intents[SH_INTENTS_MAX];
    uint64_t intent_session[SH_INTENTS_MAX];
    uint32_t intent_epoch[SH_INTENTS_MAX];
    int nintents;
    kvm_hid_err_t intent_result; // what the fake arbiter returns

    // ownership / release bookkeeping
    uint32_t epoch;
    uint64_t owner_session;
    kvm_release_reason_t releases[KVM_RA_COUNT];
    int nreleases;
    kvm_release_reason_t last_release;
    int nrelease_all;
    kvm_release_reason_t last_release_all;
    int nheartbeats;

    // control approval
    kvm_srt_approval_t approval_answer;
    int napproval_begin;
    int napproval_cancel;
    char approval_name[KVM_RP_NAME_MAX];
    uint64_t approval_session;

    // pairing events
    kvm_pair_event_t events[16];
    int nevents;

    bool trusted[FAKE_PAIR_MAX];
    bool grant_denied;
    bool request_denied;
} sh_t;

// --- hooks ---------------------------------------------------------------------------------------

static inline kvm_hid_err_t sh_hid_intent(void *ctx, uint64_t session_id, uint32_t lease_epoch,
                                   const kvm_hid_intent_t *intent)
{
    sh_t *h = (sh_t *)ctx;
    if (h->intent_result == KVM_HID_OK && h->nintents < SH_INTENTS_MAX) {
        h->intents[h->nintents] = *intent;
        h->intent_session[h->nintents] = session_id;
        h->intent_epoch[h->nintents] = lease_epoch;
        h->nintents++;
    }
    return h->intent_result;
}

static inline kvm_hid_err_t sh_control_request(void *ctx, uint64_t session_id)
{
    sh_t *h = (sh_t *)ctx;
    (void)session_id;
    return h->request_denied ? KVM_HID_ERR_CONTROL_DENIED : KVM_HID_OK;
}

static inline kvm_hid_err_t sh_control_grant(void *ctx, uint64_t session_id, uint32_t *lease_epoch)
{
    sh_t *h = (sh_t *)ctx;
    if (h->grant_denied) {
        return KVM_HID_ERR_CONTROL_DENIED;
    }
    h->epoch++;
    h->owner_session = session_id;
    *lease_epoch = h->epoch;
    return KVM_HID_OK;
}

static inline void sh_control_release(void *ctx, uint64_t session_id, kvm_release_reason_t reason,
                               uint32_t *new_epoch)
{
    sh_t *h = (sh_t *)ctx;
    (void)session_id;
    h->epoch++;
    h->owner_session = 0;
    h->last_release = reason;
    if (h->nreleases < KVM_RA_COUNT) {
        h->releases[h->nreleases++] = reason;
    }
    *new_epoch = h->epoch;
}

static inline void sh_heartbeat(void *ctx, uint64_t session_id)
{
    (void)session_id;
    ((sh_t *)ctx)->nheartbeats++;
}

static inline void sh_release_all(void *ctx, kvm_release_reason_t reason)
{
    sh_t *h = (sh_t *)ctx;
    h->nrelease_all++;
    h->last_release_all = reason;
}

static inline void sh_send(void *ctx, uint64_t conn_id, kvm_srt_conn_kind_t kind, const char *frame,
                    size_t len)
{
    sh_t *h = (sh_t *)ctx;
    if (h->nsent >= SH_FRAMES_MAX || len >= SH_FRAME_CAP) {
        return;
    }
    sh_frame_t *f = &h->sent[h->nsent++];
    f->conn_id = conn_id;
    f->kind = kind;
    memcpy(f->text, frame, len);
    f->text[len] = '\0';
    f->len = len;
}

static inline void sh_close(void *ctx, uint64_t conn_id, kvm_srt_conn_kind_t kind)
{
    sh_t *h = (sh_t *)ctx;
    (void)kind;
    if (h->nclosed < SH_FRAMES_MAX) {
        h->closed[h->nclosed++] = conn_id;
    }
}

static inline void sh_status_fill(void *ctx, kvm_rp_status_t *out)
{
    sh_t *h = (sh_t *)ctx;
    memset(out, 0, sizeof(*out));
    out->host_usb = KVM_HOST_USB_MOUNTED;
    out->wifi_connected = true;
    out->owner = (h->owner_session != 0u) ? OWNER_REMOTE : OWNER_NONE;
    out->lease_epoch = h->epoch;
    snprintf(out->device_id, sizeof(out->device_id), "kvm-test");
    snprintf(out->fw, sizeof(out->fw), "1.0.0");
    snprintf(out->host_layout, sizeof(out->host_layout), "us");
}

static inline kvm_srt_approval_t sh_approval_begin(void *ctx, uint64_t session_id, const char *client_name,
                                            const char *reason)
{
    sh_t *h = (sh_t *)ctx;
    (void)reason;
    h->napproval_begin++;
    h->approval_session = session_id;
    snprintf(h->approval_name, sizeof(h->approval_name), "%s",
             (client_name != NULL) ? client_name : "");
    return h->approval_answer;
}

static inline void sh_approval_cancel(void *ctx, uint64_t session_id)
{
    (void)session_id;
    ((sh_t *)ctx)->napproval_cancel++;
}

static inline bool sh_client_trusted(void *ctx, int client_index)
{
    sh_t *h = (sh_t *)ctx;
    if (client_index < 0 || (unsigned)client_index >= FAKE_PAIR_MAX) {
        return false;
    }
    return h->trusted[client_index];
}

static inline void sh_pair_event(void *ctx, const kvm_pair_event_t *event)
{
    sh_t *h = (sh_t *)ctx;
    if (h->nevents < 16) {
        h->events[h->nevents++] = *event;
    }
}

// --- setup ---------------------------------------------------------------------------------------

/** Certificate hash of the fake DEVICE identity. Not a key and not a secret: a fixture constant. */
static inline void sh_cert_hash(uint8_t out[KVM_PAIR_SHA256_LEN])
{
    for (unsigned i = 0; i < KVM_PAIR_SHA256_LEN; i++) {
        out[i] = (uint8_t)(0xA0u + i);
    }
}

static inline kvm_pair_err_t sh_init(sh_t *h, uint64_t seed)
{
    memset(h, 0, sizeof(*h));
    det_rng_seed(seed);
    det_clock_reset();

    fake_pair_store_init(&h->store, &h->cfg.store);
    h->cfg.platform = *det_platform();
    h->cfg.rp = NULL; // KVM_RP_CONFIG_DEFAULT
    h->cfg.pair_require_accept = true;
    h->cfg.control_approval = KVM_CONTROL_APPROVAL_ALWAYS;
    snprintf(h->cfg.device_name, sizeof(h->cfg.device_name), "KVMpad-TEST");
    sh_cert_hash(h->cfg.cert_sha256);

    h->intent_result = KVM_HID_OK;
    h->approval_answer = KVM_SRT_APPROVAL_PENDING;

    const kvm_srt_hooks_t hooks = {
        .hid_intent = sh_hid_intent,
        .control_request = sh_control_request,
        .control_grant = sh_control_grant,
        .control_release = sh_control_release,
        .heartbeat = sh_heartbeat,
        .release_all = sh_release_all,
        .send = sh_send,
        .close = sh_close,
        .status_fill = sh_status_fill,
        .approval_begin = sh_approval_begin,
        .approval_cancel = sh_approval_cancel,
        .client_trusted = sh_client_trusted,
        .pair_event = sh_pair_event,
        .ctx = h,
    };
    return kvm_srt_init(&h->srt, &h->cfg, &hooks);
}

/** The usual starting point: Remote on, Wi-Fi up, HOST mounted, nothing owned. */
static inline void sh_online(sh_t *h)
{
    kvm_srt_set_remote_enabled(&h->srt, true, det_clock_now() * 1000u);
    kvm_srt_set_host(&h->srt, KVM_HOST_USB_MOUNTED, false, true, det_clock_now() * 1000u);
    kvm_srt_set_owner(&h->srt, OWNER_NONE, 0u, h->epoch, det_clock_now() * 1000u);
}

static inline uint64_t sh_now_us(void)
{
    return det_clock_now() * 1000u;
}

static inline void sh_clear_wire(sh_t *h)
{
    h->nsent = 0;
    h->nclosed = 0;
}

/** The newest frame sent on a connection, or NULL. */
static inline const sh_frame_t *sh_last(const sh_t *h)
{
    return (h->nsent > 0) ? &h->sent[h->nsent - 1] : NULL;
}

/** True when any sent frame contains `needle` verbatim. */
static inline bool sh_sent_contains(const sh_t *h, const char *needle)
{
    for (int i = 0; i < h->nsent; i++) {
        if (strstr(h->sent[i].text, needle) != NULL) {
            return true;
        }
    }
    return false;
}

static inline bool sh_was_closed(const sh_t *h, uint64_t conn_id)
{
    for (int i = 0; i < h->nclosed; i++) {
        if (h->closed[i] == conn_id) {
            return true;
        }
    }
    return false;
}

/** Open an admitted control connection for `token`, returning the connection id or 0. */
static inline uint64_t sh_open_control(sh_t *h, const char *authorization, uint64_t conn_id)
{
    kvm_srt_admission_t adm;
    if (kvm_srt_admit(&h->srt, KVM_SRT_CONN_CONTROL, authorization, sh_now_us(), &adm) !=
        KVM_SRT_ADMIT_OK) {
        return 0u;
    }
    if (kvm_srt_conn_open(&h->srt, conn_id, &adm, sh_now_us()) < 0) {
        return 0u;
    }
    return conn_id;
}

static inline void sh_frame(sh_t *h, uint64_t conn_id, const char *text)
{
    kvm_srt_conn_frame(&h->srt, conn_id, text, strlen(text), sh_now_us());
}

/** Frame writer: single quotes become double quotes so the test literals stay readable. */
static inline const char *sh_jf(const char *fmt, ...)
{
    static char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (size_t i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '\'') {
            buf[i] = '"';
        }
    }
    return buf;
}

/** A control client: its connection id, the wire label the DEVICE assigned and its own `seq`. */
typedef struct {
    uint64_t id;
    char label[KVM_RP_LABEL_MAX];
    uint64_t q;
} sh_cli_t;

/** Open a control connection for `authorization` and complete `hello`. */
static inline bool sh_cli_open(sh_t *h, sh_cli_t *c, const char *authorization, uint64_t conn_id)
{
    memset(c, 0, sizeof(*c));
    c->id = sh_open_control(h, authorization, conn_id);
    if (c->id == 0u) {
        return false;
    }
    c->q = 1;
    sh_frame(h, c->id,
             sh_jf("{'v':1,'type':'hello','seq':1,'payload':{'client_name':'Desk1',"
                   "'capabilities':['hid_v1']}}"));
    snprintf(c->label, sizeof(c->label), "%s", kvm_rp_session_label(&h->srt.rp, c->id));
    return true;
}

static inline void sh_cmd(sh_t *h, sh_cli_t *c, const char *type, const char *payload)
{
    c->q++;
    sh_frame(h, c->id,
             sh_jf("{'v':1,'type':'%s','seq':%llu,'session_id':'%s','payload':{%s}}", type,
                   (unsigned long long)c->q, c->label, payload));
}

static inline void sh_input_ep(sh_t *h, sh_cli_t *c, uint32_t epoch, const char *payload)
{
    c->q++;
    sh_frame(h, c->id,
             sh_jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                   "'payload':{%s}}",
                   (unsigned long long)c->q, c->label, epoch, payload));
}

/** Copy a string member of the newest sent frame's payload, e.g. "blob" or "token_enc". */
static inline bool sh_payload_str(const sh_t *h, const char *field, char *out, size_t cap)
{
    static kvm_json_doc_t d;
    const sh_frame_t *f = sh_last(h);
    if (f == NULL || kvm_json_parse(&d, f->text, f->len) != KVM_JSON_OK) {
        return false;
    }
    const int payload = kvm_json_member(&d, kvm_json_root(&d), "payload");
    const int tok = kvm_json_member(&d, payload, field);
    return kvm_json_str_copy(&d, tok, out, cap) != (size_t)-1;
}

/** Decode a base64 payload field of the newest sent frame. */
static inline bool sh_payload_blob(const sh_t *h, const char *field, uint8_t *out, size_t cap, size_t *n)
{
    char b64[KVM_PAIR_ROUND1_B64_CAP];
    if (!sh_payload_str(h, field, b64, sizeof(b64))) {
        return false;
    }
    return kvm_pair_b64_decode(b64, out, cap, n);
}

/** The `type` of the newest sent frame, or "" when there is none. */
static inline const char *sh_last_type(const sh_t *h)
{
    static char type[32];
    type[0] = '\0';
    static kvm_json_doc_t d;
    const sh_frame_t *f = sh_last(h);
    if (f == NULL || kvm_json_parse(&d, f->text, f->len) != KVM_JSON_OK) {
        return type;
    }
    const int tok = kvm_json_member(&d, kvm_json_root(&d), "type");
    if (kvm_json_str_copy(&d, tok, type, sizeof(type)) == (size_t)-1) {
        type[0] = '\0';
    }
    return type;
}
