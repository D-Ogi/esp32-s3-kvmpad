// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// CLIENT protocol v1 (spec 03 §6, §7; TECH-043..046) — RemoteProtocolValidator plus the session
// and command layer.
//
// What this component is:
//   - the only place where untrusted network bytes are turned into typed intents;
//   - a validator that implements every row of 03 §6.5 with the error codes of 03 §6.4/§7;
//   - a session state machine (hello, status, control, ping, input, close) with lease epochs,
//     replay protection, a rolling input rate limit and a 30 s non-blocking approval timer.
//
// What this component is NOT and never becomes:
//   - a HID emitter. It never includes tusb.h/tinyusb.h and never touches the pressed-ledger.
//     Validated input leaves through kvm_rp_hooks_t::hid_sink as a kvm_hid_intent_t, which the
//     integrator forwards to hid_dispatch -> InputArbiter -> HidScheduler (03 INV-001).
//   - the source of truth for HOST_ACTIVE, ownership or the epoch. Those arrive through
//     kvm_rp_set_host() / kvm_rp_set_owner() from the authoritative hid_core snapshot.
//   - a TLS, WebSocket, auth or pairing implementation. The transport hands over complete,
//     already authenticated text frames; `pair.*` frames are routed to secure_remote (ADR-022).
//   - a logger. Counters only: no payload, usage id, client name, token or code is ever logged
//     or echoed in an error payload (TECH-046, SEC-105, AT-SEC-08).
//
// Pure C11, no ESP-IDF dependency, no allocation on any path, monotonic time injected by the
// caller. Host unit tests in test/remote_protocol.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_core.h"       // kvm_host_usb_t, kvm_owner_kind_t, kvm_release_reason_t, kvm_hid_err_t
#include "kvm_hid_intent.h" // kvm_hid_intent_t
#include "remote_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Concurrent authenticated sessions: 1 operator + observers (03 §6.5: max 1 active operator). */
#define KVM_RP_MAX_SESSIONS 4

/** Wire `session_id` label (03 §6.2: a non-secret server label, never an authorization token). */
#define KVM_RP_LABEL_MAX 16

/** Client name accepted from `hello` / `pair.hello` (FR-724); sanitized before it reaches the UI. */
#define KVM_RP_NAME_MAX 32

/** `control.request` reason string (diagnostic only). */
#define KVM_RP_REASON_MAX 24

/** Outbound frame budget; the same 1024 B limit as inbound (03 §6.1). */
#define KVM_RP_OUT_MAX KVM_JSON_MAX_FRAME

/** Rolling input-rate window: 10 buckets of 100 ms = 1 s (03 §6.5 ">120 input msg/s"). */
#define KVM_RP_RATE_BUCKETS 10
#define KVM_RP_RATE_BUCKET_US 100000u

// ---- Error codes ---------------------------------------------------------------------------
//
// The wire strings are the codes of 03 §6.4/§7. ERR_AUTH and ERR_RATE_LIMIT are additions
// recorded in docs/ADR/ADR-030-remote-protocol-component.md (03 §6.5 names the conditions but no
// code for them).
typedef enum {
    KVM_RP_OK = 0,
    KVM_RP_ERR_VERSION,         // "ERR_VERSION"       v != 1
    KVM_RP_ERR_BAD_FRAME,       // "ERR_BAD_FRAME"     size / syntax / depth / unknown type or member
    KVM_RP_ERR_RANGE,           // "ERR_RANGE"         dx/dy/wheel outside the contract range
    KVM_RP_ERR_USAGE,           // "ERR_USAGE"         usage id or command id not on the allowlist
    KVM_RP_ERR_REPLAY,          // "ERR_REPLAY"        seq <= last_seq
    KVM_RP_ERR_STALE_EPOCH,     // "ERR_STALE_EPOCH"   lease_epoch != owner.epoch
    KVM_RP_ERR_SESSION,         // "ERR_SESSION"       session_id does not match the connection
    KVM_RP_ERR_HOST_OFFLINE,    // "ERR_HOST_OFFLINE"  no USB_MOUNTED
    KVM_RP_ERR_USB_SUSPENDED,   // "ERR_USB_SUSPENDED"
    KVM_RP_ERR_CLIENT_STALE,    // "ERR_CLIENT_STALE"  no heartbeat within the stale window
    KVM_RP_ERR_CONTROL_DENIED,  // "ERR_CONTROL_DENIED" not the operator / approval refused
    KVM_RP_ERR_PAIRING_EXPIRED, // "ERR_PAIRING_EXPIRED" pair.* outside an open window (SEC-103)
    KVM_RP_ERR_OTA_IN_PROGRESS, // "ERR_OTA_IN_PROGRESS" maintenance mode
    KVM_RP_ERR_HID_QUEUE_OVERFLOW, // "ERR_HID_QUEUE_OVERFLOW" the arbiter queue is saturated
    KVM_RP_ERR_AUTH,            // "ERR_AUTH"          session not authenticated (ADR-030)
    KVM_RP_ERR_RATE_LIMIT,      // "ERR_RATE_LIMIT"    non-idempotent excess over the limit (ADR-030)
    KVM_RP_ERR_COUNT,
} kvm_rp_err_t;

