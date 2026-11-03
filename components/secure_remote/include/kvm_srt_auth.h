// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Token presentation on the WebSocket upgrade request (TASK-100 transport; 03 §6.1, ADR-028 §2,
// TECH-042, SEC-105).
//
// The CLIENT presents the 32-byte pairing token as `Authorization: Bearer <base64url>`, 43
// characters, standard base64url alphabet, no padding. 03 §6.1 forbids a token in a query string,
// and ADR-028 chose the header so the DEVICE can refuse an unauthenticated peer with HTTP 401
// before any WebSocket state machine or JSON parser exists.
//
// This file only decodes the presentation. Verification is a constant-time walk of the paired
// register (`kvm_pair_clients_verify_token`), and the failure counting and cooldown of TECH-042
// live in kvm_srt.
//
// Pure C11, no ESP-IDF dependency, no allocation. Nothing here logs, and no function takes or
// returns a diagnostic string built from the token.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing_types.h" // KVM_PAIR_TOKEN_LEN

#ifdef __cplusplus
extern "C" {
#endif

/** Characters of a 32-byte token in base64url without padding. */
#define KVM_SRT_TOKEN_B64URL_LEN 43u

/** Buffer for the encoded token, NUL included. */
#define KVM_SRT_TOKEN_B64URL_CAP (KVM_SRT_TOKEN_B64URL_LEN + 1u)

/** The `Authorization` scheme of ADR-028 §2, including the single separating space. */
#define KVM_SRT_AUTH_SCHEME "Bearer "

/**
 * `WWW-Authenticate` value on a refused upgrade. It names no realm: a realm would tell an
 * unauthenticated peer which device it reached, and the CLIENT already knows (SEC-105).
 */
#define KVM_SRT_AUTH_SCHEME_CHALLENGE "Bearer"

/** Subprotocol names of ADR-028 §2. The DEVICE echoes the one that belongs to the endpoint. */
#define KVM_SRT_SUBPROTO_CONTROL "kvmpad.v1"
#define KVM_SRT_SUBPROTO_PAIRING "kvmpad.pair.v1"

/** URI paths of ADR-028 §2. There is no `ws://` listener and no third path. */
#define KVM_SRT_URI_CONTROL "/v1/ws"
#define KVM_SRT_URI_PAIRING "/v1/pair"

typedef enum {
    KVM_SRT_AUTH_OK = 0,
    KVM_SRT_AUTH_ERR_ABSENT,   // no header at all
    KVM_SRT_AUTH_ERR_SCHEME,   // not exactly one "Bearer " prefix
    KVM_SRT_AUTH_ERR_ENCODING, // wrong length, bad character, or non-canonical trailing bits
} kvm_srt_auth_err_t;

/**
 * Decode `Authorization: Bearer <token>` into the raw 32 bytes.
 *
 * `header` is the NUL-terminated header value as the server read it, or NULL when the header was
 * absent. Leading spaces and horizontal tabs are skipped (RFC 9110 allows optional whitespace);
 * trailing whitespace is rejected, because the CLIENT emits none and accepting it would let two
 * different header values present the same token.
 *
 * On any error `out` is zeroized. The caller must zeroize `out` after use as well.
 */
kvm_srt_auth_err_t kvm_srt_auth_parse_bearer(const char *header,
                                             uint8_t out[KVM_PAIR_TOKEN_LEN]);

/** Name of an auth result. Safe to log: it describes the shape of the failure, not the token. */
const char *kvm_srt_auth_err_name(kvm_srt_auth_err_t err);

/**
 * base64url (RFC 4648 §5) without padding. `len` is the number of input characters; the input must
 * not contain `=`, whitespace or any character outside the URL-safe alphabet, and the trailing bits
 * of the last character must be zero, so one byte string has exactly one encoding.
 *
 * False on any violation or when the result does not fit `cap`; `*out_len` is then 0.
 */
bool kvm_srt_b64url_decode(const char *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);

/** base64url without padding, NUL-terminated. False when `cap` is too small. */
bool kvm_srt_b64url_encode(const uint8_t *in, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
