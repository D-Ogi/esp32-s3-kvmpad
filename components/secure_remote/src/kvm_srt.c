// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Secure remote transport session glue (TASK-100). See kvm_srt.h.

#include "kvm_srt.h"

#include <string.h>

// ---- loss mapping --------------------------------------------------------------------------

const char *kvm_srt_loss_name(kvm_srt_loss_t loss)
{
    switch (loss) {
    case KVM_SRT_LOSS_CLIENT_CLOSE:
        return "client_close";
    case KVM_SRT_LOSS_SOCKET:
        return "socket";
    case KVM_SRT_LOSS_TLS:
        return "tls";
    case KVM_SRT_LOSS_TIMEOUT:
        return "timeout";
    case KVM_SRT_LOSS_PROTOCOL:
        return "protocol";
    case KVM_SRT_LOSS_UNPAIRED:
        return "unpaired";
    case KVM_SRT_LOSS_REMOTE_DISABLED:
        return "remote_disabled";
    case KVM_SRT_LOSS_MAINTENANCE:
        return "maintenance";
    case KVM_SRT_LOSS_SHUTDOWN:
        return "shutdown";
    default:
        return "unknown";
    }
}

kvm_rp_revoke_reason_t kvm_srt_loss_revoke_reason(kvm_srt_loss_t loss)
{
    switch (loss) {
    case KVM_SRT_LOSS_TIMEOUT:
        return KVM_RP_REVOKE_CLIENT_STALE;
    case KVM_SRT_LOSS_PROTOCOL:
        return KVM_RP_REVOKE_PROTOCOL_ABUSE;
    case KVM_SRT_LOSS_UNPAIRED:
        return KVM_RP_REVOKE_UNPAIRED;
    case KVM_SRT_LOSS_REMOTE_DISABLED:
        // The user switched Remote off on the DEVICE, which is a local decision overriding a
        // remote lease (ADR-011), not a network failure.
        return KVM_RP_REVOKE_LOCAL_OVERRIDE;
    case KVM_SRT_LOSS_MAINTENANCE:
        return KVM_RP_REVOKE_MAINTENANCE;
    case KVM_SRT_LOSS_CLIENT_CLOSE:
    case KVM_SRT_LOSS_SOCKET:
    case KVM_SRT_LOSS_TLS:
    case KVM_SRT_LOSS_SHUTDOWN:
    default:
        return KVM_RP_REVOKE_SESSION_CLOSED;
    }
}

kvm_release_reason_t kvm_srt_loss_release_reason(kvm_srt_loss_t loss)
{
    return kvm_rp_release_reason(kvm_srt_loss_revoke_reason(loss));
}

const char *kvm_srt_admit_name(kvm_srt_admit_t admit)
{
    switch (admit) {
    case KVM_SRT_ADMIT_OK:
        return "OK";
    case KVM_SRT_ADMIT_UNAUTHORIZED:
        return "UNAUTHORIZED";
    case KVM_SRT_ADMIT_FORBIDDEN:
        return "FORBIDDEN";
    case KVM_SRT_ADMIT_BUSY:
        return "BUSY";
    default:
        return "UNKNOWN";
    }
}

// The one place the pairing core's result codes become protocol codes. The three groups are the
// rows of ADR-029 §5, and test_srt_pair_json checks that each group's wire string is the one
// kvm_pair_err_wire_code() produces, so the two tables cannot drift apart.
static kvm_rp_err_t rp_err_of_pair(kvm_pair_err_t err)
{
    switch (err) {
    case KVM_PAIR_OK:
        return KVM_RP_OK;
    case KVM_PAIR_ERR_FRAME:
    case KVM_PAIR_ERR_ARG:
        return KVM_RP_ERR_BAD_FRAME;
    case KVM_PAIR_ERR_BAD_CODE:
    case KVM_PAIR_ERR_BINDING:
    case KVM_PAIR_ERR_FULL:
    case KVM_PAIR_ERR_CRYPTO:
    case KVM_PAIR_ERR_PRECONDITION:
        return KVM_RP_ERR_CONTROL_DENIED;
    case KVM_PAIR_ERR_STATE:
    case KVM_PAIR_ERR_EXPIRED:
    case KVM_PAIR_ERR_LOCKED:
    case KVM_PAIR_ERR_BUSY:
    case KVM_PAIR_ERR_REJECTED:
    default:
        // Five distinct internal states collapse onto one code on purpose: a probe must not be
        // able to tell a closed window from an expired code from a lockout (ADR-029 §5).
        return KVM_RP_ERR_PAIRING_EXPIRED;
    }
}

// ---- small helpers -------------------------------------------------------------------------

static kvm_srt_conn_t *conn_of(kvm_srt_t *srt, uint64_t id)
{
    if (id == 0u) {
        return NULL;
    }
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (srt->conn[i].state != 0u && srt->conn[i].id == id) {
            return &srt->conn[i];
        }
    }
    return NULL;
}

static kvm_srt_conn_t *pairing_conn(kvm_srt_t *srt)
{
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (srt->conn[i].state != 0u && srt->conn[i].kind == (uint8_t)KVM_SRT_CONN_PAIRING) {
            return &srt->conn[i];
        }
    }
    return NULL;
}

static int control_count(const kvm_srt_t *srt)
{
    int n = 0;
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (srt->conn[i].state != 0u && srt->conn[i].kind == (uint8_t)KVM_SRT_CONN_CONTROL) {
            n++;
        }
    }
    return n;
}