/** Wire string of a code, e.g. "ERR_BAD_FRAME". Safe to log: no payload, no secret. */
const char *kvm_rp_err_name(kvm_rp_err_t err);

/** True when a client may retry the same request later (03 §6.4 `retryable`). */
bool kvm_rp_err_retryable(kvm_rp_err_t err);

/** Map an arbiter result to the protocol code, so the integrator never invents a mapping. */
kvm_rp_err_t kvm_rp_err_from_hid(kvm_hid_err_t err);

// ---- Revoke reasons (wire strings of `control.revoked.reason`) ------------------------------
typedef enum {
    KVM_RP_REVOKE_CLIENT_RELEASE = 0, // "client_release"
    KVM_RP_REVOKE_CLIENT_STALE,       // "client_stale"
    KVM_RP_REVOKE_SESSION_CLOSED,     // "session_closed"
    KVM_RP_REVOKE_LOCAL_OVERRIDE,     // "local_override"
    KVM_RP_REVOKE_HOST_OFFLINE,       // "host_offline"
    KVM_RP_REVOKE_MAINTENANCE,        // "maintenance"
    KVM_RP_REVOKE_RATE_FLOOD,         // "rate_flood"
    KVM_RP_REVOKE_PROTOCOL_ABUSE,     // "protocol_abuse"
    KVM_RP_REVOKE_UNPAIRED,           // "unpaired"
    KVM_RP_REVOKE_COUNT,
} kvm_rp_revoke_reason_t;

const char *kvm_rp_revoke_reason_name(kvm_rp_revoke_reason_t reason);

/** The kvm_hid_core RELEASE_ALL reason that belongs to a revoke reason (03 §4.2, TECH-005). */
kvm_release_reason_t kvm_rp_release_reason(kvm_rp_revoke_reason_t reason);

// ---- Physical approval (03 §6.5 note, PROC-006 step 5, ADR-023 `control.approval`) ----------
typedef enum {
    KVM_RP_APPROVAL_GRANTED = 0, // policy `auto`/`trusted`: no prompt, grant now
    KVM_RP_APPROVAL_PENDING,     // the UI is asking; answer with kvm_rp_approval_result()
    KVM_RP_APPROVAL_DENIED,      // policy or UI refuses immediately
} kvm_rp_approval_t;

// ---- Read-only status source (03 §6.6) -----------------------------------------------------
//
// Filled by the integrator from authoritative snapshots. It carries no token, no SSID password,
// no pairing secret, no key event and no policy value (SEC-107, TECH-046).
typedef struct {
    kvm_host_usb_t host_usb;
    bool maintenance;
    bool wifi_connected;
    kvm_owner_kind_t owner;
    uint32_t lease_epoch;
    char device_id[24];    // e.g. "kvm-7f3a"
    char fw[16];           // e.g. "1.0.0"
    char host_layout[24];  // ADR-024 / FR-745: an enumerated id from kbd_layouts, never free text
} kvm_rp_status_t;

