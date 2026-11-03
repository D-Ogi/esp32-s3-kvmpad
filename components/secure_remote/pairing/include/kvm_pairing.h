// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pairing window state machine and paired-client register (TASK-100; FR-721..727, SEC-101..106,
// ADR-022, ADR-023, ADR-029).
//
// The DEVICE side is a pure state machine: it never reads a clock, a socket or flash by itself. The
// transport component feeds it decoded `pair.*` payloads and calls kvm_pair_tick() regularly; the UI
// calls kvm_pair_window_open(), kvm_pair_local_accept(), kvm_pair_local_reject() and
// kvm_pair_local_cancel(). Every call returns the message to send, if any, in `kvm_pair_out_t`.
//
// What the state machine guarantees:
//   * one window at a time, one code per window, one run at a time (FR-722, KVM_PAIR_ERR_BUSY);
//   * the code dies at KVM_PAIR_CODE_TTL_MS and the whole window at KVM_PAIR_WINDOW_MAX_MS, counted
//     from the tap on Pair client -- the Accept screen cannot push past the cap (FR-722);
//   * the Accept screen times out at KVM_PAIR_ACCEPT_TIMEOUT_MS into a reject (FR-725);
//   * KVM_PAIR_MAX_FAILURES failed runs close the window and wipe the code; KVM_PAIR_MAX_CLOSURES
//     such closures inside KVM_PAIR_CLOSURE_WINDOW_MS start a KVM_PAIR_LOCKOUT_MS lockout and raise
//     a warning event (SEC-104);
//   * LOCAL cancels at any point (ADR-011) and the code is zeroized whenever the window closes
//     (SEC-105);
//   * nothing is stored unless key confirmation passed and -- with `require_accept` ON -- LOCAL
//     accepted (SEC-102, FR-725);
//   * the code, the derived keys and the token never leave through this interface. Only
//     kvm_pair_out_t carries the sealed token, and only on the accepted path.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing_pake.h"
#include "kvm_pairing_platform.h"
#include "kvm_pairing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- state ---

typedef enum {
    KVM_PAIR_STATE_IDLE = 0,    // no window; the pairing endpoint is closed
    KVM_PAIR_STATE_OPEN,        // window open, code on screen, no run in progress
    KVM_PAIR_STATE_RUNNING,     // a PAKE run holds the window
    KVM_PAIR_STATE_AWAIT_ACCEPT,// key confirmation passed, Accept/Reject on screen (FR-725)
    KVM_PAIR_STATE_COMPLETED,   // client stored, token handed out; window closing
    KVM_PAIR_STATE_LOCKED       // lockout active (SEC-104)
} kvm_pair_state_t;

// Diagnostics counters (SEC-104). Exported through `diagnostics`; never reset by the core.
typedef struct {
    uint32_t pair_attempts;  // PAKE runs started
    uint32_t pair_failures;  // runs that ended in a failure
    uint32_t pair_lockouts;  // lockouts started
    uint32_t pair_successes; // clients stored
    uint32_t windows_opened;
    uint32_t windows_closed_on_failures; // closures caused by KVM_PAIR_MAX_FAILURES
} kvm_pair_counters_t;

// Preconditions of SEC-106, read from the domain snapshot at the moment the window is opened.
typedef struct {
    bool remote_enabled;  // Settings -> Remote is on
    bool wifi_connected;  // wifi:CONNECTED (TECH-040)
} kvm_pair_preconditions_t;

// The one configurable pairing policy (ADR-023). Everything else in ADR-022 is a hard invariant.
typedef struct {
    bool require_accept;      // `pair.require_accept`, ON by default
    char device_name[KVM_PAIR_NAME_CAP]; // advertised name, e.g. "KVMpad-7F3A"
    uint8_t cert_sha256[KVM_PAIR_SHA256_LEN]; // SHA-256 of the DER DEVICE TLS certificate
} kvm_pair_config_t;

// Why a window closed, for the local message and for the `pair.rejected` reason.
typedef enum {
    KVM_PAIR_CLOSE_NONE = 0,
    KVM_PAIR_CLOSE_COMPLETED,
    KVM_PAIR_CLOSE_CODE_EXPIRED,
    KVM_PAIR_CLOSE_WINDOW_CAP,
    KVM_PAIR_CLOSE_LOCAL_CANCEL,
    KVM_PAIR_CLOSE_LOCAL_REJECT,
    KVM_PAIR_CLOSE_ACCEPT_TIMEOUT,
    KVM_PAIR_CLOSE_TOO_MANY_FAILURES,
    KVM_PAIR_CLOSE_LOCKED,
    KVM_PAIR_CLOSE_INTERNAL_ERROR // RNG, PSA or storage failure; always fail closed
} kvm_pair_close_reason_t;

// --- outbound messages ---

