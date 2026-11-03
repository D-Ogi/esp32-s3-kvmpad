// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// RemoteProtocolValidator and the session/command layer of CLIENT protocol v1
// (spec 03 §6.2–§6.6, §7; TECH-043..046; ADR-011, ADR-019, ADR-022, ADR-023).
//
// Order of checks for every frame, fail closed at the first failure:
//   session known -> authenticated -> size -> JSON -> envelope members -> v -> type -> seq/replay
//   -> session_id -> per-type authorization -> rate -> payload ranges -> intent -> sink.
// No HID intent can leave this file before every one of those steps has passed.
#include "remote_protocol.h"

#include <string.h>

#include "hid_keymap.h"
#include "remote_wire.h"

#define RP_SEQ_MAX 9007199254740991ll // 2^53-1 (03 §6.2: safe as a textual JSON number)
#define RP_DELTA_MAX 4096             // 03 §6.5: dx/dy per event; ADR-030 applies it to the wheel
#define RP_TYPE_MAX 24
#define RP_KIND_MAX 24
#define RP_CMD_MAX 24
#define RP_NAME_RAW_MAX 96

// ---- names and mappings --------------------------------------------------------------------

const char *kvm_rp_err_name(kvm_rp_err_t err)
{
    switch (err) {
    case KVM_RP_OK:
        return "OK";
    case KVM_RP_ERR_VERSION:
        return "ERR_VERSION";
    case KVM_RP_ERR_BAD_FRAME:
        return "ERR_BAD_FRAME";
    case KVM_RP_ERR_RANGE:
        return "ERR_RANGE";
    case KVM_RP_ERR_USAGE:
        return "ERR_USAGE";
    case KVM_RP_ERR_REPLAY:
        return "ERR_REPLAY";
    case KVM_RP_ERR_STALE_EPOCH:
        return "ERR_STALE_EPOCH";
    case KVM_RP_ERR_SESSION:
        return "ERR_SESSION";
    case KVM_RP_ERR_HOST_OFFLINE:
        return "ERR_HOST_OFFLINE";
    case KVM_RP_ERR_USB_SUSPENDED:
        return "ERR_USB_SUSPENDED";
    case KVM_RP_ERR_CLIENT_STALE:
        return "ERR_CLIENT_STALE";
    case KVM_RP_ERR_CONTROL_DENIED:
        return "ERR_CONTROL_DENIED";
    case KVM_RP_ERR_PAIRING_EXPIRED:
        return "ERR_PAIRING_EXPIRED";
    case KVM_RP_ERR_OTA_IN_PROGRESS:
        return "ERR_OTA_IN_PROGRESS";
    case KVM_RP_ERR_HID_QUEUE_OVERFLOW:
        return "ERR_HID_QUEUE_OVERFLOW";
    case KVM_RP_ERR_AUTH:
        return "ERR_AUTH";
    case KVM_RP_ERR_RATE_LIMIT:
        return "ERR_RATE_LIMIT";
    default:
        return "ERR_BAD_FRAME";
    }
}

bool kvm_rp_err_retryable(kvm_rp_err_t err)
{
    switch (err) {
    case KVM_RP_ERR_HOST_OFFLINE:
    case KVM_RP_ERR_USB_SUSPENDED:
    case KVM_RP_ERR_CLIENT_STALE:
    case KVM_RP_ERR_CONTROL_DENIED:
    case KVM_RP_ERR_OTA_IN_PROGRESS:
    case KVM_RP_ERR_HID_QUEUE_OVERFLOW:
    case KVM_RP_ERR_RATE_LIMIT:
        return true;
    default:
        return false;
    }
}

kvm_rp_err_t kvm_rp_err_from_hid(kvm_hid_err_t err)
{
    switch (err) {
    case KVM_HID_OK:
        return KVM_RP_OK;
    case KVM_HID_ERR_HOST_OFFLINE:
        return KVM_RP_ERR_HOST_OFFLINE;
    case KVM_HID_ERR_USB_SUSPENDED:
        return KVM_RP_ERR_USB_SUSPENDED;
    case KVM_HID_ERR_STALE_EPOCH:
        return KVM_RP_ERR_STALE_EPOCH;
    case KVM_HID_ERR_SESSION:
        return KVM_RP_ERR_SESSION;
    case KVM_HID_ERR_CLIENT_STALE:
        return KVM_RP_ERR_CLIENT_STALE;
    case KVM_HID_ERR_CONTROL_DENIED:
        return KVM_RP_ERR_CONTROL_DENIED;
    case KVM_HID_ERR_USAGE:
        return KVM_RP_ERR_USAGE;
    case KVM_HID_ERR_RANGE:
        return KVM_RP_ERR_RANGE;
    case KVM_HID_ERR_HID_QUEUE_OVERFLOW:
        return KVM_RP_ERR_HID_QUEUE_OVERFLOW;
    case KVM_HID_ERR_OTA_IN_PROGRESS:
        return KVM_RP_ERR_OTA_IN_PROGRESS;
    default:
        return KVM_RP_ERR_BAD_FRAME;
    }
}

const char *kvm_rp_revoke_reason_name(kvm_rp_revoke_reason_t reason)
{
    switch (reason) {
    case KVM_RP_REVOKE_CLIENT_RELEASE:
        return "client_release";
    case KVM_RP_REVOKE_CLIENT_STALE:
        return "client_stale";
    case KVM_RP_REVOKE_SESSION_CLOSED:
        return "session_closed";
    case KVM_RP_REVOKE_LOCAL_OVERRIDE:
        return "local_override";
    case KVM_RP_REVOKE_HOST_OFFLINE:
        return "host_offline";
    case KVM_RP_REVOKE_MAINTENANCE:
        return "maintenance";
    case KVM_RP_REVOKE_RATE_FLOOD:
        return "rate_flood";
    case KVM_RP_REVOKE_PROTOCOL_ABUSE:
        return "protocol_abuse";
    case KVM_RP_REVOKE_UNPAIRED:
        return "unpaired";
    default:
        return "session_closed";
    }
}