static void conn_free(kvm_srt_conn_t *cn)
{
    memset(cn, 0, sizeof(*cn));
    cn->client_index = -1;
}

static int client_index_of(const kvm_srt_t *srt, uint64_t session_id)
{
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (srt->conn[i].state != 0u && srt->conn[i].id == session_id) {
            return srt->conn[i].client_index;
        }
    }
    return -1;
}

static uint32_t srt_rand32(kvm_srt_t *srt)
{
    uint8_t b[4] = {0, 0, 0, 0};
    if (srt->cfg.platform.random != NULL) {
        (void)srt->cfg.platform.random(srt->cfg.platform.ctx, b, sizeof(b));
    }
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static void ask_close(kvm_srt_t *srt, const kvm_srt_conn_t *cn)
{
    if (srt->hooks.close != NULL) {
        srt->hooks.close(srt->hooks.ctx, cn->id, (kvm_srt_conn_kind_t)cn->kind);
    }
}

// ---- remote_protocol hooks -----------------------------------------------------------------

static kvm_rp_err_t h_hid_sink(void *ctx, uint64_t session_id, uint32_t lease_epoch,
                               const kvm_hid_intent_t *intent)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.hid_intent == NULL) {
        return KVM_RP_ERR_CONTROL_DENIED;
    }
    return kvm_rp_err_from_hid(
        srt->hooks.hid_intent(srt->hooks.ctx, session_id, lease_epoch, intent));
}

static kvm_rp_err_t h_control_request(void *ctx, uint64_t session_id)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.control_request == NULL) {
        return KVM_RP_ERR_CONTROL_DENIED;
    }
    return kvm_rp_err_from_hid(srt->hooks.control_request(srt->hooks.ctx, session_id));
}

static kvm_rp_err_t h_control_grant(void *ctx, uint64_t session_id, uint32_t *lease_epoch)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.control_grant == NULL) {
        return KVM_RP_ERR_CONTROL_DENIED;
    }
    return kvm_rp_err_from_hid(srt->hooks.control_grant(srt->hooks.ctx, session_id, lease_epoch));
}

static void h_control_release(void *ctx, uint64_t session_id, kvm_release_reason_t reason,
                              uint32_t *new_epoch)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.control_release != NULL) {
        srt->hooks.control_release(srt->hooks.ctx, session_id, reason, new_epoch);
    }
    srt->owner_kind = OWNER_NONE;
    srt->owner_session = 0u;
    if (new_epoch != NULL) {
        srt->owner_epoch = *new_epoch;
    }
}

static void h_heartbeat(void *ctx, uint64_t session_id)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.heartbeat != NULL) {
        srt->hooks.heartbeat(srt->hooks.ctx, session_id);
    }
}

static kvm_rp_approval_t h_approval_begin(void *ctx, uint64_t session_id, const char *client_name,
                                          const char *reason)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;

    // FR-733. `auto` grants without a prompt; `trusted` grants only for a register entry the user
    // marked trusted; `always` -- the default -- always prompts. The policy can relax the prompt,
    // never the lease, the epoch or the local override (ADR-023, SEC-107).
    if (srt->cfg.control_approval == KVM_CONTROL_APPROVAL_AUTO) {
        srt->c.approvals_auto++;
        return KVM_RP_APPROVAL_GRANTED;
    }
    if (srt->cfg.control_approval == KVM_CONTROL_APPROVAL_TRUSTED) {
        const int idx = client_index_of(srt, session_id);
        if (idx >= 0 && srt->hooks.client_trusted != NULL &&
            srt->hooks.client_trusted(srt->hooks.ctx, idx)) {
            srt->c.approvals_trusted++;
            return KVM_RP_APPROVAL_GRANTED;
        }
    }
    if (srt->hooks.approval_begin == NULL) {
        return KVM_RP_APPROVAL_DENIED;
    }
    srt->c.approvals_prompted++;
    const kvm_srt_approval_t a =
        srt->hooks.approval_begin(srt->hooks.ctx, session_id, client_name, reason);
    return (a == KVM_SRT_APPROVAL_PENDING) ? KVM_RP_APPROVAL_PENDING : KVM_RP_APPROVAL_DENIED;
}

static void h_approval_cancel(void *ctx, uint64_t session_id)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.approval_cancel != NULL) {
        srt->hooks.approval_cancel(srt->hooks.ctx, session_id);
    }
}

static void h_send(void *ctx, uint64_t session_id, const char *frame, size_t len)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.send != NULL) {
        srt->hooks.send(srt->hooks.ctx, session_id, KVM_SRT_CONN_CONTROL, frame, len);
    }
}

static void h_close_session(void *ctx, uint64_t session_id, kvm_rp_err_t reason)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    (void)reason;
    if (srt->hooks.close != NULL) {
        srt->hooks.close(srt->hooks.ctx, session_id, KVM_SRT_CONN_CONTROL);
    }
}

static void h_status_fill(void *ctx, kvm_rp_status_t *out)
{
    kvm_srt_t *srt = (kvm_srt_t *)ctx;
    if (srt->hooks.status_fill != NULL) {
        srt->hooks.status_fill(srt->hooks.ctx, out);
    }
}

// ---- lifecycle -----------------------------------------------------------------------------

