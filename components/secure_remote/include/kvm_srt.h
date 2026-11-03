// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Secure remote transport session glue (TASK-100; FR-721..727, SEC-101..107, TECH-040..046,
// ADR-021/022/023/028/029/030/032).
//
// What this layer is:
//   - the one owner of the two WebSocket endpoints of ADR-028 §2: `/v1/ws` (control, subprotocol
//     `kvmpad.v1`, `Authorization: Bearer <token>`) and `/v1/pair` (pairing, subprotocol
//     `kvmpad.pair.v1`, no token, open only inside a pairing window);
//   - admission control: the token is decoded and verified against the paired register before a
//     connection exists at all, with the attempt limit and cooldown of TECH-042;
//   - the glue that turns admitted connections into `remote_protocol` sessions, so validated input
//     leaves as typed `kvm_hid_intent_t` through one hook the integrator forwards to hid_dispatch
//     -> InputArbiter -> HidScheduler (03 INV-001, AGENTS.md prohibition 2);
//   - the router for `pair.*` frames into the pairing core, with its own envelope codec and its own
//     server-to-client `seq`, so the pairing endpoint holds no session slot, no owner mirror and no
//     intent sink (SEC-103);
//   - the place where every loss -- disconnect, TLS error, timeout, protocol abuse, revoked client,
//     Remote switched off, maintenance, shutdown -- ends in a RELEASE_ALL request with the reason
//     that belongs to it (AGENTS.md prohibition 3, TECH-005).
//
// What this layer is NOT and never becomes:
//   - a HID emitter. It never includes tusb.h / tinyusb.h and never touches the pressed-ledger.
//   - the source of truth for HOST_ACTIVE, ownership, the epoch or Caps Lock. Those arrive through
//     kvm_srt_set_host() / kvm_srt_set_owner() from the authoritative hid_core snapshot.
//   - a TLS or WebSocket implementation. Those are the ESP-IDF adapters in port/ (kvm_srt_server.h),
//     which hand over complete text frames and report every loss.
//   - a settings owner. `pair.require_accept` and `control.approval` are read from config_store and
//     pushed in with kvm_srt_set_policy(); no CLIENT message can change them (SEC-107).
//   - a logger. Counters only: no payload, usage id, client name, token or pairing code is logged or
//     echoed in an error payload (TECH-046, SEC-105).
//
// Pure C11, no ESP-IDF dependency, no allocation on any path, monotonic time injected by the
// caller. Host unit tests in test/secure_remote.
//
// Threading: not reentrant and not thread-safe. Every entry point must run in one context -- in the
// firmware that is the HTTP server task, which the adapter also uses for the tick through
// httpd_queue_work(). No hook may call back into kvm_srt_* on the same instance.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_core.h"
#include "kvm_config.h" // kvm_control_approval_t (FR-733); header-only use
#include "kvm_hid_intent.h"
#include "kvm_pairing.h"
#include "kvm_srt_auth.h"
#include "kvm_srt_pair_json.h"
#include "remote_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Control connections (one per `remote_protocol` session) plus the single pairing connection. */
#define KVM_SRT_MAX_CONTROL KVM_RP_MAX_SESSIONS
#define KVM_SRT_MAX_CONNS (KVM_SRT_MAX_CONTROL + 1)

/** TECH-042 defaults: consecutive failed admissions before a cooldown, and its length. */
#define KVM_SRT_AUTH_FAIL_MAX 5u
#define KVM_SRT_AUTH_COOLDOWN_US 30000000u

/** `control.request` reason as it reaches the UI; sized to the protocol field. */
#define KVM_SRT_REASON_CAP KVM_RP_REASON_MAX

typedef enum {
    KVM_SRT_CONN_CONTROL = 0,
    KVM_SRT_CONN_PAIRING = 1,
    KVM_SRT_CONN_KINDS = 2,
} kvm_srt_conn_kind_t;