kvm_release_reason_t kvm_rp_release_reason(kvm_rp_revoke_reason_t reason)
{
    switch (reason) {
    case KVM_RP_REVOKE_CLIENT_RELEASE:
        return KVM_RA_REMOTE_RELEASE;
    case KVM_RP_REVOKE_CLIENT_STALE:
        return KVM_RA_LEASE_TIMEOUT;
    case KVM_RP_REVOKE_LOCAL_OVERRIDE:
        return KVM_RA_LOCAL_OVERRIDE;
    case KVM_RP_REVOKE_HOST_OFFLINE:
        return KVM_RA_USB_LOSS;
    case KVM_RP_REVOKE_MAINTENANCE:
        return KVM_RA_MAINTENANCE;
    case KVM_RP_REVOKE_SESSION_CLOSED:
    case KVM_RP_REVOKE_RATE_FLOOD:
    case KVM_RP_REVOKE_PROTOCOL_ABUSE:
    case KVM_RP_REVOKE_UNPAIRED:
    default:
        return KVM_RA_REMOTE_DISCONNECT;
    }
}

// ---- small helpers -------------------------------------------------------------------------

static kvm_rp_session_t *find(kvm_rp_t *rp, uint64_t id)
{
    if (id == 0u) {
        return NULL;
    }
    for (int i = 0; i < KVM_RP_MAX_SESSIONS; i++) {
        if (rp->s[i].state != KVM_RP_SESS_FREE && rp->s[i].id == id) {
            return &rp->s[i];
        }
    }
    return NULL;
}

int kvm_rp_slot_of(const kvm_rp_t *rp, uint64_t session_id)
{
    if (session_id == 0u) {
        return -1;
    }
    for (int i = 0; i < KVM_RP_MAX_SESSIONS; i++) {
        if (rp->s[i].state != KVM_RP_SESS_FREE && rp->s[i].id == session_id) {
            return i;
        }
    }
    return -1;
}

const char *kvm_rp_session_label(const kvm_rp_t *rp, uint64_t session_id)
{
    int slot = kvm_rp_slot_of(rp, session_id);
    return (slot < 0) ? "" : rp->s[slot].label;
}

const kvm_rp_counters_t *kvm_rp_counters(const kvm_rp_t *rp)
{
    return &rp->c;
}

static bool is_owner(const kvm_rp_t *rp, const kvm_rp_session_t *s)
{
    return rp->owner_kind == OWNER_REMOTE && rp->owner_session == s->id;
}

static void shadow_clear(kvm_rp_session_t *s)
{
    memset(s->keys_down, 0, sizeof(s->keys_down));
    s->buttons_down = 0;
    s->pend_dx = 0;
    s->pend_dy = 0;
    s->pend_move = false;
}

static bool key_shadow(const kvm_rp_session_t *s, uint8_t usage)
{
    return ((s->keys_down[usage >> 3] >> (usage & 7u)) & 1u) != 0u;
}

static void key_shadow_set(kvm_rp_session_t *s, uint8_t usage, bool on)
{
    uint8_t mask = (uint8_t)(1u << (usage & 7u));
    if (on) {
        s->keys_down[usage >> 3] |= mask;
    } else {
        s->keys_down[usage >> 3] = (uint8_t)(s->keys_down[usage >> 3] & (uint8_t)~mask);
    }
}

static kvm_rp_err_t count_err(kvm_rp_t *rp, kvm_rp_err_t code)
{
    if ((unsigned)code < (unsigned)KVM_RP_ERR_COUNT) {
        rp->c.errors[code]++;
    }
    rp->c.frames_rejected++;
    return code;
}

/** Report an error to the client and count it. */
static kvm_rp_err_t reject(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_err_t code, uint64_t reply_to)
{
    (void)count_err(rp, code);
    kvm_rp_send_error(rp, s, code, reply_to);
    return code;
}

static void close_now(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_err_t why);

/** 03 §6.5: "count + close on abuse". */
static kvm_rp_err_t reject_abuse(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_err_t code,
                                 uint64_t reply_to)
{
    kvm_rp_err_t r = reject(rp, s, code, reply_to);
    s->bad_frames++;
    if (s->bad_frames >= rp->cfg.bad_frame_limit) {
        close_now(rp, s, code);
    }
    return r;
}

// ---- rolling input rate window (03 §6.5) ---------------------------------------------------

static void rate_advance(kvm_rp_session_t *s, uint64_t now)
{
    if (s->rate_bucket_us == 0u || now < s->rate_bucket_us) {
        s->rate_bucket_us = now;
        return;
    }
    uint64_t steps = (now - s->rate_bucket_us) / KVM_RP_RATE_BUCKET_US;
    if (steps == 0u) {
        return;
    }
    if (steps >= (uint64_t)KVM_RP_RATE_BUCKETS) {
        memset(s->rate, 0, sizeof(s->rate));
        s->rate_head = 0;
    } else {
        for (uint64_t k = 0; k < steps; k++) {
            s->rate_head = (uint8_t)((s->rate_head + 1u) % KVM_RP_RATE_BUCKETS);
            s->rate[s->rate_head] = 0;
        }
    }
    s->rate_bucket_us += steps * KVM_RP_RATE_BUCKET_US;
}

static uint32_t rate_count(const kvm_rp_session_t *s)
{
    uint32_t n = 0;
    for (int i = 0; i < KVM_RP_RATE_BUCKETS; i++) {
        n += (uint32_t)s->rate[i];
    }
    return n;
}

static void rate_add(kvm_rp_session_t *s)
{
    if (s->rate[s->rate_head] < 0xFFFFu) {
        s->rate[s->rate_head]++;
    }
}

static void rate_reset(kvm_rp_session_t *s)
{
    memset(s->rate, 0, sizeof(s->rate));
    s->rate_head = 0;
    s->rate_bucket_us = 0;
}

// ---- ownership transitions -----------------------------------------------------------------

/**
 * Every loss path goes through here: revoke + RELEASE_ALL(reason) + epoch bump are asked of the
 * integrator, which owns the arbiter (03 §4.2 INV-002, TECH-005).
 */