kvm_pair_err_t kvm_srt_init(kvm_srt_t *srt, const kvm_srt_config_t *cfg,
                            const kvm_srt_hooks_t *hooks)
{
    if (srt == NULL || cfg == NULL || hooks == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    memset(srt, 0, sizeof(*srt));
    srt->cfg = *cfg;
    srt->hooks = *hooks;
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        conn_free(&srt->conn[i]);
    }
    srt->owner_kind = OWNER_NONE;
    srt->host_usb = KVM_HOST_USB_UNINITIALIZED;

    const kvm_rp_hooks_t rph = {
        .hid_sink = h_hid_sink,
        .control_request = h_control_request,
        .control_grant = h_control_grant,
        .control_release = h_control_release,
        .heartbeat = h_heartbeat,
        .approval_begin = h_approval_begin,
        .approval_cancel = h_approval_cancel,
        .send = h_send,
        .close_session = h_close_session,
        .status_fill = h_status_fill,
        // SEC-103: `pair.*` belongs to `/v1/pair` alone. A NULL hook is the documented
        // configuration in which remote_protocol answers every `pair.*` frame on the control
        // endpoint with ERR_PAIRING_EXPIRED.
        .pair_frame = NULL,
        .ctx = srt,
    };
    kvm_rp_init(&srt->rp, cfg->rp, &rph);

    kvm_pair_config_t pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.require_accept = cfg->pair_require_accept;
    memcpy(pcfg.cert_sha256, cfg->cert_sha256, sizeof(pcfg.cert_sha256));
    memcpy(pcfg.device_name, cfg->device_name, sizeof(pcfg.device_name) - 1u);

    const kvm_pair_err_t err =
        kvm_pair_session_init(&srt->pair, &pcfg, &cfg->platform, &cfg->store);
    srt->pair_ready = (err == KVM_PAIR_OK);
    // Runs last so the sanitizing in kvm_srt_set_identity() applies to both copies of the name.
    kvm_srt_set_identity(srt, cfg->cert_sha256, cfg->device_name);
    return err;
}

void kvm_srt_shutdown(kvm_srt_t *srt, uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    kvm_srt_drop_all(srt, KVM_SRT_LOSS_SHUTDOWN, true, now_us);
    if (srt->pair_ready) {
        kvm_pair_session_free(&srt->pair);
        srt->pair_ready = false;
    }
}

// ---- authoritative state in -----------------------------------------------------------------

// SEC-106: a window may only live while Remote is on and Wi-Fi is up. Losing either closes it on
// the spot and wipes the code; it is not left to expire.
static void enforce_pair_preconditions(kvm_srt_t *srt);

// Hand the newest pairing event to the UI. The core keeps only one, so this runs after every call
// that can post one.
static void drain_pair_events(kvm_srt_t *srt);

void kvm_srt_set_host(kvm_srt_t *srt, kvm_host_usb_t usb, bool maintenance, bool wifi_connected,
                      uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    srt->host_usb = usb;
    srt->maintenance = maintenance;
    srt->wifi_connected = wifi_connected;
    kvm_rp_set_host(&srt->rp, usb, maintenance, wifi_connected, now_us);
    enforce_pair_preconditions(srt);
}

void kvm_srt_set_remote_enabled(kvm_srt_t *srt, bool enabled, uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    const bool was = srt->remote_enabled;
    srt->remote_enabled = enabled;
    if (was && !enabled) {
        kvm_srt_drop_all(srt, KVM_SRT_LOSS_REMOTE_DISABLED, true, now_us);
    }
    enforce_pair_preconditions(srt);
}

void kvm_srt_set_owner(kvm_srt_t *srt, kvm_owner_kind_t kind, uint64_t session_id, uint32_t epoch,
                       uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    srt->owner_kind = kind;
    srt->owner_session = session_id;
    srt->owner_epoch = epoch;
    kvm_rp_set_owner(&srt->rp, kind, session_id, epoch, now_us);
}

void kvm_srt_set_policy(kvm_srt_t *srt, bool pair_require_accept,
                        kvm_control_approval_t control_approval)
{
    if (srt == NULL) {
        return;
    }
    srt->cfg.pair_require_accept = pair_require_accept;
    srt->cfg.control_approval = control_approval;
    if (srt->pair_ready) {
        kvm_pair_set_require_accept(&srt->pair, pair_require_accept);
    }
}

void kvm_srt_set_identity(kvm_srt_t *srt, const uint8_t cert_sha256[KVM_PAIR_SHA256_LEN],
                          const char *device_name)
{
    if (srt == NULL) {
        return;
    }
    if (cert_sha256 != NULL) {
        memcpy(srt->cfg.cert_sha256, cert_sha256, KVM_PAIR_SHA256_LEN);
        if (srt->pair_ready) {
            memcpy(srt->pair.cfg.cert_sha256, cert_sha256, KVM_PAIR_SHA256_LEN);
        }
    }
    if (device_name != NULL) {
        memset(srt->cfg.device_name, 0, sizeof(srt->cfg.device_name));
        size_t i = 0;
        // The name reaches the CLIENT in `pair.round1` / `pair.accepted` and the mDNS TXT record.
        // Control characters are dropped here, so neither a log line nor a screen can be steered
        // by a name that came from a settings import.
        for (; device_name[i] != '\0' && i + 1u < sizeof(srt->cfg.device_name); i++) {
            const unsigned char c = (unsigned char)device_name[i];
            srt->cfg.device_name[i] = (c < 0x20u || c == 0x7Fu) ? '?' : device_name[i];
        }
        srt->cfg.device_name[i] = '\0';
        if (srt->pair_ready) {
            memcpy(srt->pair.cfg.device_name, srt->cfg.device_name,
                   sizeof(srt->pair.cfg.device_name));
        }
    }
}