// ---- loss paths ----------------------------------------------------------------------------
//
// Every way a remote session can end. There is deliberately no "unknown" value: the adapter has to
// name the cause, because the cause selects the RELEASE_ALL reason the HOST sees in diagnostics.
//
// A local mode switch or view change is NOT in this list. Local input overrides a remote lease
// through the arbiter (ADR-011, LOCAL > REMOTE), which raises its own RELEASE_ALL and reports back
// through kvm_srt_notify_revoked(KVM_RP_REVOKE_LOCAL_OVERRIDE); it does not drop the connection.
typedef enum {
    KVM_SRT_LOSS_CLIENT_CLOSE = 0, // clean WebSocket close, or `session.close`
    KVM_SRT_LOSS_SOCKET,           // socket error, reset, LRU purge
    KVM_SRT_LOSS_TLS,              // TLS alert or handshake failure after admission
    KVM_SRT_LOSS_TIMEOUT,          // idle / heartbeat deadline
    KVM_SRT_LOSS_PROTOCOL,         // frame abuse past the bad-frame budget
    KVM_SRT_LOSS_UNPAIRED,         // the client was removed from the register (FR-726)
    KVM_SRT_LOSS_REMOTE_DISABLED,  // Settings -> Remote switched off locally
    KVM_SRT_LOSS_MAINTENANCE,      // OTA / factory reset (PROC-009)
    KVM_SRT_LOSS_SHUTDOWN,         // the server is stopping
    KVM_SRT_LOSS_COUNT,
} kvm_srt_loss_t;

/** Name of a loss path. Safe to log. */
const char *kvm_srt_loss_name(kvm_srt_loss_t loss);

/** The protocol revoke reason a loss maps to (03 §6.4 `control.revoked.reason`). */
kvm_rp_revoke_reason_t kvm_srt_loss_revoke_reason(kvm_srt_loss_t loss);

/** The hid_core RELEASE_ALL reason a loss maps to (03 §4.2, TECH-005). */
kvm_release_reason_t kvm_srt_loss_release_reason(kvm_srt_loss_t loss);

// ---- admission -----------------------------------------------------------------------------

typedef enum {
    KVM_SRT_ADMIT_OK = 0,
    KVM_SRT_ADMIT_UNAUTHORIZED, // -> HTTP 401, no body detail (ADR-028 §2)
    KVM_SRT_ADMIT_FORBIDDEN,    // -> HTTP 403: Remote off, maintenance, pairing window closed
    KVM_SRT_ADMIT_BUSY,         // -> HTTP 503: no free connection slot
} kvm_srt_admit_t;

const char *kvm_srt_admit_name(kvm_srt_admit_t admit);

/** What an accepted upgrade request carries into kvm_srt_conn_open(). */
typedef struct {
    kvm_srt_conn_kind_t kind;
    bool operator_role; // true for a paired client; observers (FR-404) are a later role
    int16_t client_index;   // index in the paired register, -1 on the pairing endpoint
    uint32_t label_entropy; // randomness for the non-secret wire label
} kvm_srt_admission_t;

// ---- physical approval (ADR-023 FR-733) ----------------------------------------------------

typedef enum {
    KVM_SRT_APPROVAL_PENDING = 0, // the UI is asking; answer with kvm_srt_approval_result()
    KVM_SRT_APPROVAL_DENIED,      // the UI refuses immediately
} kvm_srt_approval_t;