static void do_release(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_revoke_reason_t reason, bool notify,
                       uint64_t reply_to)
{
    kvm_release_reason_t rr = kvm_rp_release_reason(reason);
    uint32_t new_epoch = rp->owner_epoch;
    if ((unsigned)rr < (unsigned)KVM_RA_COUNT) {
        rp->c.release_requests[rr]++;
    }
    if (rp->hooks.control_release != NULL) {
        rp->hooks.control_release(rp->hooks.ctx, s->id, rr, &new_epoch);
    }
    rp->owner_kind = OWNER_NONE;
    rp->owner_session = 0;
    rp->owner_epoch = new_epoch;
    rp->c.client_revokes++;
    if ((unsigned)reason < (unsigned)KVM_RP_REVOKE_COUNT) {
        rp->c.revokes_by_reason[reason]++;
    }
    shadow_clear(s);
    rate_reset(s);
    if (notify) {
        kvm_rp_send_control_revoked(rp, s, reason, new_epoch, reply_to);
    }
}

static void close_now(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_err_t why)
{
    if (s->state == KVM_RP_SESS_CLOSING || s->state == KVM_RP_SESS_FREE) {
        return;
    }
    if (s->approval_pending) {
        s->approval_pending = false;
        if (rp->hooks.approval_cancel != NULL) {
            rp->hooks.approval_cancel(rp->hooks.ctx, s->id);
        }
    }
    if (is_owner(rp, s)) {
        do_release(rp, s, KVM_RP_REVOKE_PROTOCOL_ABUSE, false, 0);
    }
    s->state = KVM_RP_SESS_CLOSING;
    if (rp->hooks.close_session != NULL) {
        rp->hooks.close_session(rp->hooks.ctx, s->id, why);
    }
}

static kvm_rp_err_t do_grant(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to)
{
    uint32_t epoch = 0;
    kvm_rp_err_t e = KVM_RP_ERR_CONTROL_DENIED;
    if (rp->hooks.control_grant != NULL) {
        e = rp->hooks.control_grant(rp->hooks.ctx, s->id, &epoch);
    }
    if (e != KVM_RP_OK) {
        rp->c.control_denied++;
        return reject(rp, s, e, reply_to);
    }
    rp->owner_kind = OWNER_REMOTE;
    rp->owner_session = s->id;
    rp->owner_epoch = epoch;
    s->last_heartbeat_us = rp->now_us;
    s->stale = false;
    shadow_clear(s);
    rate_reset(s);
    rp->c.control_grants++;
    kvm_rp_send_control_granted(rp, s, epoch, reply_to);
    return KVM_RP_OK;
}

// ---- lifecycle -----------------------------------------------------------------------------

void kvm_rp_init(kvm_rp_t *rp, const kvm_rp_config_t *cfg, const kvm_rp_hooks_t *hooks)
{
    memset(rp, 0, sizeof(*rp));
    static const kvm_rp_config_t defaults = KVM_RP_CONFIG_DEFAULT;
    rp->cfg = (cfg != NULL) ? *cfg : defaults;
    if (rp->cfg.input_rate_limit == 0u) {
        rp->cfg.input_rate_limit = defaults.input_rate_limit;
    }
    if (rp->cfg.input_flood_limit < rp->cfg.input_rate_limit) {
        rp->cfg.input_flood_limit = rp->cfg.input_rate_limit * 2u;
    }
    if (rp->cfg.bad_frame_limit == 0u) {
        rp->cfg.bad_frame_limit = defaults.bad_frame_limit;
    }
    if (hooks != NULL) {
        rp->hooks = *hooks;
    }
    rp->host_usb = KVM_HOST_USB_UNINITIALIZED;
    rp->owner_kind = OWNER_NONE;
}

int kvm_rp_session_open(kvm_rp_t *rp, const kvm_rp_session_info_t *info, uint64_t now_us)
{
    rp->now_us = now_us;
    if (info == NULL || info->session_id == 0u || find(rp, info->session_id) != NULL) {
        rp->c.sessions_rejected++;
        return -1;
    }
    for (int i = 0; i < KVM_RP_MAX_SESSIONS; i++) {
        kvm_rp_session_t *s = &rp->s[i];
        if (s->state != KVM_RP_SESS_FREE) {
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->state = KVM_RP_SESS_OPEN;
        s->id = info->session_id;
        s->authenticated = info->authenticated;
        s->operator_role = info->operator_role && !info->pairing_only;
        s->pairing_only = info->pairing_only;
        s->opened_us = now_us;
        s->last_rx_us = now_us;
        s->last_heartbeat_us = now_us;
        rp->label_seq++;
        uint32_t h = info->label_entropy ^ (rp->label_seq * 0x9E3779B9u);
        static const char hexd[] = "0123456789abcdef";
        s->label[0] = 'c';
        s->label[1] = '-';
        for (int k = 0; k < 4; k++) {
            s->label[2 + k] = hexd[(h >> (12 - 4 * k)) & 0x0Fu];
        }
        s->label[6] = '\0';
        rp->c.sessions_opened++;
        return i;
    }
    rp->c.sessions_rejected++;
    return -1;
}

void kvm_rp_session_close(kvm_rp_t *rp, uint64_t session_id, kvm_rp_revoke_reason_t reason,
                          uint64_t now_us)
{
    rp->now_us = now_us;
    kvm_rp_session_t *s = find(rp, session_id);
    if (s == NULL) {
        return;
    }
    if (s->approval_pending) {
        s->approval_pending = false;
        if (rp->hooks.approval_cancel != NULL) {
            rp->hooks.approval_cancel(rp->hooks.ctx, s->id);
        }
    }
    if (is_owner(rp, s)) {
        do_release(rp, s, reason, false, 0);
    }
    s->state = KVM_RP_SESS_FREE;
    memset(s, 0, sizeof(*s));
}

void kvm_rp_set_host(kvm_rp_t *rp, kvm_host_usb_t usb, bool maintenance, bool wifi_connected,
                     uint64_t now_us)
{
    rp->now_us = now_us;
    rp->host_usb = usb;
    rp->maintenance = maintenance;
    rp->wifi_connected = wifi_connected;
}

void kvm_rp_set_owner(kvm_rp_t *rp, kvm_owner_kind_t kind, uint64_t session_id, uint32_t epoch,
                      uint64_t now_us)
{
    rp->now_us = now_us;
    uint64_t prev = (rp->owner_kind == OWNER_REMOTE) ? rp->owner_session : 0u;
    rp->owner_kind = kind;
    rp->owner_session = (kind == OWNER_REMOTE) ? session_id : 0u;
    rp->owner_epoch = epoch;
    if (prev != 0u && prev != rp->owner_session) {
        kvm_rp_session_t *s = find(rp, prev);
        if (s != NULL) {
            shadow_clear(s);
            rate_reset(s);
            s->stale = false;
        }
    }
}

void kvm_rp_notify_revoked(kvm_rp_t *rp, uint64_t session_id, kvm_rp_revoke_reason_t reason,
                           uint32_t new_epoch, uint64_t now_us)
{
    rp->now_us = now_us;
    kvm_rp_session_t *s = find(rp, session_id);
    if (rp->owner_kind == OWNER_REMOTE && rp->owner_session == session_id) {
        rp->owner_kind = OWNER_NONE;
        rp->owner_session = 0;
    }
    rp->owner_epoch = new_epoch;
    rp->c.client_revokes++;
    if ((unsigned)reason < (unsigned)KVM_RP_REVOKE_COUNT) {
        rp->c.revokes_by_reason[reason]++;
    }
    if (s == NULL) {
        return;
    }
    shadow_clear(s);
    rate_reset(s);
    s->stale = false;
    if (s->approval_pending) {
        s->approval_pending = false;
        if (rp->hooks.approval_cancel != NULL) {
            rp->hooks.approval_cancel(rp->hooks.ctx, s->id);
        }
    }
    if (s->state == KVM_RP_SESS_READY) {
        kvm_rp_send_control_revoked(rp, s, reason, new_epoch, 0);
    }
}

void kvm_rp_approval_result(kvm_rp_t *rp, uint64_t session_id, bool granted, uint64_t now_us)
{
    rp->now_us = now_us;
    kvm_rp_session_t *s = find(rp, session_id);
    if (s == NULL || !s->approval_pending) {
        return;
    }
    s->approval_pending = false;
    if (granted) {
        (void)do_grant(rp, s, 0);
    } else {
        rp->c.control_denied++;
        (void)reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, 0);
    }
}