void kvm_srt_notify_revoked(kvm_srt_t *srt, uint64_t session_id, kvm_rp_revoke_reason_t reason,
                            uint32_t new_epoch, uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    srt->owner_kind = OWNER_NONE;
    srt->owner_session = 0u;
    srt->owner_epoch = new_epoch;
    kvm_rp_notify_revoked(&srt->rp, session_id, reason, new_epoch, now_us);
}

// ---- pairing frame handling -----------------------------------------------------------------

static void pair_send_err(kvm_srt_t *srt, kvm_srt_conn_t *cn, kvm_rp_err_t err, bool close_after)
{
    srt->c.pair_frames_rejected++;
    const size_t n =
        kvm_srt_error_frame(err, cn->out_seq + 1u, cn->label, srt->pout, sizeof(srt->pout));
    if (n > 0u) {
        cn->out_seq++;
        srt->c.pair_frames_out++;
        if (srt->hooks.send != NULL) {
            srt->hooks.send(srt->hooks.ctx, cn->id, KVM_SRT_CONN_PAIRING, srt->pout, n);
        }
    } else {
        srt->c.pair_out_dropped++;
    }
    if (close_after) {
        ask_close(srt, cn);
    }
}

// A malformed frame costs one unit of the bad-frame budget of 03 §6.5. Running out closes the
// connection: a peer that cannot produce a valid frame is not negotiated with.
static void pair_bad_frame(kvm_srt_t *srt, kvm_srt_conn_t *cn, kvm_rp_err_t err)
{
    cn->bad_frames++;
    const uint32_t limit =
        (srt->rp.cfg.bad_frame_limit != 0u) ? srt->rp.cfg.bad_frame_limit : 8u;
    pair_send_err(srt, cn, err, cn->bad_frames >= limit);
}

static void pair_emit(kvm_srt_t *srt, kvm_srt_conn_t *cn, const kvm_pair_out_t *out,
                      kvm_pair_err_t err)
{
    // A `pair.rejected` is a message, not an error payload (ADR-029 §4 row 8'), so a kind that
    // carries a frame is always sent as that frame -- even when the result code is not OK.
    if (out->kind == KVM_PAIR_OUT_NONE || out->kind == KVM_PAIR_OUT_ERROR) {
        if (out->kind == KVM_PAIR_OUT_ERROR || err != KVM_PAIR_OK) {
            pair_send_err(srt, cn, rp_err_of_pair(err), out->close_after);
        }
        return;
    }
    for (unsigned part = 0; part < 2u; part++) {
        const size_t n = kvm_srt_pair_frame(out, part, cn->out_seq + 1u, cn->label,
                                            srt->cfg.device_name, srt->pout, sizeof(srt->pout));
        if (n == 0u) {
            // Part 0 always exists for every kind that reaches here, so a zero length means the
            // frame did not fit. It is dropped whole: a half-written pairing message would be a
            // protocol error on the CLIENT, not a recoverable one.
            if (part == 0u) {
                srt->c.pair_out_dropped++;
            }
            break;
        }
        cn->out_seq++;
        srt->c.pair_frames_out++;
        if (srt->hooks.send != NULL) {
            srt->hooks.send(srt->hooks.ctx, cn->id, KVM_SRT_CONN_PAIRING, srt->pout, n);
        }
    }
    if (out->close_after) {
        ask_close(srt, cn);
    }
}

// Route a message the pairing core produced outside a frame call (a tick deadline, a LOCAL
// Accept/Reject/Cancel) to the pairing connection, if one is open.
static void pair_emit_unsolicited(kvm_srt_t *srt, const kvm_pair_out_t *out, kvm_pair_err_t err)
{
    if (out->kind == KVM_PAIR_OUT_NONE) {
        return;
    }
    kvm_srt_conn_t *cn = pairing_conn(srt);
    if (cn == NULL) {
        return;
    }
    pair_emit(srt, cn, out, err);
}