// ---- integrator hooks ----------------------------------------------------------------------
//
// Every hook runs synchronously in the caller's context and must not block. None may call back
// into kvm_srt_* on the same instance.
typedef struct {
    /**
     * Forward one validated intent with origin = KVM_ORIGIN_REMOTE, the given session id and lease
     * epoch, and return the arbiter result. Anything but KVM_HID_OK means no HID left the arbiter;
     * the client is answered with the mapped protocol code (kvm_rp_err_from_hid()).
     */
    kvm_hid_err_t (*hid_intent)(void *ctx, uint64_t session_id, uint32_t lease_epoch,
                                const kvm_hid_intent_t *intent);

    /** 03 §4.3 on_control_request: is a lease admissible now (LOCAL idle, no other lease)? */
    kvm_hid_err_t (*control_request)(void *ctx, uint64_t session_id);

    /** After approval: new epoch + RELEASE_ALL + owner = REMOTE. Writes the granted lease epoch. */
    kvm_hid_err_t (*control_grant)(void *ctx, uint64_t session_id, uint32_t *lease_epoch);

    /**
     * Revoke + RELEASE_ALL(reason) + epoch bump for this session (03 §4.2 INV-002). Must write the
     * epoch in force afterwards to `*new_epoch`.
     */
    void (*control_release)(void *ctx, uint64_t session_id, kvm_release_reason_t reason,
                            uint32_t *new_epoch);

    /** 03 §4.3 on_heartbeat: renew the lease of the owning session. */
    void (*heartbeat)(void *ctx, uint64_t session_id);

    /**
     * Unconditional RELEASE_ALL, independent of any session. Used on the broad loss paths
     * (Remote switched off, maintenance, shutdown, every client unpaired) when REMOTE still owns
     * the HOST after the sessions are gone, so no key can stay down with nobody to release it.
     */
    void (*release_all)(void *ctx, kvm_release_reason_t reason);

    /** Send one complete text frame on this connection. `frame` is NUL-terminated, `len` bytes. */
    void (*send)(void *ctx, uint64_t conn_id, kvm_srt_conn_kind_t kind, const char *frame,
                 size_t len);

    /**
     * Ask the transport to drop this connection; the last answer has already been sent. The
     * transport must then call kvm_srt_conn_close() for it (directly or once the socket is really
     * gone), otherwise the slot stays occupied until it does.
     */
    void (*close)(void *ctx, uint64_t conn_id, kvm_srt_conn_kind_t kind);

    /** Fill the read-only snapshot of 03 §6.6. No token, no password, no policy value (SEC-107). */
    void (*status_fill)(void *ctx, kvm_rp_status_t *out);

    /**
     * Start the physical approval prompt for a control request (PROC-006 step 5, FR-733 `always`
     * and the untrusted half of `trusted`). `client_name` and `reason` are sanitized and
     * NUL-terminated. Returning KVM_SRT_APPROVAL_PENDING arms the 30 s timer of 03 §6.5.
     */
    kvm_srt_approval_t (*approval_begin)(void *ctx, uint64_t session_id, const char *client_name,
                                         const char *reason);

    /** Withdraw a pending prompt (timeout, connection gone, local override). */
    void (*approval_cancel)(void *ctx, uint64_t session_id);

    /** FR-733 `trusted`: is this register entry marked trusted? NULL hook reads as "no". */
    bool (*client_trusted)(void *ctx, int client_index);

    /**
     * One pairing event for the UI to render: window opened/closed, show the Accept screen, the
     * paired toast, a failed attempt, a lockout warning (FR-722, FR-725, SEC-104). Callbacks only;
     * no LVGL call happens here.
     */
    void (*pair_event)(void *ctx, const kvm_pair_event_t *event);

    void *ctx;
} kvm_srt_hooks_t;

// ---- configuration -------------------------------------------------------------------------