typedef enum {
    KVM_PAIR_OUT_NONE = 0,
    KVM_PAIR_OUT_ROUND1,  // `pair.round1`, DEVICE blob + device_name
    KVM_PAIR_OUT_ROUND2,  // `pair.round2`, DEVICE blob
    KVM_PAIR_OUT_CONFIRM, // `pair.confirm`, DEVICE key-confirmation tag
    KVM_PAIR_OUT_ACCEPTED, // `pair.accepted`, sealed token + device_name
    // `pair.confirm` immediately followed by `pair.accepted`: the require_accept OFF path of
    // ADR-023, where there is no screen to wait for. `blob` holds the tag, `token` the sealed token.
    KVM_PAIR_OUT_CONFIRM_ACCEPTED,
    KVM_PAIR_OUT_REJECTED, // `pair.rejected`, reason
    KVM_PAIR_OUT_ERROR     // `error`, wire code from kvm_pair_err_wire_code()
} kvm_pair_out_kind_t;

// One message -- or, for KVM_PAIR_OUT_CONFIRM_ACCEPTED, two -- to send. `blob`/`blob_len` hold a
// round blob or a confirmation tag, `token`/`token_len` a sealed token; the transport base64-encodes
// them. `reason` is the `pair.rejected` reason string and `wire_code` the `error` payload code; both
// are static strings or NULL.
typedef struct {
    kvm_pair_out_kind_t kind;
    uint8_t blob[KVM_PAIR_ROUND1_MAX];
    size_t blob_len;
    uint8_t token[KVM_PAIR_TOKEN_SEALED_LEN];
    size_t token_len;
    const char *reason;
    const char *wire_code;
    bool close_after; // the transport must close the pairing connection after sending
} kvm_pair_out_t;

// --- local events the UI has to render ---

typedef enum {
    KVM_PAIR_EVENT_NONE = 0,
    KVM_PAIR_EVENT_WINDOW_OPENED,
    KVM_PAIR_EVENT_WINDOW_CLOSED,
    KVM_PAIR_EVENT_ACCEPT_SCREEN,   // show "Pair with <name>?" (FR-725)
    KVM_PAIR_EVENT_PAIRED_TOAST,    // require_accept OFF: toast with name + Remove (FR-725)
    KVM_PAIR_EVENT_FAILED_ATTEMPT,
    KVM_PAIR_EVENT_LOCKOUT          // "possible intruder on LAN" warning (SEC-104)
} kvm_pair_event_kind_t;

typedef struct {
    kvm_pair_event_kind_t kind;
    kvm_pair_close_reason_t reason;
    char client_name[KVM_PAIR_NAME_CAP]; // filled for the Accept screen and the toast
    uint32_t remaining_ms;               // filled for the Accept screen and the toast
} kvm_pair_event_t;

// --- the `pair.hello` payload, already parsed by the transport ---

typedef struct {
    uint32_t v;                            // must equal KVM_PAIR_WIRE_VERSION
    const char *pake;                      // must equal KVM_PAIR_PAKE_NAME
    char client_name[KVM_PAIR_NAME_CAP];   // FR-724, the Windows host name
    uint8_t client_id[KVM_PAIR_CLIENT_ID_LEN];
    uint8_t cert_sha256[KVM_PAIR_SHA256_LEN]; // what the CLIENT saw in the handshake (SEC-102)
} kvm_pair_hello_t;

// --- the session ---

typedef struct {
    kvm_pair_config_t cfg;
    kvm_pair_platform_t platform;
    kvm_pair_store_t store;

    kvm_pair_state_t state;
    kvm_pair_counters_t counters;

    char code[KVM_PAIR_CODE_CAP];
    uint64_t window_opened_ms;
    uint64_t code_issued_ms;
    uint64_t accept_screen_ms;
    uint64_t completed_ms;
    uint64_t lockout_until_ms;
    bool code_used;

    uint8_t failures;                                 // failed runs in this window
    uint64_t closures[KVM_PAIR_MAX_CLOSURES];         // ring of recent failure-driven closures
    uint8_t closure_count;
    uint8_t closure_next;

    kvm_pake_t pake;
    bool pake_live;
    char peer_name[KVM_PAIR_NAME_CAP];
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    bool token_ready;

    kvm_pair_close_reason_t last_close;
    kvm_pair_event_t event; // the most recent event; read with kvm_pair_take_event()
    bool event_pending;
} kvm_pair_session_t;

// Set the session up. `cfg`, `platform` and `store` are copied; `platform` must be complete.
// Returns KVM_PAIR_ERR_ARG on a missing hook, and initializes PSA crypto on the first call.
kvm_pair_err_t kvm_pair_session_init(kvm_pair_session_t *s, const kvm_pair_config_t *cfg,
                                     const kvm_pair_platform_t *platform,
                                     const kvm_pair_store_t *store);