static void pair_frame(kvm_srt_t *srt, kvm_srt_conn_t *cn, const char *frame, size_t len)
{
    srt->c.pair_frames_in++;
    if (!srt->pair_ready) {
        pair_send_err(srt, cn, KVM_RP_ERR_PAIRING_EXPIRED, true);
        return;
    }

    kvm_srt_pair_env_t env;
    kvm_rp_err_t e = kvm_srt_pair_parse_env(&srt->pdoc, frame, len, &env);
    if (e != KVM_RP_OK) {
        pair_bad_frame(srt, cn, e);
        return;
    }
    // 03 §6.2: `seq` is monotonic per session. WebSocket delivers in order, but an application can
    // resend an old frame after a reconnect, which is forbidden.
    if (cn->have_seq && env.seq <= cn->last_seq) {
        pair_send_err(srt, cn, KVM_RP_ERR_REPLAY, false);
        return;
    }
    // The label is assigned by the DEVICE in `pair.round1`. Before that the CLIENT has none, so an
    // absent `session_id` is accepted; a value that is not ours is not.
    if (env.have_session_id && cn->label[0] != '\0' &&
        strcmp(env.session_id, cn->label) != 0) {
        pair_send_err(srt, cn, KVM_RP_ERR_SESSION, false);
        return;
    }
    // SEC-103: outside an open window the endpoint answers one code and nothing else.
    if (!kvm_pair_endpoint_open(&srt->pair)) {
        pair_send_err(srt, cn, KVM_RP_ERR_PAIRING_EXPIRED, true);
        return;
    }
    if (env.payload < 0) {
        pair_bad_frame(srt, cn, KVM_RP_ERR_BAD_FRAME);
        return;
    }
    cn->last_seq = env.seq;
    cn->have_seq = true;

    kvm_pair_out_t out;
    memset(&out, 0, sizeof(out));
    kvm_pair_err_t pe;

    switch (env.msg) {
    case KVM_SRT_PAIR_MSG_HELLO: {
        // One run per connection: a second `pair.hello` on the same socket would be a new attempt
        // without a new TLS handshake, so the certificate hash it binds to is no longer observed.
        if (cn->pair_hello_seen) {
            pair_bad_frame(srt, cn, KVM_RP_ERR_BAD_FRAME);
            return;
        }
        kvm_srt_pair_hello_t h;
        e = kvm_srt_pair_parse_hello(&srt->pdoc, env.payload, &h);
        if (e != KVM_RP_OK) {
            memset(&h, 0, sizeof(h));
            pair_bad_frame(srt, cn, e);
            return;
        }
        pe = kvm_pair_handle_hello(&srt->pair, &h.hello, &out);
        memset(&h, 0, sizeof(h));
        if (pe == KVM_PAIR_OK) {
            cn->pair_hello_seen = true;
        }
        break;
    }
    case KVM_SRT_PAIR_MSG_ROUND1: {
        uint8_t blob[KVM_PAIR_ROUND1_MAX];
        size_t n = 0;
        e = kvm_srt_pair_parse_blob(&srt->pdoc, env.payload, "blob", blob, sizeof(blob), 0u, &n);
        if (e != KVM_RP_OK) {
            pair_bad_frame(srt, cn, e);
            return;
        }
        pe = kvm_pair_handle_round1(&srt->pair, blob, n, &out);
        break;
    }
    case KVM_SRT_PAIR_MSG_ROUND2: {
        uint8_t blob[KVM_PAIR_ROUND2_MAX];
        size_t n = 0;
        e = kvm_srt_pair_parse_blob(&srt->pdoc, env.payload, "blob", blob, sizeof(blob), 0u, &n);
        if (e != KVM_RP_OK) {
            pair_bad_frame(srt, cn, e);
            return;
        }
        pe = kvm_pair_handle_round2(&srt->pair, blob, n, &out);
        break;
    }
    case KVM_SRT_PAIR_MSG_CONFIRM: {
        uint8_t tag[KVM_PAIR_CONFIRM_LEN];
        size_t n = 0;
        e = kvm_srt_pair_parse_blob(&srt->pdoc, env.payload, "mac", tag, sizeof(tag),
                                    KVM_PAIR_CONFIRM_LEN, &n);
        if (e != KVM_RP_OK) {
            pair_bad_frame(srt, cn, e);
            return;
        }
        pe = kvm_pair_handle_confirm(&srt->pair, tag, n, &out);
        break;
    }
    case KVM_SRT_PAIR_MSG_COUNT:
    default:
        pair_bad_frame(srt, cn, KVM_RP_ERR_BAD_FRAME);
        return;
    }

    pair_emit(srt, cn, &out, pe);
    memset(&out, 0, sizeof(out));
}

// ---- admission -----------------------------------------------------------------------------

static void auth_fail(kvm_srt_t *srt, uint64_t now_us)
{
    const uint32_t max = (srt->cfg.auth_fail_max != 0u) ? srt->cfg.auth_fail_max
                                                        : KVM_SRT_AUTH_FAIL_MAX;
    srt->auth_fails++;
    if (srt->auth_fails >= max) {
        srt->auth_fails = 0u;
        const uint64_t cd = (srt->cfg.auth_cooldown_us != 0u) ? srt->cfg.auth_cooldown_us
                                                              : KVM_SRT_AUTH_COOLDOWN_US;
        srt->auth_cooldown_until_us = now_us + cd;
        srt->c.auth_cooldowns++;
    }
}