typedef struct {
    /** NULL = KVM_RP_CONFIG_DEFAULT. */
    const kvm_rp_config_t *rp;

    /** Randomness, monotonic milliseconds and wall clock for the pairing core. */
    kvm_pair_platform_t platform;

    /** Paired-client register; the integrator maps it onto config_store (FR-726, SEC-105). */
    kvm_pair_store_t store;

    /** Advertised DEVICE name, e.g. "KVMpad-7F3A" (FR-723). */
    char device_name[KVM_PAIR_NAME_CAP];

    /** SHA-256 of the DER DEVICE TLS certificate: the CLIENT's pin and the binding of SEC-102. */
    uint8_t cert_sha256[KVM_PAIR_SHA256_LEN];

    /** FR-732 `pair.require_accept`, ON by default. */
    bool pair_require_accept;

    /** FR-733 `control.approval`; KVM_CONTROL_APPROVAL_ALWAYS is the default. */
    kvm_control_approval_t control_approval;

    /** TECH-042; 0 selects KVM_SRT_AUTH_FAIL_MAX / KVM_SRT_AUTH_COOLDOWN_US. */
    uint32_t auth_fail_max;
    uint64_t auth_cooldown_us;
} kvm_srt_config_t;

// ---- counters (TECH-061, SEC-104; aggregated, never a payload) ------------------------------

typedef struct {
    uint32_t admit_ok[KVM_SRT_CONN_KINDS];
    uint32_t admit_unauthorized;
    uint32_t admit_forbidden;
    uint32_t admit_busy;
    uint32_t auth_absent;
    uint32_t auth_bad_scheme;
    uint32_t auth_bad_encoding;
    uint32_t auth_unknown_token;
    uint32_t auth_cooldowns;   // cooldowns started (TECH-042)
    uint32_t auth_refused_cold; // admissions refused while a cooldown was running
    uint32_t conns_opened[KVM_SRT_CONN_KINDS];
    uint32_t conns_closed[KVM_SRT_CONN_KINDS];
    uint32_t conns_rejected;
    uint32_t losses[KVM_SRT_LOSS_COUNT];
    uint32_t release_all_requests; // unconditional RELEASE_ALL asked for on a broad loss
    uint32_t pair_frames_in;
    uint32_t pair_frames_out;
    uint32_t pair_frames_rejected;
    uint32_t pair_out_dropped; // a pairing reply did not fit 1024 B: dropped, never half-sent
    uint32_t pair_events;
    uint32_t approvals_auto;    // granted without a prompt (FR-733 `auto`)
    uint32_t approvals_trusted; // granted because the client is trusted (FR-733 `trusted`)
    uint32_t approvals_prompted;
    uint32_t clients_revoked; // sessions dropped because their register entry went (FR-726)
} kvm_srt_counters_t;

// ---- state (exposed for static allocation only; use the API) -------------------------------

typedef struct {
    uint8_t state; // 0 = free, 1 = open
    uint8_t kind;  // kvm_srt_conn_kind_t
    bool pair_hello_seen;
    uint64_t id;
    int16_t client_index;
    uint64_t opened_us;
    // pairing endpoint only: its own replay guard and its own server-to-client counter
    uint64_t last_seq;
    bool have_seq;
    uint32_t out_seq;
    uint32_t bad_frames;
    char label[KVM_SRT_PAIR_LABEL_CAP];
} kvm_srt_conn_t;

typedef struct {
    kvm_srt_config_t cfg;
    kvm_srt_hooks_t hooks;

    kvm_rp_t rp;
    kvm_pair_session_t pair;
    bool pair_ready;

    kvm_srt_conn_t conn[KVM_SRT_MAX_CONNS];

    bool remote_enabled;
    bool wifi_connected;
    bool maintenance;
    kvm_host_usb_t host_usb;
    kvm_owner_kind_t owner_kind;
    uint64_t owner_session;
    uint32_t owner_epoch;

    uint32_t auth_fails;
    uint64_t auth_cooldown_until_us;
    uint64_t now_us;

    kvm_json_doc_t pdoc;              // pairing-frame document; one per instance, never per frame
    char pout[KVM_JSON_MAX_FRAME + 1]; // pairing serializer buffer
    kvm_srt_counters_t c;
} kvm_srt_t;

// ---- lifecycle -----------------------------------------------------------------------------