// ---- Integrator hooks ----------------------------------------------------------------------
//
// Every hook runs in the caller's context, synchronously, and must not block. None of them may
// call back into kvm_rp_* on the same instance (the component is not reentrant).
typedef struct {
    /**
     * Deliver one validated intent. The integrator forwards it to hid_dispatch with
     * origin = KVM_ORIGIN_REMOTE and the given session/lease epoch, and returns the mapped
     * arbiter result (kvm_rp_err_from_hid()). Returning anything but KVM_RP_OK means no HID left
     * the arbiter; the component counts it and answers the client with that code.
     */
    kvm_rp_err_t (*hid_sink)(void *ctx, uint64_t session_id, uint32_t lease_epoch,
                             const kvm_hid_intent_t *intent);

    /** 03 §4.3 on_control_request probe: is a lease admissible now (LOCAL idle, no other lease)? */
    kvm_rp_err_t (*control_request)(void *ctx, uint64_t session_id);

    /** After approval: new epoch + RELEASE_ALL + owner = REMOTE. Writes the granted lease epoch. */
    kvm_rp_err_t (*control_grant)(void *ctx, uint64_t session_id, uint32_t *lease_epoch);

    /**
     * Revoke + RELEASE_ALL(reason) + epoch bump for this session (03 §4.2 INV-002). Must write the
     * epoch in force afterwards to `*new_epoch`. Called on every loss path: client release,
     * session close, stale heartbeat, flood, protocol abuse.
     */
    void (*control_release)(void *ctx, uint64_t session_id, kvm_release_reason_t reason,
                            uint32_t *new_epoch);

    /** 03 §4.3 on_heartbeat: renew the lease of the owning session. */
    void (*heartbeat)(void *ctx, uint64_t session_id);

    /**
     * Start the physical approval prompt (non-blocking). `client_name` and `reason` are sanitized,
     * NUL-terminated and safe to render. Returning KVM_RP_APPROVAL_PENDING arms a 30 s timer.
     */
    kvm_rp_approval_t (*approval_begin)(void *ctx, uint64_t session_id, const char *client_name,
                                        const char *reason);

    /** Withdraw a pending prompt (timeout, session gone, local override). */
    void (*approval_cancel)(void *ctx, uint64_t session_id);

    /** Send one complete text frame to this session. `frame` is NUL-terminated, `len` bytes. */
    void (*send)(void *ctx, uint64_t session_id, const char *frame, size_t len);

    /**
     * Ask the transport to drop the connection; the component has already sent its last answer.
     * The transport must then call kvm_rp_session_close() for that session (directly or once the
     * socket is really gone), otherwise the slot stays occupied until it does.
     */
    void (*close_session)(void *ctx, uint64_t session_id, kvm_rp_err_t reason);

    /** Fill the read-only snapshot for `status.get`. */
    void (*status_fill)(void *ctx, kvm_rp_status_t *out);

    /**
     * ADR-022 `pair.*` routing to secure_remote. `type` is the validated type string, `payload` the
     * payload object token (-1 when absent). Return KVM_RP_OK when the frame was consumed (the
     * pairing state machine answers through `send` itself), or an error code to be reported.
     * NULL hook = no pairing window exists -> every `pair.*` frame gets ERR_PAIRING_EXPIRED
     * (SEC-103).
     */
    kvm_rp_err_t (*pair_frame)(void *ctx, uint64_t session_id, const char *type,
                               const kvm_json_doc_t *doc, int payload);

    void *ctx;
} kvm_rp_hooks_t;

// ---- Configuration -------------------------------------------------------------------------
typedef struct {
    uint32_t input_rate_limit;    // 03 §6.5: >120 input msg/s -> coalesce / throttle
    uint32_t input_flood_limit;   // beyond this: revoke + release (ADR-030)
    uint64_t heartbeat_stale_us;  // 03 §4.3: 3 s without a heartbeat -> STALE, sending lost
    uint64_t lease_us;            // 03 §4.3: 4 s, reported as `expires_in_ms`
    uint64_t approval_timeout_us; // 03 §6.5 note: 30 s -> reject
    uint32_t bad_frame_limit;     // 03 §6.5: "count + close on abuse"
    bool send_input_ack;          // 03 §6.3: `input.ack` is optional; off by default
    bool send_reply_to_seq;       // 03 §6.4: add `reply_to_seq` to replies
} kvm_rp_config_t;