kvm_srt_admit_t kvm_srt_admit(kvm_srt_t *srt, kvm_srt_conn_kind_t kind, const char *authorization,
                              uint64_t now_us, kvm_srt_admission_t *out)
{
    if (srt == NULL || out == NULL) {
        return KVM_SRT_ADMIT_FORBIDDEN;
    }
    memset(out, 0, sizeof(*out));
    out->client_index = -1;
    srt->now_us = now_us;

    if (!srt->pair_ready) {
        // Fail closed: without a usable pairing core there is no register to verify against.
        srt->c.admit_forbidden++;
        return KVM_SRT_ADMIT_FORBIDDEN;
    }

    if (kind == KVM_SRT_CONN_PAIRING) {
        // SEC-103 / SEC-106. There is no token on this endpoint, so the window is the only gate.
        if (!srt->remote_enabled || !srt->wifi_connected ||
            !kvm_pair_endpoint_open(&srt->pair)) {
            srt->c.admit_forbidden++;
            return KVM_SRT_ADMIT_FORBIDDEN;
        }
        if (pairing_conn(srt) != NULL) {
            // One run at a time (FR-722). A second peer does not get to race the first.
            srt->c.admit_busy++;
            return KVM_SRT_ADMIT_BUSY;
        }
        out->kind = KVM_SRT_CONN_PAIRING;
        out->label_entropy = srt_rand32(srt);
        srt->c.admit_ok[KVM_SRT_CONN_PAIRING]++;
        return KVM_SRT_ADMIT_OK;
    }

    if (!srt->remote_enabled) {
        srt->c.admit_forbidden++;
        return KVM_SRT_ADMIT_FORBIDDEN;
    }
    // TECH-042: after a run of failures the endpoint stops answering for a while. The cooldown is
    // a refusal, never a sleep: this runs in the server task, which must not be blocked.
    if (now_us < srt->auth_cooldown_until_us) {
        srt->c.auth_refused_cold++;
        srt->c.admit_unauthorized++;
        return KVM_SRT_ADMIT_UNAUTHORIZED;
    }

    uint8_t token[KVM_PAIR_TOKEN_LEN];
    const kvm_srt_auth_err_t ae = kvm_srt_auth_parse_bearer(authorization, token);
    if (ae != KVM_SRT_AUTH_OK) {
        switch (ae) {
        case KVM_SRT_AUTH_ERR_ABSENT:
            srt->c.auth_absent++;
            break;
        case KVM_SRT_AUTH_ERR_SCHEME:
            srt->c.auth_bad_scheme++;
            break;
        case KVM_SRT_AUTH_ERR_ENCODING:
        default:
            srt->c.auth_bad_encoding++;
            break;
        }
        memset(token, 0, sizeof(token));
        auth_fail(srt, now_us);
        srt->c.admit_unauthorized++;
        return KVM_SRT_ADMIT_UNAUTHORIZED;
    }

    const int idx = kvm_pair_clients_verify_token(&srt->pair, token);
    memset(token, 0, sizeof(token));
    if (idx < 0) {
        srt->c.auth_unknown_token++;
        auth_fail(srt, now_us);
        srt->c.admit_unauthorized++;
        return KVM_SRT_ADMIT_UNAUTHORIZED;
    }
    if (control_count(srt) >= KVM_SRT_MAX_CONTROL) {
        srt->c.admit_busy++;
        return KVM_SRT_ADMIT_BUSY;
    }

    srt->auth_fails = 0u;
    out->kind = KVM_SRT_CONN_CONTROL;
    // Every paired client may ask for the lease; the single-operator rule of 03 §6.5 is enforced
    // by the arbiter, not by handing out roles here. Read-only observers are FR-404, a later role.
    out->operator_role = true;
    out->client_index = (int16_t)idx;
    out->label_entropy = srt_rand32(srt);
    srt->c.admit_ok[KVM_SRT_CONN_CONTROL]++;
    return KVM_SRT_ADMIT_OK;
}

int kvm_srt_conn_open(kvm_srt_t *srt, uint64_t conn_id, const kvm_srt_admission_t *adm,
                      uint64_t now_us)
{
    if (srt == NULL || adm == NULL || conn_id == 0u || conn_of(srt, conn_id) != NULL) {
        if (srt != NULL) {
            srt->c.conns_rejected++;
        }
        return -1;
    }
    srt->now_us = now_us;

    if (adm->kind == KVM_SRT_CONN_CONTROL) {
        const kvm_rp_session_info_t info = {
            .session_id = conn_id,
            .authenticated = true, // the token was verified in kvm_srt_admit()
            .operator_role = adm->operator_role,
            .pairing_only = false,
            .label_entropy = adm->label_entropy,
        };
        if (kvm_rp_session_open(&srt->rp, &info, now_us) < 0) {
            srt->c.conns_rejected++;
            return -1;
        }
    } else if (pairing_conn(srt) != NULL) {
        srt->c.conns_rejected++;
        return -1;
    }

    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (srt->conn[i].state == 0u) {
            kvm_srt_conn_t *cn = &srt->conn[i];
            conn_free(cn);
            cn->state = 1u;
            cn->kind = (uint8_t)adm->kind;
            cn->id = conn_id;
            cn->client_index = adm->client_index;
            cn->opened_us = now_us;
            if (adm->kind == KVM_SRT_CONN_PAIRING) {
                kvm_srt_pair_label(cn->label, adm->label_entropy);
            }
            srt->c.conns_opened[adm->kind]++;
            return i;
        }
    }
    // The table cannot be full here: it holds one more slot than remote_protocol has sessions, and
    // the pairing slot was checked above. Undo the session anyway rather than leaving a half-open
    // one behind.
    if (adm->kind == KVM_SRT_CONN_CONTROL) {
        kvm_rp_session_close(&srt->rp, conn_id, KVM_RP_REVOKE_SESSION_CLOSED, now_us);
    }
    srt->c.conns_rejected++;
    return -1;
}

void kvm_srt_conn_frame(kvm_srt_t *srt, uint64_t conn_id, const char *frame, size_t len,
                        uint64_t now_us)
{
    if (srt == NULL || frame == NULL) {
        return;
    }
    srt->now_us = now_us;
    kvm_srt_conn_t *cn = conn_of(srt, conn_id);
    if (cn == NULL) {
        return;
    }
    if (cn->kind == (uint8_t)KVM_SRT_CONN_CONTROL) {
        (void)kvm_rp_on_frame(&srt->rp, conn_id, frame, len, now_us);
    } else {
        pair_frame(srt, cn, frame, len);
        // Every pairing handler can post an event -- the Accept screen, a failed attempt, a
        // lockout warning -- and the core keeps only the newest one, so it has to be drained on
        // the same call and not left for the next tick (FR-725, SEC-104).
        drain_pair_events(srt);
    }
}

