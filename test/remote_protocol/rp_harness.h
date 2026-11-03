// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host harness for the remote_protocol suites: a fake integrator (arbiter, UI approval, transport)
// that records every intent, frame, release reason and ownership change, plus a small frame writer
// so the tests can stay readable.
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "remote_protocol.h"

#define RPH_MAX_INTENTS 2048
#define RPH_MAX_FRAMES 64
#define RPH_MAX_EVENTS 128

typedef struct {
    kvm_rp_t rp;

    // recorded HID intents (what the arbiter would have been asked to do)
    kvm_hid_intent_t intent[RPH_MAX_INTENTS];
    int n_intent;

    // recorded outbound frames
    char frame[RPH_MAX_FRAMES][KVM_RP_OUT_MAX + 1];
    int n_frame;

    // recorded release reasons (RELEASE_ALL requests through control_release)
    kvm_release_reason_t release[RPH_MAX_EVENTS];
    int n_release;

    int n_approval_begin;
    int n_approval_cancel;
    int n_close;
    int n_heartbeat;
    char last_approval_name[KVM_RP_NAME_MAX];
    char last_approval_reason[KVM_RP_REASON_MAX];

    // behavior knobs
    uint32_t epoch;                 // the fake arbiter's epoch
    kvm_rp_err_t request_result;    // control_request probe result
    kvm_rp_err_t grant_result;      // control_grant result
    kvm_rp_err_t sink_result;       // hid_sink result
    kvm_rp_approval_t approval;     // approval_begin result
    bool auto_close;                // close_session -> kvm_rp_session_close
    bool pair_hook_installed;
    kvm_rp_err_t pair_result;
    int n_pair;

    uint64_t now;
} rph_t;

static rph_t g_h;

// ---- fake integrator -----------------------------------------------------------------------

static inline kvm_rp_err_t rph_sink(void *ctx, uint64_t session_id, uint32_t lease_epoch,
                             const kvm_hid_intent_t *intent)
{
    (void)ctx;
    (void)session_id;
    (void)lease_epoch;
    if (g_h.sink_result != KVM_RP_OK) {
        return g_h.sink_result;
    }
    if (g_h.n_intent < RPH_MAX_INTENTS) {
        g_h.intent[g_h.n_intent] = *intent;
    }
    g_h.n_intent++;
    return KVM_RP_OK;
}

static inline kvm_rp_err_t rph_control_request(void *ctx, uint64_t session_id)
{
    (void)ctx;
    (void)session_id;
    return g_h.request_result;
}

static inline kvm_rp_err_t rph_control_grant(void *ctx, uint64_t session_id, uint32_t *lease_epoch)
{
    (void)ctx;
    (void)session_id;
    if (g_h.grant_result != KVM_RP_OK) {
        return g_h.grant_result;
    }
    g_h.epoch++; // 03 §4.3: a grant always increases the epoch
    *lease_epoch = g_h.epoch;
    return KVM_RP_OK;
}

static inline void rph_control_release(void *ctx, uint64_t session_id, kvm_release_reason_t reason,
                                uint32_t *new_epoch)
{
    (void)ctx;
    (void)session_id;
    if (g_h.n_release < RPH_MAX_EVENTS) {
        g_h.release[g_h.n_release] = reason;
    }
    g_h.n_release++;
    g_h.epoch++; // revoke + RELEASE_ALL + epoch bump
    *new_epoch = g_h.epoch;
}

static inline void rph_heartbeat(void *ctx, uint64_t session_id)
{
    (void)ctx;
    (void)session_id;
    g_h.n_heartbeat++;
}

static inline kvm_rp_approval_t rph_approval_begin(void *ctx, uint64_t session_id, const char *client_name,
                                            const char *reason)
{
    (void)ctx;
    (void)session_id;
    g_h.n_approval_begin++;
    snprintf(g_h.last_approval_name, sizeof(g_h.last_approval_name), "%s", client_name);
    snprintf(g_h.last_approval_reason, sizeof(g_h.last_approval_reason), "%s", reason);
    return g_h.approval;
}

static inline void rph_approval_cancel(void *ctx, uint64_t session_id)
{
    (void)ctx;
    (void)session_id;
    g_h.n_approval_cancel++;
}

static inline void rph_send(void *ctx, uint64_t session_id, const char *frame, size_t len)
{
    (void)ctx;
    (void)session_id;
    // ring buffer: the newest RPH_MAX_FRAMES frames are always inspectable
    if (len <= KVM_RP_OUT_MAX) {
        int slot = g_h.n_frame % RPH_MAX_FRAMES;
        memcpy(g_h.frame[slot], frame, len);
        g_h.frame[slot][len] = '\0';
    }
    g_h.n_frame++;
}

static inline void rph_close(void *ctx, uint64_t session_id, kvm_rp_err_t reason)
{
    (void)ctx;
    (void)reason;
    g_h.n_close++;
    if (g_h.auto_close) {
        kvm_rp_session_close(&g_h.rp, session_id, KVM_RP_REVOKE_SESSION_CLOSED, g_h.now);
    }
}

static inline void rph_status_fill(void *ctx, kvm_rp_status_t *out)
{
    (void)ctx;
    snprintf(out->device_id, sizeof(out->device_id), "kvm-7f3a");
    snprintf(out->fw, sizeof(out->fw), "1.0.0");
    snprintf(out->host_layout, sizeof(out->host_layout), "pl(dvorak)");
}

static inline kvm_rp_err_t rph_pair(void *ctx, uint64_t session_id, const char *type,
                             const kvm_json_doc_t *doc, int payload)
{
    (void)ctx;
    (void)session_id;
    (void)type;
    (void)doc;
    (void)payload;
    g_h.n_pair++;
    return g_h.pair_result;
}