// ---- coalesced movement --------------------------------------------------------------------

static int32_t clamp16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return v;
}

/** Emit the accumulated movement, preserving the sum: the remainder stays for the next flush. */
static kvm_rp_err_t flush_move(kvm_rp_t *rp, kvm_rp_session_t *s)
{
    int32_t ox = clamp16(s->pend_dx);
    int32_t oy = clamp16(s->pend_dy);
    s->pend_dx -= ox;
    s->pend_dy -= oy;
    s->pend_move = (s->pend_dx != 0 || s->pend_dy != 0);
    if (ox == 0 && oy == 0) {
        return KVM_RP_OK;
    }
    kvm_hid_intent_t it;
    memset(&it, 0, sizeof(it));
    it.type = KVM_HID_MOUSE_MOVE;
    it.origin = KVM_ORIGIN_REMOTE;
    it.epoch = rp->owner_epoch;
    it.seq = (uint32_t)s->last_seq;
    it.timestamp_us = rp->now_us;
    it.data.mouse_move.dx = (int16_t)ox;
    it.data.mouse_move.dy = (int16_t)oy;
    if (rp->hooks.hid_sink == NULL) {
        return KVM_RP_ERR_CONTROL_DENIED;
    }
    return rp->hooks.hid_sink(rp->hooks.ctx, s->id, rp->owner_epoch, &it);
}

void kvm_rp_poll(kvm_rp_t *rp, uint64_t now_us)
{
    rp->now_us = now_us;
    for (int i = 0; i < KVM_RP_MAX_SESSIONS; i++) {
        kvm_rp_session_t *s = &rp->s[i];
        if (s->state == KVM_RP_SESS_FREE || s->state == KVM_RP_SESS_CLOSING) {
            continue;
        }
        if (s->approval_pending && now_us >= s->approval_deadline_us) {
            // 03 §6.5 note: 30 s for physical confirmation, timeout -> reject, never blocking.
            s->approval_pending = false;
            rp->c.approval_timeouts++;
            rp->c.control_denied++;
            if (rp->hooks.approval_cancel != NULL) {
                rp->hooks.approval_cancel(rp->hooks.ctx, s->id);
            }
            (void)reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, 0);
        }
        if (is_owner(rp, s) && !s->stale &&
            now_us - s->last_heartbeat_us >= rp->cfg.heartbeat_stale_us) {
            // 03 §4.3: 3 s without a heartbeat -> STALE, sending lost immediately, release.
            s->stale = true;
            do_release(rp, s, KVM_RP_REVOKE_CLIENT_STALE, true, 0);
            continue;
        }
        if (s->pend_move && is_owner(rp, s)) {
            for (int k = 0; k < 8 && s->pend_move; k++) {
                if (flush_move(rp, s) != KVM_RP_OK) {
                    break;
                }
            }
        }
    }
}

// ---- payload helpers -----------------------------------------------------------------------

static bool get_int_in(const kvm_json_doc_t *d, int tok, int64_t lo, int64_t hi, int64_t *out)
{
    int64_t v;
    if (!kvm_json_get_i64(d, tok, &v) || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

/** Copy a string token, then drop control characters. Truncates on a UTF-8 boundary. */
static bool sanitized_string(const kvm_json_doc_t *d, int tok, char *out, size_t cap)
{
    char raw[RP_NAME_RAW_MAX];
    out[0] = '\0';
    size_t n = kvm_json_str_copy(d, tok, raw, sizeof(raw));
    if (n == (size_t)-1) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)raw[i];
        if (c < 0x20u || c == 0x7Fu) {
            continue; // never let a control character reach the UI or a counter label
        }
        if (o + 1u >= cap) {
            break;
        }
        out[o++] = (char)c;
    }
    while (o > 0u && ((uint8_t)out[o - 1u] & 0xC0u) == 0x80u) {
        o--; // do not leave a truncated UTF-8 sequence
    }
    out[o] = '\0';
    return true;
}

// ---- input (03 §6.3, §6.5) -----------------------------------------------------------------

typedef enum {
    RP_IN_MOVE = 0,
    RP_IN_BUTTON,
    RP_IN_WHEEL,
    RP_IN_KEY,
    RP_IN_SHORTCUT,
} rp_input_kind_t;

