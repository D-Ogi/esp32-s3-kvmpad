// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Per-device TLS identity (TASK-100; TECH-041, TECH-056, SEC-102, ADR-028 §3).
//
// The DEVICE presents a self-signed ECDSA P-256 certificate. Its SHA-256 over the whole DER is
// three things at once:
//   * what the CLIENT pins during pairing and checks on every later session (ADR-028 §3);
//   * the channel binding of the pairing PAKE (SEC-102, ADR-029 §2);
//   * the only identity in the handshake, since the chain can never validate against a public
//     root and the CLIENT ignores SslPolicyErrors on purpose.
//
// Therefore the certificate MUST survive reboots and OTA: regenerating it invalidates every pin
// and costs every user a re-pairing (ADR-028 §3, "Consequences"). It is generated once on first
// boot and then only read back.
//
// The key never leaves the device and is never logged. Persistence is injected, so this file has no
// NVS dependency and the integrator decides where the two blobs live.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing_types.h" // KVM_PAIR_SHA256_LEN, KVM_PAIR_NAME_CAP

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A P-256 self-signed certificate with one CN, basicConstraints and the two key identifiers is
 * around 450 bytes of DER; the headroom covers a longer name.
 */
#define KVM_SRT_CERT_DER_MAX 1024u

/** A P-256 private key in SEC1 DER is 121 bytes. */
#define KVM_SRT_KEY_DER_MAX 256u

/** Storage keys of the two blobs. */
#define KVM_SRT_IDENTITY_BLOB_CERT "srt_cert"
#define KVM_SRT_IDENTITY_BLOB_KEY "srt_key"

/**
 * Validity of the generated certificate, as mbedTLS wants it ("YYYYMMDDhhmmss", UTC).
 *
 * Fixed constants rather than a clock reading: the DEVICE has no RTC and may have no network time
 * on first boot, and the pin covers notBefore and notAfter anyway, so the CLIENT compares the exact
 * bytes the user approved on screen. ADR-028 §3 states that expiry is deliberately not re-checked,
 * which only holds if the dates cannot make a working pairing stop working.
 */
#define KVM_SRT_CERT_NOT_BEFORE "20260101000000"
#define KVM_SRT_CERT_NOT_AFTER "20760101000000"

typedef struct {
    uint8_t cert_der[KVM_SRT_CERT_DER_MAX];
    size_t cert_len;
    uint8_t key_der[KVM_SRT_KEY_DER_MAX]; // SEC1 `ECPrivateKey`, as mbedtls_pk_write_key_der emits
    size_t key_len;
    uint8_t cert_sha256[KVM_PAIR_SHA256_LEN]; // the CLIENT's pin and the SEC-102 binding
    char device_name[KVM_PAIR_NAME_CAP];      // the certificate CN and the mDNS `name` (FR-723)
    bool generated;                           // true when this boot created it
} kvm_srt_identity_t;

/**
 * Blob persistence, injected by the integrator (NVS in the firmware).
 *
 * `load` returns false when the blob is absent or does not fit `cap`. `save` returns false when the
 * write failed; a failed save is fatal for identity creation, because a certificate that is not
 * persisted would change on the next boot and silently break every pin.
 */
typedef struct {
    bool (*load)(void *ctx, const char *key, uint8_t *out, size_t cap, size_t *out_len);
    bool (*save)(void *ctx, const char *key, const uint8_t *in, size_t len);
    void *ctx;
} kvm_srt_identity_store_t;

#ifdef __cplusplus
}
#endif