// ---- setup ---------------------------------------------------------------------------------

static inline void rph_reset(const kvm_rp_config_t *cfg)
{
    memset(&g_h, 0, sizeof(g_h));
    g_h.approval = KVM_RP_APPROVAL_PENDING;
    g_h.auto_close = true;
    g_h.now = 1000000u;
    kvm_rp_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.hid_sink = rph_sink;
    hooks.control_request = rph_control_request;
    hooks.control_grant = rph_control_grant;
    hooks.control_release = rph_control_release;
    hooks.heartbeat = rph_heartbeat;
    hooks.approval_begin = rph_approval_begin;
    hooks.approval_cancel = rph_approval_cancel;
    hooks.send = rph_send;
    hooks.close_session = rph_close;
    hooks.status_fill = rph_status_fill;
    kvm_rp_init(&g_h.rp, cfg, &hooks);
    kvm_rp_set_host(&g_h.rp, KVM_HOST_USB_MOUNTED, false, true, g_h.now);
    kvm_rp_set_owner(&g_h.rp, OWNER_NONE, 0, 0, g_h.now);
}

static inline void rph_install_pair_hook(void)
{
    g_h.rp.hooks.pair_frame = rph_pair;
    g_h.pair_hook_installed = true;
}

/** Frame writer: single quotes become double quotes so the test literals stay readable. */
static inline const char *jf(const char *fmt, ...)
{
    static char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (size_t i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '\'') {
            buf[i] = '"';
        }
    }
    return buf;
}

static inline kvm_rp_err_t rph_feed(uint64_t sid, const char *frame)
{
    return kvm_rp_on_frame(&g_h.rp, sid, frame, strlen(frame), g_h.now);
}

/** Open a session and complete `hello`; returns the assigned wire label. */
static inline const char *rph_hello(uint64_t sid, bool op_role)
{
    kvm_rp_session_info_t info;
    memset(&info, 0, sizeof(info));
    info.session_id = sid;
    info.authenticated = true;
    info.operator_role = op_role;
    info.label_entropy = (uint32_t)(sid * 7919u);
    (void)kvm_rp_session_open(&g_h.rp, &info, g_h.now);
    (void)rph_feed(sid, jf("{'v':1,'type':'hello','seq':1,'payload':{'client_name':'Desk1',"
                           "'capabilities':['hid_v1']}}"));
    return kvm_rp_session_label(&g_h.rp, sid);
}

// ---- client-side convenience ---------------------------------------------------------------

typedef struct {
    uint64_t sid;
    char label[KVM_RP_LABEL_MAX];
    uint64_t q; // last client seq used
} rph_cli_t;

/** Open an authenticated session and complete `hello` (seq 1). */
static inline void rph_cli_open(rph_cli_t *c, uint64_t sid, bool op_role)
{
    c->sid = sid;
    c->q = 1;
    snprintf(c->label, sizeof(c->label), "%s", rph_hello(sid, op_role));
}

/** Any client->server frame with an empty or given payload body (single quotes become quotes). */
static inline kvm_rp_err_t rph_cmd(rph_cli_t *c, const char *type, const char *payload)
{
    c->q++;
    return rph_feed(c->sid, jf("{'v':1,'type':'%s','seq':%llu,'session_id':'%s','payload':{%s}}",
                               type, (unsigned long long)c->q, c->label, payload));
}

/** One `input` frame with an explicit lease epoch. */
static inline kvm_rp_err_t rph_input_ep(rph_cli_t *c, uint32_t epoch, const char *payload)
{
    c->q++;
    return rph_feed(c->sid,
                    jf("{'v':1,'type':'input','seq':%llu,'session_id':'%s','lease_epoch':%u,"
                       "'payload':{%s}}",
                       (unsigned long long)c->q, c->label, epoch, payload));
}

/** One `input` frame with the epoch the device currently holds. */
static inline kvm_rp_err_t rph_input(rph_cli_t *c, const char *payload)
{
    return rph_input_ep(c, g_h.rp.owner_epoch, payload);
}

/** Request and receive control through the normal approval path. Returns the lease epoch. */
static inline uint32_t rph_cli_grant(rph_cli_t *c)
{
    g_h.approval = KVM_RP_APPROVAL_GRANTED;
    (void)rph_cmd(c, "control.request", "'reason':'remote_support'");
    return g_h.rp.owner_epoch;
}

/** Keep the lease alive without advancing the fake clock past the stale window. */
static inline void rph_ping(rph_cli_t *c)
{
    (void)rph_cmd(c, "ping", "'nonce':101");
}

/** Does the last outbound frame contain this substring? */
static inline bool rph_last_has(const char *needle)
{
    if (g_h.n_frame == 0) {
        return false;
    }
    return strstr(g_h.frame[(g_h.n_frame - 1) % RPH_MAX_FRAMES], needle) != NULL;
}

static inline bool rph_any_has(const char *needle)
{
    int n = (g_h.n_frame < RPH_MAX_FRAMES) ? g_h.n_frame : RPH_MAX_FRAMES;
    for (int i = 0; i < n; i++) {
        if (strstr(g_h.frame[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static inline bool rph_released_with(kvm_release_reason_t reason)
{
    int n = (g_h.n_release < RPH_MAX_EVENTS) ? g_h.n_release : RPH_MAX_EVENTS;
    for (int i = 0; i < n; i++) {
        if (g_h.release[i] == reason) {
            return true;
        }
    }
    return false;
}