static kvm_rp_err_t handle_input(kvm_rp_t *rp, kvm_rp_session_t *s, int root, int pl, uint64_t seq)
{
    const kvm_json_doc_t *d = &rp->doc;

    if (pl < 0) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    // --- authorization before anything else: no payload semantics for a client that may not send
    if (rp->maintenance) {
        return reject(rp, s, KVM_RP_ERR_OTA_IN_PROGRESS, seq);
    }
    if (rp->host_usb == KVM_HOST_USB_SUSPENDED) {
        return reject(rp, s, KVM_RP_ERR_USB_SUSPENDED, seq);
    }
    if (rp->host_usb != KVM_HOST_USB_MOUNTED) {
        return reject(rp, s, KVM_RP_ERR_HOST_OFFLINE, seq);
    }
    // 03 §6.2: lease_epoch is mandatory for input. It is checked before ownership so that a frame
    // from the epoch before a revoke always answers ERR_STALE_EPOCH (VEC-005), not a weaker code.
    int tle = kvm_json_member(d, root, "lease_epoch");
    int64_t le;
    if (tle < 0 || !get_int_in(d, tle, 0, 0xFFFFFFFFll, &le)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    if ((uint32_t)le != rp->owner_epoch) {
        return reject(rp, s, KVM_RP_ERR_STALE_EPOCH, seq);
    }
    if (!s->operator_role || !is_owner(rp, s)) {
        return reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, seq);
    }
    if (rp->now_us - s->last_heartbeat_us >= rp->cfg.heartbeat_stale_us) {
        s->stale = true;
        do_release(rp, s, KVM_RP_REVOKE_CLIENT_STALE, true, seq);
        return reject(rp, s, KVM_RP_ERR_CLIENT_STALE, seq);
    }

    // --- rate window: every authorized input frame counts (03 §6.5)
    rate_advance(s, rp->now_us);
    uint32_t count = rate_count(s);
    if (count + 1u > rp->cfg.input_flood_limit) {
        rp->c.input_floods++;
        do_release(rp, s, KVM_RP_REVOKE_RATE_FLOOD, true, seq);
        kvm_rp_err_t r = reject(rp, s, KVM_RP_ERR_RATE_LIMIT, seq);
        close_now(rp, s, KVM_RP_ERR_RATE_LIMIT);
        return r;
    }
    rate_add(s);
    bool over = (count + 1u > rp->cfg.input_rate_limit);

    // --- payload
    int tk = kvm_json_member(d, pl, "kind");
    if (kvm_json_type(d, tk) != KVM_JSON_STRING) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    rp_input_kind_t kind;
    static const char *const keys_move[] = {"kind", "dx", "dy", NULL};
    static const char *const keys_btn[] = {"kind", "button", "down", NULL};
    static const char *const keys_wheel[] = {"kind", "vertical", "horizontal", NULL};
    static const char *const keys_key[] = {"kind", "usage_id", "down", NULL};
    static const char *const keys_cmd[] = {"kind", "command_id", NULL};
    const char *const *allowed;
    if (kvm_json_str_eq(d, tk, "mouse.move")) {
        kind = RP_IN_MOVE;
        allowed = keys_move;
    } else if (kvm_json_str_eq(d, tk, "mouse.button")) {
        kind = RP_IN_BUTTON;
        allowed = keys_btn;
    } else if (kvm_json_str_eq(d, tk, "mouse.wheel")) {
        kind = RP_IN_WHEEL;
        allowed = keys_wheel;
    } else if (kvm_json_str_eq(d, tk, "keyboard.key")) {
        kind = RP_IN_KEY;
        allowed = keys_key;
    } else if (kvm_json_str_eq(d, tk, "shortcut")) {
        kind = RP_IN_SHORTCUT;
        allowed = keys_cmd;
    } else {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    if (!kvm_json_keys_allowed(d, pl, allowed)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }

    kvm_hid_intent_t it;
    memset(&it, 0, sizeof(it));
    it.origin = KVM_ORIGIN_REMOTE;
    it.epoch = (uint32_t)le;
    it.seq = (uint32_t)seq;
    it.timestamp_us = rp->now_us;

    bool release_kind = false; // an UP is never dropped by the throttle (AGENTS §7 priority 2)
    uint8_t usage = 0;
    int32_t dx = 0;
    int32_t dy = 0;

    switch (kind) {
    case RP_IN_MOVE: {
        int64_t vx;
        int64_t vy;
        int tx = kvm_json_member(d, pl, "dx");
        int ty = kvm_json_member(d, pl, "dy");
        if (tx < 0 || ty < 0) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        if (kvm_json_type(d, tx) != KVM_JSON_NUMBER || kvm_json_type(d, ty) != KVM_JSON_NUMBER) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        if (!get_int_in(d, tx, -RP_DELTA_MAX, RP_DELTA_MAX, &vx) ||
            !get_int_in(d, ty, -RP_DELTA_MAX, RP_DELTA_MAX, &vy)) {
            return reject(rp, s, KVM_RP_ERR_RANGE, seq); // 03 §6.5: no hidden clipping
        }
        dx = (int32_t)vx;
        dy = (int32_t)vy;
        it.type = KVM_HID_MOUSE_MOVE;
        break;
    }
    case RP_IN_BUTTON: {
        int tb = kvm_json_member(d, pl, "button");
        int td = kvm_json_member(d, pl, "down");
        bool down;
        if (kvm_json_type(d, tb) != KVM_JSON_STRING || !kvm_json_get_bool(d, td, &down)) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        uint8_t btn;
        if (kvm_json_str_eq(d, tb, "left")) {
            btn = KVM_MOUSE_BTN_LEFT;
        } else if (kvm_json_str_eq(d, tb, "right")) {
            btn = KVM_MOUSE_BTN_RIGHT;
        } else if (kvm_json_str_eq(d, tb, "middle")) {
            btn = KVM_MOUSE_BTN_MIDDLE;
        } else {
            return reject(rp, s, KVM_RP_ERR_USAGE, seq);
        }
        uint8_t bit = (uint8_t)(1u << (btn - 1u));
        if (down) {
            if ((s->buttons_down & bit) != 0u) {
                rp->c.idempotent_key_down++; // same rule as a repeated key DOWN (03 §6.5)
                rp->c.frames_accepted++;
                return KVM_RP_OK;
            }
        } else if ((s->buttons_down & bit) == 0u) {
            rp->c.keyup_without_down++; // 03 §6.5: no-op + metric
            rp->c.frames_accepted++;
            return KVM_RP_OK;
        }
        it.type = down ? KVM_HID_MOUSE_BTN_DOWN : KVM_HID_MOUSE_BTN_UP;
        it.data.mouse_button.button_id = btn;
        usage = bit;
        release_kind = !down;
        break;
    }
    case RP_IN_WHEEL: {
        int tv = kvm_json_member(d, pl, "vertical");
        int th = kvm_json_member(d, pl, "horizontal");
        int64_t vv = 0;
        int64_t hh = 0;
        if (tv < 0 && th < 0) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        if (tv >= 0 && !get_int_in(d, tv, -RP_DELTA_MAX, RP_DELTA_MAX, &vv)) {
            return reject(rp, s, KVM_RP_ERR_RANGE, seq);
        }
        if (th >= 0 && !get_int_in(d, th, -RP_DELTA_MAX, RP_DELTA_MAX, &hh)) {
            return reject(rp, s, KVM_RP_ERR_RANGE, seq);
        }
        it.type = KVM_HID_MOUSE_WHEEL;
        it.data.wheel.vertical = (int16_t)vv;
        it.data.wheel.horizontal = (int16_t)hh;
        break;
    }
    case RP_IN_KEY: {
        int tu = kvm_json_member(d, pl, "usage_id");
        int td = kvm_json_member(d, pl, "down");
        int64_t uv;
        bool down;
        if (tu < 0 || kvm_json_type(d, tu) != KVM_JSON_NUMBER || !kvm_json_get_bool(d, td, &down)) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        if (!get_int_in(d, tu, 0, 255, &uv)) {
            return reject(rp, s, KVM_RP_ERR_USAGE, seq); // 03 §6.5: usage_id outside 0..255
        }
        usage = (uint8_t)uv;
        if (!kvm_usage_is_valid_key(usage)) {
            return reject(rp, s, KVM_RP_ERR_USAGE, seq); // unknown key: no HID
        }
        if (down) {
            if (key_shadow(s, usage)) {
                rp->c.idempotent_key_down++; // 03 §6.5: do not repeat to USB
                rp->c.frames_accepted++;
                return KVM_RP_OK;
            }
        } else if (!key_shadow(s, usage)) {
            rp->c.keyup_without_down++; // 03 §6.5: no-op + metric
            rp->c.frames_accepted++;
            return KVM_RP_OK;
        }
        it.type = down ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP;
        it.data.key.usage_id = usage;
        release_kind = !down;
        break;
    }
    case RP_IN_SHORTCUT:
    default: {
        int tc = kvm_json_member(d, pl, "command_id");
        if (kvm_json_type(d, tc) != KVM_JSON_STRING) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        uint16_t chord = 0;
        for (uint16_t id = 1; id < (uint16_t)KVM_CHORD_COUNT; id++) {
            const kvm_chord_t *c = kvm_chord_get(id);
            if (c != NULL && c->name != NULL && kvm_json_str_eq(d, tc, c->name)) {
                chord = id;
                break;
            }
        }
        if (chord == 0u) {
            return reject(rp, s, KVM_RP_ERR_USAGE, seq); // 03 §3 rule 2: allowlist only
        }
        it.type = KVM_HID_CHORD;
        it.data.chord.chord_id = chord;
        break;
    }
    }

    // --- throttle policy (03 §6.5): coalesce movement, reject the non-idempotent excess, and
    //     never drop an UP, which could only leave a key or a button pressed.
    if (over) {
        if (kind == RP_IN_MOVE) {
            s->pend_dx += dx;
            s->pend_dy += dy;
            s->pend_move = (s->pend_dx != 0 || s->pend_dy != 0);
            rp->c.input_coalesced++;
            rp->c.frames_accepted++;
            return KVM_RP_OK;
        }
        if (!release_kind) {
            rp->c.input_throttled++;
            return reject(rp, s, KVM_RP_ERR_RATE_LIMIT, seq);
        }
    }

    kvm_rp_err_t e;
    if (kind == RP_IN_MOVE) {
        s->pend_dx += dx;
        s->pend_dy += dy;
        e = flush_move(rp, s);
    } else if (rp->hooks.hid_sink != NULL) {
        e = rp->hooks.hid_sink(rp->hooks.ctx, s->id, (uint32_t)le, &it);
    } else {
        e = KVM_RP_ERR_CONTROL_DENIED;
    }
    if (e != KVM_RP_OK) {
        return reject(rp, s, e, seq);
    }
    if (kind == RP_IN_KEY) {
        key_shadow_set(s, usage, it.type == KVM_HID_KEY_DOWN);
    } else if (kind == RP_IN_BUTTON) {
        if (it.type == KVM_HID_MOUSE_BTN_DOWN) {
            s->buttons_down |= usage;
        } else {
            s->buttons_down = (uint8_t)(s->buttons_down & (uint8_t)~usage);
        }
    }
    rp->c.input_accepted++;
    rp->c.frames_accepted++;
    if (rp->cfg.send_input_ack) {
        kvm_rp_send_input_ack(rp, s, seq);
    }
    return KVM_RP_OK;
}

// ---- session commands ----------------------------------------------------------------------

static const char *const k_empty_keys[] = {NULL};

static kvm_rp_err_t handle_hello(kvm_rp_t *rp, kvm_rp_session_t *s, int pl, uint64_t seq)
{
    const kvm_json_doc_t *d = &rp->doc;
    if (s->state != KVM_RP_SESS_OPEN) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq); // a second hello
    }
    static const char *const keys[] = {"client_name", "capabilities", NULL};
    if (pl >= 0 && !kvm_json_keys_allowed(d, pl, keys)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    if (pl >= 0) {
        int tn = kvm_json_member(d, pl, "client_name");
        if (tn >= 0) {
            if (kvm_json_type(d, tn) != KVM_JSON_STRING ||
                !sanitized_string(d, tn, s->name, sizeof(s->name))) {
                return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
            }
        }
        int tc = kvm_json_member(d, pl, "capabilities");
        if (tc >= 0) {
            if (kvm_json_type(d, tc) != KVM_JSON_ARRAY || kvm_json_count(d, tc) > 8u) {
                return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
            }
            for (uint16_t i = 0; i < kvm_json_count(d, tc); i++) {
                int te = kvm_json_element(d, tc, i);
                char tmp[24];
                if (kvm_json_type(d, te) != KVM_JSON_STRING ||
                    kvm_json_str_copy(d, te, tmp, sizeof(tmp)) == (size_t)-1) {
                    return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
                }
            }
        }
    }
    s->state = KVM_RP_SESS_READY;
    rp->c.hello_ok++;
    rp->c.frames_accepted++;
    kvm_rp_send_hello_ack(rp, s, seq);
    return KVM_RP_OK;
}