#define KVM_RP_CONFIG_DEFAULT                                                                      \
    {                                                                                              \
        .input_rate_limit = 120u, .input_flood_limit = 240u, .heartbeat_stale_us = 3000000u,        \
        .lease_us = 4000000u, .approval_timeout_us = 30000000u, .bad_frame_limit = 8u,              \
        .send_input_ack = false, .send_reply_to_seq = true,                                         \
    }

// ---- Counters (TECH-061; aggregated, never a payload) --------------------------------------
typedef struct {
    uint32_t frames_in;
    uint32_t frames_oversize;
    uint32_t frames_accepted;
    uint32_t frames_rejected;
    uint32_t frames_out;
    uint32_t frames_out_truncated; // a reply did not fit 1024 B: dropped, never half-sent
    uint32_t parse_errors[KVM_JSON_ERR_COUNT];
    uint32_t errors[KVM_RP_ERR_COUNT];
    uint32_t client_auth_denials; // TECH-061
    uint32_t client_revokes;      // TECH-061
    uint32_t sessions_opened;
    uint32_t sessions_rejected;
    uint32_t hello_ok;
    uint32_t status_snapshots;
    uint32_t control_requests;
    uint32_t control_grants;
    uint32_t control_denied;
    uint32_t approval_timeouts;
    uint32_t pings;
    uint32_t input_accepted;
    uint32_t input_coalesced;   // mouse moves merged while over the rate limit
    uint32_t input_throttled;   // non-idempotent input rejected over the limit
    uint32_t input_floods;      // rolling count over input_flood_limit -> revoke
    uint32_t idempotent_key_down; // 03 §6.5: already down -> no repeat to USB
    uint32_t keyup_without_down;  // 03 §6.5 metric
    uint32_t pair_frames;
    uint32_t release_requests[KVM_RA_COUNT]; // RELEASE_ALL asked for, by reason
    uint32_t revokes_by_reason[KVM_RP_REVOKE_COUNT];
} kvm_rp_counters_t;

// ---- Session / instance state (exposed for static allocation only; use the API) ------------
typedef enum {
    KVM_RP_SESS_FREE = 0,
    KVM_RP_SESS_OPEN,    // authenticated, no `hello` yet
    KVM_RP_SESS_READY,   // `hello` done, label assigned
    KVM_RP_SESS_CLOSING, // answered, waiting for the transport to drop it
} kvm_rp_sess_state_t;

typedef struct {
    uint8_t state;
    bool authenticated;
    bool operator_role; // false = read-only observer (FR-404)
    bool pairing_only;  // SEC-103: only `pair.*`, zero status, zero HID
    bool stale;         // no heartbeat within heartbeat_stale_us while owning control
    bool approval_pending;
    uint64_t id;                     // arbiter session id (uint64, never sent on the wire)
    char label[KVM_RP_LABEL_MAX];    // wire `session_id`
    char name[KVM_RP_NAME_MAX];      // sanitized client name (UI only)
    uint64_t last_seq;               // 03 §6.5 replay guard
    bool have_seq;
    uint32_t out_seq;                // own server->client counter (03 §6.4)
    uint64_t opened_us;
    uint64_t last_rx_us;
    uint64_t last_heartbeat_us;
    uint64_t approval_deadline_us;
    uint32_t bad_frames;
    uint16_t rate[KVM_RP_RATE_BUCKETS];
    uint64_t rate_bucket_us; // start of the newest bucket
    uint8_t rate_head;
    int32_t pend_dx, pend_dy; // coalesced mouse movement (sum preserved)
    bool pend_move;
    uint8_t keys_down[32]; // shadow bitmap of usages this session pressed (metrics + idempotence)
    uint8_t buttons_down;  // shadow mask, bits 0..2
} kvm_rp_session_t;