void kvm_srt_conn_close(kvm_srt_t *srt, uint64_t conn_id, kvm_srt_loss_t loss, uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    kvm_srt_conn_t *cn = conn_of(srt, conn_id);
    if (cn == NULL) {
        return;
    }
    const kvm_srt_conn_kind_t kind = (kvm_srt_conn_kind_t)cn->kind;
    if ((unsigned)loss < (unsigned)KVM_SRT_LOSS_COUNT) {
        srt->c.losses[loss]++;
    }
    srt->c.conns_closed[kind]++;
    conn_free(cn);

    if (kind == KVM_SRT_CONN_CONTROL) {
        // The revoke + RELEASE_ALL + epoch bump happens here when this session owned the HOST
        // (03 §4.2 INV-002, AGENTS.md prohibition 3).
        kvm_rp_session_close(&srt->rp, conn_id, kvm_srt_loss_revoke_reason(loss), now_us);
    }
}

void kvm_srt_drop_all(kvm_srt_t *srt, kvm_srt_loss_t loss, bool include_pairing, uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (srt->conn[i].state == 0u) {
            continue;
        }
        if (srt->conn[i].kind == (uint8_t)KVM_SRT_CONN_PAIRING && !include_pairing) {
            continue;
        }
        const uint64_t id = srt->conn[i].id;
        ask_close(srt, &srt->conn[i]);
        kvm_srt_conn_close(srt, id, loss, now_us);
    }
    // Belt and braces for the broad losses: if REMOTE still owns the HOST after every session is
    // gone, nobody is left to release what it pressed, so ask for a RELEASE_ALL outright. The
    // check on the owner keeps this from cutting into a local drag (LOCAL > REMOTE, ADR-011).
    if (srt->owner_kind == OWNER_REMOTE && srt->hooks.release_all != NULL) {
        srt->c.release_all_requests++;
        srt->hooks.release_all(srt->hooks.ctx, kvm_srt_loss_release_reason(loss));
        srt->owner_kind = OWNER_NONE;
        srt->owner_session = 0u;
    }
    if (include_pairing && srt->pair_ready) {
        kvm_pair_out_t out;
        memset(&out, 0, sizeof(out));
        (void)kvm_pair_local_cancel(&srt->pair, &out);
    }
}

// ---- tick ----------------------------------------------------------------------------------

static void drain_pair_events(kvm_srt_t *srt)
{
    kvm_pair_event_t ev;
    while (kvm_pair_take_event(&srt->pair, &ev)) {
        srt->c.pair_events++;
        if (srt->hooks.pair_event != NULL) {
            srt->hooks.pair_event(srt->hooks.ctx, &ev);
        }
    }
}

static void enforce_pair_preconditions(kvm_srt_t *srt)
{
    if (!srt->pair_ready) {
        return;
    }
    if (srt->remote_enabled && srt->wifi_connected) {
        return;
    }
    if (!kvm_pair_endpoint_open(&srt->pair)) {
        return;
    }
    kvm_pair_out_t out;
    memset(&out, 0, sizeof(out));
    const kvm_pair_err_t err = kvm_pair_local_cancel(&srt->pair, &out);
    pair_emit_unsolicited(srt, &out, err);
    drain_pair_events(srt);
}

void kvm_srt_tick(kvm_srt_t *srt, uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    kvm_rp_poll(&srt->rp, now_us);

    if (!srt->pair_ready) {
        return;
    }
    kvm_pair_out_t out;
    memset(&out, 0, sizeof(out));
    const kvm_pair_err_t err = kvm_pair_tick(&srt->pair, &out);
    pair_emit_unsolicited(srt, &out, err);
    drain_pair_events(srt);

    // Once the window is gone the pairing socket has nothing left to carry. Ask the transport to
    // drop it so an idle unverified-TLS connection cannot sit there until it times out (SEC-103).
    if (!kvm_pair_endpoint_open(&srt->pair)) {
        kvm_srt_conn_t *cn = pairing_conn(srt);
        if (cn != NULL) {
            ask_close(srt, cn);
        }
    }
}

void kvm_srt_approval_result(kvm_srt_t *srt, uint64_t session_id, bool granted, uint64_t now_us)
{
    if (srt == NULL) {
        return;
    }
    srt->now_us = now_us;
    kvm_rp_approval_result(&srt->rp, session_id, granted, now_us);
}

int kvm_srt_slot_of(const kvm_srt_t *srt, uint64_t conn_id)
{
    if (srt == NULL || conn_id == 0u) {
        return -1;
    }
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (srt->conn[i].state != 0u && srt->conn[i].id == conn_id) {
            return i;
        }
    }
    return -1;
}

kvm_srt_conn_kind_t kvm_srt_conn_kind(const kvm_srt_t *srt, uint64_t conn_id)
{
    const int slot = kvm_srt_slot_of(srt, conn_id);
    if (slot < 0) {
        return KVM_SRT_CONN_KINDS;
    }
    return (kvm_srt_conn_kind_t)srt->conn[slot].kind;
}