static kvm_rp_err_t handle_status(kvm_rp_t *rp, kvm_rp_session_t *s, int pl, uint64_t seq)
{
    if (pl >= 0 && !kvm_json_keys_allowed(&rp->doc, pl, k_empty_keys)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    rp->c.frames_accepted++;
    kvm_rp_send_status(rp, s, seq);
    return KVM_RP_OK;
}

static kvm_rp_err_t handle_control_request(kvm_rp_t *rp, kvm_rp_session_t *s, int pl, uint64_t seq)
{
    const kvm_json_doc_t *d = &rp->doc;
    static const char *const keys[] = {"reason", NULL};
    char reason[KVM_RP_REASON_MAX];
    reason[0] = '\0';
    if (pl >= 0) {
        if (!kvm_json_keys_allowed(d, pl, keys)) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        int tr = kvm_json_member(d, pl, "reason");
        if (tr >= 0 && (kvm_json_type(d, tr) != KVM_JSON_STRING ||
                        !sanitized_string(d, tr, reason, sizeof(reason)))) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
    }
    rp->c.control_requests++;
    if (!s->operator_role) {
        rp->c.control_denied++;
        return reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, seq); // FR-404: observers never get HID
    }
    if (rp->maintenance) {
        return reject(rp, s, KVM_RP_ERR_OTA_IN_PROGRESS, seq);
    }
    if (is_owner(rp, s)) {
        rp->c.frames_accepted++;
        kvm_rp_send_control_granted(rp, s, rp->owner_epoch, seq); // idempotent re-answer
        return KVM_RP_OK;
    }
    if (s->approval_pending) {
        rp->c.control_denied++;
        return reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, seq);
    }
    for (int i = 0; i < KVM_RP_MAX_SESSIONS; i++) {
        if (rp->s[i].state != KVM_RP_SESS_FREE && rp->s[i].approval_pending) {
            rp->c.control_denied++;
            return reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, seq); // one prompt at a time
        }
    }
    kvm_rp_err_t probe = KVM_RP_ERR_CONTROL_DENIED;
    if (rp->hooks.control_request != NULL) {
        probe = rp->hooks.control_request(rp->hooks.ctx, s->id);
    }
    if (probe != KVM_RP_OK) {
        rp->c.control_denied++;
        return reject(rp, s, probe, seq);
    }
    kvm_rp_approval_t a = KVM_RP_APPROVAL_DENIED;
    if (rp->hooks.approval_begin != NULL) {
        a = rp->hooks.approval_begin(rp->hooks.ctx, s->id, s->name, reason);
    }
    if (a == KVM_RP_APPROVAL_GRANTED) {
        rp->c.frames_accepted++;
        return do_grant(rp, s, seq);
    }
    if (a == KVM_RP_APPROVAL_PENDING) {
        s->approval_pending = true;
        s->approval_deadline_us = rp->now_us + rp->cfg.approval_timeout_us;
        rp->c.frames_accepted++;
        kvm_rp_send_control_pending(rp, s, seq);
        return KVM_RP_OK;
    }
    rp->c.control_denied++;
    return reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, seq);
}