typedef struct {
    kvm_rp_config_t cfg;
    kvm_rp_hooks_t hooks;
    kvm_rp_session_t s[KVM_RP_MAX_SESSIONS];
    kvm_host_usb_t host_usb;
    bool maintenance;
    bool wifi_connected;
    kvm_owner_kind_t owner_kind;
    uint64_t owner_session;
    uint32_t owner_epoch;
    uint32_t label_seq;
    uint64_t now_us;
    kvm_json_doc_t doc;          // one shared parse document: no per-frame stack or heap cost
    char out[KVM_RP_OUT_MAX + 1]; // one shared serializer buffer
    kvm_rp_counters_t c;
} kvm_rp_t;

// ---- API -----------------------------------------------------------------------------------

/** `cfg` NULL = KVM_RP_CONFIG_DEFAULT. Zero-initializes every session and counter. */
void kvm_rp_init(kvm_rp_t *rp, const kvm_rp_config_t *cfg, const kvm_rp_hooks_t *hooks);

typedef struct {
    uint64_t session_id;     // arbiter session id, non-zero and unique among open sessions
    bool authenticated;      // TLS + token verified by the transport (03 §6.1)
    bool operator_role;      // may ask for control (03 §6.3); false = observer
    bool pairing_only;       // SEC-103 pairing endpoint session
    uint32_t label_entropy;  // randomness for the wire label; the label is not a secret
} kvm_rp_session_info_t;

/**
 * Register an accepted connection. Returns the slot index, or -1 when the table is full or the
 * session id is zero / already open (then `sessions_rejected` is counted).
 */
int kvm_rp_session_open(kvm_rp_t *rp, const kvm_rp_session_info_t *info, uint64_t now_us);

/**
 * Drop a session. When it owned control this performs the revoke + RELEASE_ALL path with the
 * reason mapped by kvm_rp_release_reason(). Safe to call for an already closed session.
 */
void kvm_rp_session_close(kvm_rp_t *rp, uint64_t session_id, kvm_rp_revoke_reason_t reason,
                          uint64_t now_us);

/**
 * Validate and execute one client frame. `frame` need not be NUL-terminated; it must stay valid
 * for the duration of the call. Returns KVM_RP_OK or the code that was reported to the client.
 * No HID intent ever leaves this call for a frame that is not fully valid and authorized.
 */
kvm_rp_err_t kvm_rp_on_frame(kvm_rp_t *rp, uint64_t session_id, const char *frame, size_t len,
                             uint64_t now_us);

/** Timers: approval timeout, heartbeat staleness, coalesced-move flush. Call at least every 50 ms. */
void kvm_rp_poll(kvm_rp_t *rp, uint64_t now_us);

/** Authoritative HOST/device state (FR-401, FR-403, INV-005). */
void kvm_rp_set_host(kvm_rp_t *rp, kvm_host_usb_t usb, bool maintenance, bool wifi_connected,
                     uint64_t now_us);

/** Authoritative ownership mirror from the arbiter (03 §4.1). Call after every epoch change. */
void kvm_rp_set_owner(kvm_rp_t *rp, kvm_owner_kind_t kind, uint64_t session_id, uint32_t epoch,
                      uint64_t now_us);

/**
 * Tell a client that its lease is gone (03 §6.4 `control.revoked`). Used for the paths the arbiter
 * decides on its own: local override, USB loss, maintenance, client removed from the registry.
 */
void kvm_rp_notify_revoked(kvm_rp_t *rp, uint64_t session_id, kvm_rp_revoke_reason_t reason,
                           uint32_t new_epoch, uint64_t now_us);

/** The UI answered a pending physical approval (PROC-006 step 5). */
void kvm_rp_approval_result(kvm_rp_t *rp, uint64_t session_id, bool granted, uint64_t now_us);

const kvm_rp_counters_t *kvm_rp_counters(const kvm_rp_t *rp);

/** Slot of an open session, or -1. */
int kvm_rp_slot_of(const kvm_rp_t *rp, uint64_t session_id);

/** Wire label assigned in `hello.ack`, or "" before that. Never a secret (03 §6.2). */
const char *kvm_rp_session_label(const kvm_rp_t *rp, uint64_t session_id);

#ifdef __cplusplus
}
#endif