// ---- pairing, driven by the local UI --------------------------------------------------------

kvm_pair_err_t kvm_srt_pair_open(kvm_srt_t *srt, uint64_t now_us)
{
    if (srt == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    srt->now_us = now_us;
    if (!srt->pair_ready) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    const kvm_pair_preconditions_t pre = {
        .remote_enabled = srt->remote_enabled,
        .wifi_connected = srt->wifi_connected,
    };
    const kvm_pair_err_t err = kvm_pair_window_open(&srt->pair, &pre);
    drain_pair_events(srt);
    return err;
}

static kvm_pair_err_t pair_local(kvm_srt_t *srt, uint64_t now_us,
                                 kvm_pair_err_t (*fn)(kvm_pair_session_t *, kvm_pair_out_t *))
{
    if (srt == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    srt->now_us = now_us;
    if (!srt->pair_ready) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    kvm_pair_out_t out;
    memset(&out, 0, sizeof(out));
    const kvm_pair_err_t err = fn(&srt->pair, &out);
    pair_emit_unsolicited(srt, &out, err);
    drain_pair_events(srt);
    memset(&out, 0, sizeof(out));
    return err;
}

kvm_pair_err_t kvm_srt_pair_accept(kvm_srt_t *srt, uint64_t now_us)
{
    return pair_local(srt, now_us, kvm_pair_local_accept);
}

kvm_pair_err_t kvm_srt_pair_reject(kvm_srt_t *srt, uint64_t now_us)
{
    return pair_local(srt, now_us, kvm_pair_local_reject);
}

kvm_pair_err_t kvm_srt_pair_cancel(kvm_srt_t *srt, uint64_t now_us)
{
    return pair_local(srt, now_us, kvm_pair_local_cancel);
}

const char *kvm_srt_pair_code(const kvm_srt_t *srt)
{
    if (srt == NULL || !srt->pair_ready) {
        return NULL;
    }
    return kvm_pair_window_code(&srt->pair);
}

uint32_t kvm_srt_pair_code_remaining_ms(const kvm_srt_t *srt)
{
    return (srt != NULL && srt->pair_ready) ? kvm_pair_code_remaining_ms(&srt->pair) : 0u;
}

uint32_t kvm_srt_pair_window_remaining_ms(const kvm_srt_t *srt)
{
    return (srt != NULL && srt->pair_ready) ? kvm_pair_window_remaining_ms(&srt->pair) : 0u;
}

bool kvm_srt_pair_endpoint_open(const kvm_srt_t *srt)
{
    return (srt != NULL && srt->pair_ready) ? kvm_pair_endpoint_open(&srt->pair) : false;
}

const kvm_pair_counters_t *kvm_srt_pair_counters(const kvm_srt_t *srt)
{
    return (srt != NULL) ? &srt->pair.counters : NULL;
}

// ---- paired clients ------------------------------------------------------------------------

uint8_t kvm_srt_clients_count(const kvm_srt_t *srt)
{
    return (srt != NULL && srt->pair_ready) ? kvm_pair_clients_count(&srt->pair) : 0u;
}

bool kvm_srt_clients_get(const kvm_srt_t *srt, uint8_t index, kvm_pair_record_t *out)
{
    if (srt == NULL || !srt->pair_ready) {
        return false;
    }
    return kvm_pair_clients_get(&srt->pair, index, out);
}

bool kvm_srt_clients_remove(kvm_srt_t *srt, uint8_t index, uint64_t now_us)
{
    if (srt == NULL || !srt->pair_ready) {
        return false;
    }
    srt->now_us = now_us;
    if (!kvm_pair_clients_remove(&srt->pair, index)) {
        return false;
    }

    // FR-726: a revoke takes effect at once. The register compacts on removal, so the live
    // connections above `index` now authenticate as a different slot and their cached index has to
    // follow; without that, `trusted` would be read off the wrong entry after a removal.
    for (int i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        kvm_srt_conn_t *cn = &srt->conn[i];
        if (cn->state == 0u || cn->kind != (uint8_t)KVM_SRT_CONN_CONTROL) {
            continue;
        }
        if (cn->client_index == (int16_t)index) {
            const uint64_t id = cn->id;
            srt->c.clients_revoked++;
            ask_close(srt, cn);
            kvm_srt_conn_close(srt, id, KVM_SRT_LOSS_UNPAIRED, now_us);
        } else if (cn->client_index > (int16_t)index) {
            cn->client_index--;
        }
    }
    return true;
}

uint8_t kvm_srt_clients_remove_all(kvm_srt_t *srt, uint64_t now_us)
{
    if (srt == NULL || !srt->pair_ready) {
        return 0u;
    }
    srt->now_us = now_us;
    const uint8_t gone = kvm_pair_clients_remove_all(&srt->pair);
    if (gone > 0u) {
        srt->c.clients_revoked += gone;
    }
    kvm_srt_drop_all(srt, KVM_SRT_LOSS_UNPAIRED, false, now_us);
    return gone;
}

// ---- diagnostics ---------------------------------------------------------------------------

const kvm_srt_counters_t *kvm_srt_counters(const kvm_srt_t *srt)
{
    return (srt != NULL) ? &srt->c : NULL;
}

const kvm_rp_counters_t *kvm_srt_protocol_counters(const kvm_srt_t *srt)
{
    return (srt != NULL) ? kvm_rp_counters(&srt->rp) : NULL;
}
