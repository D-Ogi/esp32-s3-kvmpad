// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Constants and result codes shared by every part of the pairing core (TASK-100, ADR-022/ADR-029).
// The timing constants are compile-time, not settings: FR-722 makes the 180 s cap a hard limit, and
// ADR-023 leaves exactly one pairing knob configurable -- `pair.require_accept`.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- protocol identity (ADR-029) ---

// Value of the `pake` field in `pair.hello`. A client that sends anything else is refused.
#define KVM_PAIR_PAKE_NAME "ecjpake-p256-sha256"

// Wire version of the pairing exchange, carried in `pair.hello` as `v` and folded into every
// derivation label. Bumping it invalidates all cross-version transcripts by construction.
#define KVM_PAIR_WIRE_VERSION 1

// --- sizes ---

#define KVM_PAIR_CODE_LEN 6u            // decimal digits typed in by the user (FR-721)
#define KVM_PAIR_CODE_CAP (KVM_PAIR_CODE_LEN + 1u)
#define KVM_PAIR_SHA256_LEN 32u
#define KVM_PAIR_TOKEN_LEN 32u          // session token, ADR-022 step 6
#define KVM_PAIR_CLIENT_ID_LEN 16u      // stable per-installation CLIENT identifier (SEC-102)
#define KVM_PAIR_NAME_CAP 24u           // matches KVM_CONFIG_PAIR_NAME_CAP, NUL included
#define KVM_PAIR_CONFIRM_LEN 32u        // HMAC-SHA256 key-confirmation tag
#define KVM_PAIR_AEAD_NONCE_LEN 12u
#define KVM_PAIR_AEAD_TAG_LEN 16u
#define KVM_PAIR_TOKEN_SEALED_LEN (KVM_PAIR_TOKEN_LEN + KVM_PAIR_AEAD_TAG_LEN) // 48

// Round blobs: six length-prefixed chunks in round 1, three in round 2 (ADR-029 §4).
#define KVM_PAIR_ROUND1_MAX 330u
#define KVM_PAIR_ROUND2_MAX 165u

// --- timing (FR-722, FR-725, SEC-104); all milliseconds ---

#define KVM_PAIR_CODE_TTL_MS 120000u      // code validity
#define KVM_PAIR_WINDOW_MAX_MS 180000u    // hard cap on the whole window, Accept screen included
#define KVM_PAIR_ACCEPT_TIMEOUT_MS 30000u // Accept/Reject decision time
#define KVM_PAIR_TOAST_MS 10000u          // require_accept OFF: how long the toast stays up (FR-725)
#define KVM_PAIR_MAX_FAILURES 3u          // failed runs inside one window before it closes
#define KVM_PAIR_MAX_CLOSURES 3u          // such closures inside the observation period
#define KVM_PAIR_CLOSURE_WINDOW_MS 600000u // observation period for the closures (10 min)
#define KVM_PAIR_LOCKOUT_MS 600000u        // lockout after too many closures (10 min)

// --- result codes ---
//
// The names map one-to-one onto the wire errors of ADR-029 §5; `ERR_PAIRING_EXPIRED` (03 §7) is the
// only code a refused client ever sees for a closed, expired or locked-out window, so a probe
// cannot tell those three apart.
typedef enum {
    KVM_PAIR_OK = 0,
    KVM_PAIR_ERR_ARG,          // malformed call from the integrator, not from the wire
    KVM_PAIR_ERR_STATE,        // message arrived in a state that does not accept it
    KVM_PAIR_ERR_FRAME,        // malformed or over-long blob from the wire
    KVM_PAIR_ERR_EXPIRED,      // no open window, code expired, or the 180 s cap was reached
    KVM_PAIR_ERR_LOCKED,       // lockout active (SEC-104)
    KVM_PAIR_ERR_BAD_CODE,     // key confirmation failed: wrong code or an active MITM
    KVM_PAIR_ERR_BINDING,      // certificate hash or client identifier mismatch (SEC-102)
    KVM_PAIR_ERR_BUSY,         // another run already holds the window
    KVM_PAIR_ERR_FULL,         // paired-client register full
    KVM_PAIR_ERR_REJECTED,     // LOCAL rejected, or the Accept screen timed out
    KVM_PAIR_ERR_PRECONDITION, // remote disabled or Wi-Fi not connected (SEC-106)
    KVM_PAIR_ERR_CRYPTO        // PSA failure or RNG failure; always fail closed
} kvm_pair_err_t;

// Human-readable name of a result code. Safe to log: carries no secret.
const char *kvm_pair_err_name(kvm_pair_err_t err);

// Wire error code of ADR-029 §5 for a result code, e.g. "ERR_PAIRING_EXPIRED". Returns NULL when
// the result is KVM_PAIR_OK.
const char *kvm_pair_err_wire_code(kvm_pair_err_t err);

#ifdef __cplusplus
}
#endif