/**
 * Initialize the instance. Copies `cfg` and `hooks`, initializes the `remote_protocol` session
 * layer and the pairing core (which initializes PSA crypto on the first call in the process).
 *
 * Returns KVM_PAIR_OK, or KVM_PAIR_ERR_ARG when a required hook or platform entry is missing, or
 * KVM_PAIR_ERR_CRYPTO when PSA could not be initialized. On anything but KVM_PAIR_OK the instance
 * refuses every admission, so a crypto failure fails closed instead of running without pairing.
 */
kvm_pair_err_t kvm_srt_init(kvm_srt_t *srt, const kvm_srt_config_t *cfg,
                            const kvm_srt_hooks_t *hooks);

/** Drop every connection, close any pairing window and zeroize the pairing core. */
void kvm_srt_shutdown(kvm_srt_t *srt, uint64_t now_us);

// ---- authoritative state in (never decided here) -------------------------------------------

/** FR-401, FR-403, INV-005: USB state, maintenance mode and Wi-Fi from the domain snapshot. */
void kvm_srt_set_host(kvm_srt_t *srt, kvm_host_usb_t usb, bool maintenance, bool wifi_connected,
                      uint64_t now_us);

/** TECH-040 / FR-506: Settings -> Remote. Switching it off drops every control connection. */
void kvm_srt_set_remote_enabled(kvm_srt_t *srt, bool enabled, uint64_t now_us);

/** 03 §4.1 ownership mirror from the arbiter. Call after every epoch change. */
void kvm_srt_set_owner(kvm_srt_t *srt, kvm_owner_kind_t kind, uint64_t session_id, uint32_t epoch,
                       uint64_t now_us);

/** ADR-023 policies from config_store. Takes effect on the next window / next request. */
void kvm_srt_set_policy(kvm_srt_t *srt, bool pair_require_accept,
                        kvm_control_approval_t control_approval);

/** The DEVICE identity the pairing binding and the mDNS record use (SEC-102, FR-723). */
void kvm_srt_set_identity(kvm_srt_t *srt, const uint8_t cert_sha256[KVM_PAIR_SHA256_LEN],
                          const char *device_name);

/**
 * Tell a client its lease is gone for a reason the arbiter decided on its own: local override, USB
 * loss, maintenance (03 §6.4 `control.revoked`). The connection stays open.
 */
void kvm_srt_notify_revoked(kvm_srt_t *srt, uint64_t session_id, kvm_rp_revoke_reason_t reason,
                            uint32_t new_epoch, uint64_t now_us);

// ---- connections ---------------------------------------------------------------------------

/**
 * Decide on an upgrade request before any WebSocket state exists. `authorization` is the
 * `Authorization` header value, or NULL; it is ignored on the pairing endpoint, which has no token
 * yet (ADR-028 §2).
 *
 * On KVM_SRT_ADMIT_OK, `out` is filled and must be passed unchanged to kvm_srt_conn_open(). On
 * anything else nothing is reserved and `out` is zeroized. The decoded token is zeroized before
 * returning in every path.
 */
kvm_srt_admit_t kvm_srt_admit(kvm_srt_t *srt, kvm_srt_conn_kind_t kind, const char *authorization,
                              uint64_t now_us, kvm_srt_admission_t *out);

/**
 * Register an admitted connection. `conn_id` must be non-zero and unique among open connections;
 * derive it from a monotonic counter, not from a socket descriptor, so a reused descriptor cannot
 * collide with a connection that is still closing. Returns the slot index, or -1.
 */
int kvm_srt_conn_open(kvm_srt_t *srt, uint64_t conn_id, const kvm_srt_admission_t *adm,
                      uint64_t now_us);

/** One complete text frame. `frame` need not be NUL-terminated and must stay valid for the call. */
void kvm_srt_conn_frame(kvm_srt_t *srt, uint64_t conn_id, const char *frame, size_t len,
                        uint64_t now_us);