static kvm_rp_err_t handle_control_release(kvm_rp_t *rp, kvm_rp_session_t *s, int pl, uint64_t seq)
{
    if (pl >= 0 && !kvm_json_keys_allowed(&rp->doc, pl, k_empty_keys)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    if (s->approval_pending) {
        s->approval_pending = false;
        if (rp->hooks.approval_cancel != NULL) {
            rp->hooks.approval_cancel(rp->hooks.ctx, s->id);
        }
    }
    if (!is_owner(rp, s)) {
        return reject(rp, s, KVM_RP_ERR_CONTROL_DENIED, seq);
    }
    rp->c.frames_accepted++;
    do_release(rp, s, KVM_RP_REVOKE_CLIENT_RELEASE, true, seq);
    return KVM_RP_OK;
}

static kvm_rp_err_t handle_ping(kvm_rp_t *rp, kvm_rp_session_t *s, int pl, uint64_t seq)
{
    const kvm_json_doc_t *d = &rp->doc;
    static const char *const keys[] = {"nonce", NULL};
    bool has_nonce = false;
    int64_t nonce = 0;
    if (pl >= 0) {
        if (!kvm_json_keys_allowed(d, pl, keys)) {
            return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
        }
        int tn = kvm_json_member(d, pl, "nonce");
        if (tn >= 0) {
            if (!get_int_in(d, tn, -RP_SEQ_MAX, RP_SEQ_MAX, &nonce)) {
                return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
            }
            has_nonce = true;
        }
    }
    if (is_owner(rp, s)) {
        s->last_heartbeat_us = rp->now_us;
        s->stale = false;
        if (rp->hooks.heartbeat != NULL) {
            rp->hooks.heartbeat(rp->hooks.ctx, s->id); // 03 §4.3 on_heartbeat: lease renewal
        }
    }
    rp->c.pings++;
    rp->c.frames_accepted++;
    kvm_rp_send_pong(rp, s, has_nonce, nonce, seq);
    return KVM_RP_OK;
}

static kvm_rp_err_t handle_session_close(kvm_rp_t *rp, kvm_rp_session_t *s, int pl, uint64_t seq)
{
    if (pl >= 0 && !kvm_json_keys_allowed(&rp->doc, pl, k_empty_keys)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, seq);
    }
    rp->c.frames_accepted++;
    uint64_t id = s->id;
    kvm_rp_session_close(rp, id, KVM_RP_REVOKE_SESSION_CLOSED, rp->now_us);
    if (rp->hooks.close_session != NULL) {
        rp->hooks.close_session(rp->hooks.ctx, id, KVM_RP_OK);
    }
    return KVM_RP_OK;
}