// Zeroize the session, destroy any live PAKE run and wipe the code. The paired register is left
// alone; it belongs to config_store.
void kvm_pair_session_free(kvm_pair_session_t *s);

// Apply a changed `pair.require_accept` (ADR-023). Takes effect on the next window, never mid-run.
void kvm_pair_set_require_accept(kvm_pair_session_t *s, bool require_accept);

// --- LOCAL actions ---

// Open a window and draw a code (FR-722). KVM_PAIR_ERR_PRECONDITION when remote is off or Wi-Fi is
// down (SEC-106), KVM_PAIR_ERR_LOCKED during a lockout, KVM_PAIR_ERR_BUSY when a window is already
// open, KVM_PAIR_ERR_CRYPTO when the RNG failed.
kvm_pair_err_t kvm_pair_window_open(kvm_pair_session_t *s, const kvm_pair_preconditions_t *pre);

// The code to show, as 6 ASCII digits, or NULL when no code is valid. The UI renders it `ddd ddd`.
const char *kvm_pair_window_code(const kvm_pair_session_t *s);

// Milliseconds left on the code countdown, and on the whole window. Both 0 outside a window.
uint32_t kvm_pair_code_remaining_ms(const kvm_pair_session_t *s);
uint32_t kvm_pair_window_remaining_ms(const kvm_pair_session_t *s);

// True while the pairing endpoint may accept `pair.*` at all, i.e. `pair=1` in the TXT record
// (SEC-103, FR-723).
bool kvm_pair_endpoint_open(const kvm_pair_session_t *s);

// LOCAL cancel, allowed in every state (ADR-011).
kvm_pair_err_t kvm_pair_local_cancel(kvm_pair_session_t *s, kvm_pair_out_t *out);

// Accept / Reject on the Accept screen (FR-725). Accept stores the client and seals the token.
kvm_pair_err_t kvm_pair_local_accept(kvm_pair_session_t *s, kvm_pair_out_t *out);
kvm_pair_err_t kvm_pair_local_reject(kvm_pair_session_t *s, kvm_pair_out_t *out);

// Drive every deadline: code expiry, the 180 s cap, the Accept timeout, the toast and the end of a
// lockout. Call it at least every 250 ms while a window is open. Returns the message to send, if
// any -- a timed-out Accept produces `pair.rejected`.
kvm_pair_err_t kvm_pair_tick(kvm_pair_session_t *s, kvm_pair_out_t *out);

// --- inbound `pair.*` ---
//
// Each handler returns KVM_PAIR_OK with a filled `out`, or an error code; on an error `out` carries
// the matching wire message and `close_after` says whether the connection has to go. A handler that
// returns KVM_PAIR_ERR_EXPIRED, _LOCKED, _BUSY or _STATE has not touched the run in progress.
// `out` is required here (a NULL `out` returns KVM_PAIR_ERR_ARG and leaves the session untouched).

kvm_pair_err_t kvm_pair_handle_hello(kvm_pair_session_t *s, const kvm_pair_hello_t *hello,
                                     kvm_pair_out_t *out);
kvm_pair_err_t kvm_pair_handle_round1(kvm_pair_session_t *s, const uint8_t *blob, size_t len,
                                      kvm_pair_out_t *out);
kvm_pair_err_t kvm_pair_handle_round2(kvm_pair_session_t *s, const uint8_t *blob, size_t len,
                                      kvm_pair_out_t *out);
kvm_pair_err_t kvm_pair_handle_confirm(kvm_pair_session_t *s, const uint8_t *tag, size_t len,
                                       kvm_pair_out_t *out);

// --- events ---

// Take the pending local event. False when there is none. The core keeps only the newest event, so
// the UI has to drain it on every tick.
bool kvm_pair_take_event(kvm_pair_session_t *s, kvm_pair_event_t *out);

// --- paired-client register (FR-726) ---

uint8_t kvm_pair_clients_count(const kvm_pair_session_t *s);
bool kvm_pair_clients_get(const kvm_pair_session_t *s, uint8_t index, kvm_pair_record_t *out);

// Remove one client. The caller must then revoke that client's session and raise RELEASE_ALL.
bool kvm_pair_clients_remove(kvm_pair_session_t *s, uint8_t index);

// Remove every client; returns how many went. Same revoke + RELEASE_ALL duty for the caller.
uint8_t kvm_pair_clients_remove_all(kvm_pair_session_t *s);

// Find the stored client whose token matches, comparing in constant time. Returns the index, or -1.
// `token` is the 32 bytes the CLIENT presented on a normal session (03 §6.1).
int kvm_pair_clients_verify_token(const kvm_pair_session_t *s,
                                  const uint8_t token[KVM_PAIR_TOKEN_LEN]);

#ifdef __cplusplus
}
#endif