/**
 * The connection is gone. Performs the revoke + RELEASE_ALL path with the reason that belongs to
 * `loss` when it owned control. Safe to call for an unknown or already closed connection.
 */
void kvm_srt_conn_close(kvm_srt_t *srt, uint64_t conn_id, kvm_srt_loss_t loss, uint64_t now_us);

/** Drop every control connection (the pairing one too when `include_pairing`). */
void kvm_srt_drop_all(kvm_srt_t *srt, kvm_srt_loss_t loss, bool include_pairing, uint64_t now_us);

/** Timers: approval timeout, heartbeat staleness, coalesced moves, pairing deadlines and events. */
void kvm_srt_tick(kvm_srt_t *srt, uint64_t now_us);

/** The UI answered a pending control approval (PROC-006 step 5). */
void kvm_srt_approval_result(kvm_srt_t *srt, uint64_t session_id, bool granted, uint64_t now_us);

/** Slot of an open connection, or -1. */
int kvm_srt_slot_of(const kvm_srt_t *srt, uint64_t conn_id);

/** Kind of an open connection, or KVM_SRT_CONN_KINDS when it is not open. */
kvm_srt_conn_kind_t kvm_srt_conn_kind(const kvm_srt_t *srt, uint64_t conn_id);

// ---- pairing, driven by the local UI -------------------------------------------------------

/**
 * Open a pairing window and draw a code (FR-722). The preconditions of SEC-106 (Remote ON and
 * Wi-Fi connected) are taken from the state last pushed in, not from the caller.
 */
kvm_pair_err_t kvm_srt_pair_open(kvm_srt_t *srt, uint64_t now_us);

/** Accept / Reject on the Accept screen, and the always-available LOCAL cancel (ADR-011). */
kvm_pair_err_t kvm_srt_pair_accept(kvm_srt_t *srt, uint64_t now_us);
kvm_pair_err_t kvm_srt_pair_reject(kvm_srt_t *srt, uint64_t now_us);
kvm_pair_err_t kvm_srt_pair_cancel(kvm_srt_t *srt, uint64_t now_us);

/** The 6 ASCII digits to show, or NULL. The UI renders them `ddd ddd` and never logs them. */
const char *kvm_srt_pair_code(const kvm_srt_t *srt);

uint32_t kvm_srt_pair_code_remaining_ms(const kvm_srt_t *srt);
uint32_t kvm_srt_pair_window_remaining_ms(const kvm_srt_t *srt);

/** SEC-103 / FR-723: true while `/v1/pair` accepts anything and `pair=1` is advertised. */
bool kvm_srt_pair_endpoint_open(const kvm_srt_t *srt);

const kvm_pair_counters_t *kvm_srt_pair_counters(const kvm_srt_t *srt);

// ---- paired clients (FR-726) ---------------------------------------------------------------

uint8_t kvm_srt_clients_count(const kvm_srt_t *srt);
bool kvm_srt_clients_get(const kvm_srt_t *srt, uint8_t index, kvm_pair_record_t *out);

/**
 * Remove one client and immediately revoke it: every control connection that authenticated as that
 * register entry is dropped with KVM_SRT_LOSS_UNPAIRED, which raises RELEASE_ALL when it owned the
 * HOST (FR-726). The register compacts on removal, so the cached index of every surviving
 * connection above `index` is shifted down by one here.
 */
bool kvm_srt_clients_remove(kvm_srt_t *srt, uint8_t index, uint64_t now_us);

/** Remove every client; returns how many went, and drops every control connection. */
uint8_t kvm_srt_clients_remove_all(kvm_srt_t *srt, uint64_t now_us);

// ---- diagnostics ---------------------------------------------------------------------------

const kvm_srt_counters_t *kvm_srt_counters(const kvm_srt_t *srt);
const kvm_rp_counters_t *kvm_srt_protocol_counters(const kvm_srt_t *srt);

#ifdef __cplusplus
}
#endif