static kvm_rp_err_t handle_pair(kvm_rp_t *rp, kvm_rp_session_t *s, const char *type, int pl,
                                uint64_t seq)
{
    rp->c.pair_frames++;
    if (rp->hooks.pair_frame == NULL) {
        // SEC-103: outside an open pairing window only ERR_PAIRING_EXPIRED exists.
        return reject_abuse(rp, s, KVM_RP_ERR_PAIRING_EXPIRED, seq);
    }
    kvm_rp_err_t e = rp->hooks.pair_frame(rp->hooks.ctx, s->id, type, &rp->doc, pl);
    if (e != KVM_RP_OK) {
        return reject_abuse(rp, s, e, seq);
    }
    rp->c.frames_accepted++;
    return KVM_RP_OK;
}

// ---- frame entry point ---------------------------------------------------------------------

kvm_rp_err_t kvm_rp_on_frame(kvm_rp_t *rp, uint64_t session_id, const char *frame, size_t len,
                             uint64_t now_us)
{
    rp->now_us = now_us;
    rp->c.frames_in++;

    kvm_rp_session_t *s = find(rp, session_id);
    if (s == NULL || s->state == KVM_RP_SESS_CLOSING) {
        return count_err(rp, KVM_RP_ERR_SESSION); // unknown or already closing: no reply
    }
    s->last_rx_us = now_us;
    if (!s->authenticated) {
        rp->c.client_auth_denials++; // TECH-061, TECH-042
        return reject_abuse(rp, s, KVM_RP_ERR_AUTH, 0);
    }
    if (len == 0u || len > KVM_JSON_MAX_FRAME) {
        rp->c.frames_oversize++;
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, 0);
    }

    kvm_json_err_t je = kvm_json_parse(&rp->doc, frame, len);
    if (je != KVM_JSON_OK) {
        if ((unsigned)je < (unsigned)KVM_JSON_ERR_COUNT) {
            rp->c.parse_errors[je]++;
        }
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, 0);
    }
    const kvm_json_doc_t *d = &rp->doc;
    int root = kvm_json_root(d);
    if (kvm_json_type(d, root) != KVM_JSON_OBJECT) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, 0);
    }
    static const char *const env_keys[] = {"v", "type", "seq", "session_id", "lease_epoch",
                                           "payload", NULL};
    if (!kvm_json_keys_allowed(d, root, env_keys)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, 0);
    }

    // v (03 §6.2): an unknown version closes the session, it is never guessed
    int64_t v = 0;
    int tv = kvm_json_member(d, root, "v");
    if (tv < 0 || !kvm_json_get_i64(d, tv, &v)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, 0);
    }
    if (v != 1) {
        kvm_rp_err_t r = reject(rp, s, KVM_RP_ERR_VERSION, 0);
        close_now(rp, s, KVM_RP_ERR_VERSION);
        return r;
    }

    // type
    int tt = kvm_json_member(d, root, "type");
    char type[RP_TYPE_MAX];
    if (kvm_json_type(d, tt) != KVM_JSON_STRING ||
        kvm_json_str_copy(d, tt, type, sizeof(type)) == (size_t)-1) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, 0);
    }

    // seq + replay guard (03 §6.5)
    int ts = kvm_json_member(d, root, "seq");
    int64_t seq = 0;
    if (ts < 0 || !get_int_in(d, ts, 1, RP_SEQ_MAX, &seq)) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, 0);
    }
    if (s->have_seq && (uint64_t)seq <= s->last_seq) {
        return reject_abuse(rp, s, KVM_RP_ERR_REPLAY, (uint64_t)seq);
    }

    // session_id: the server's own label, assigned in hello.ack
    bool is_hello = (strcmp(type, "hello") == 0);
    bool is_pair = (strncmp(type, "pair.", 5) == 0);
    int tsid = kvm_json_member(d, root, "session_id");
    if (s->state == KVM_RP_SESS_OPEN) {
        if (tsid >= 0 && !kvm_json_str_eq(d, tsid, "")) {
            return reject_abuse(rp, s, KVM_RP_ERR_SESSION, (uint64_t)seq);
        }
    } else if (tsid < 0 || !kvm_json_str_eq(d, tsid, s->label)) {
        return reject_abuse(rp, s, KVM_RP_ERR_SESSION, (uint64_t)seq);
    }

    // payload shape
    int pl = kvm_json_member(d, root, "payload");
    if (pl >= 0 && kvm_json_type(d, pl) != KVM_JSON_OBJECT) {
        return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, (uint64_t)seq);
    }

    s->last_seq = (uint64_t)seq;
    s->have_seq = true;

    // SEC-103: the pairing endpoint carries nothing but pair.*
    if (s->pairing_only && !is_pair) {
        return reject_abuse(rp, s, KVM_RP_ERR_PAIRING_EXPIRED, (uint64_t)seq);
    }
    if (!s->pairing_only && is_pair) {
        return reject_abuse(rp, s, KVM_RP_ERR_PAIRING_EXPIRED, (uint64_t)seq);
    }
    if (!is_hello && !is_pair && s->state != KVM_RP_SESS_READY) {
        return reject_abuse(rp, s, KVM_RP_ERR_SESSION, (uint64_t)seq); // hello comes first
    }

    if (is_hello) {
        return handle_hello(rp, s, pl, (uint64_t)seq);
    }
    if (is_pair) {
        return handle_pair(rp, s, type, pl, (uint64_t)seq);
    }
    if (strcmp(type, "status.get") == 0) {
        return handle_status(rp, s, pl, (uint64_t)seq);
    }
    if (strcmp(type, "control.request") == 0) {
        return handle_control_request(rp, s, pl, (uint64_t)seq);
    }
    if (strcmp(type, "control.release") == 0) {
        return handle_control_release(rp, s, pl, (uint64_t)seq);
    }
    if (strcmp(type, "ping") == 0) {
        return handle_ping(rp, s, pl, (uint64_t)seq);
    }
    if (strcmp(type, "input") == 0) {
        return handle_input(rp, s, root, pl, (uint64_t)seq);
    }
    if (strcmp(type, "session.close") == 0) {
        return handle_session_close(rp, s, pl, (uint64_t)seq);
    }
    // Everything else, `settings.*` included (SEC-107, ADR-023 AT-EXT-19).
    return reject_abuse(rp, s, KVM_RP_ERR_BAD_FRAME, (uint64_t)seq);
}
