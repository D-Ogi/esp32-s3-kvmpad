// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// EC-JPAKE on P-256 with channel binding, key confirmation and token sealing (TASK-100, SEC-101,
// SEC-102, ADR-029). This is the only file in the pairing core that talks to PSA crypto.
//
// Algorithm: PSA_ALG_JPAKE(PSA_ALG_SHA_256) over PSA_ECC_FAMILY_SECP_R1/256 -- the single
// combination the pinned TF-PSA-Crypto 1.x implements. The built-in implementation also fixes the
// J-PAKE identities to the literals "client" and "server", so the channel binding of SEC-102 cannot
// ride in the identity fields; it is folded into the password and into the transcript instead:
//
//   binding  = 0x01 || cert_sha256[32] || client_id[16] || len(client_name) || client_name
//   pw       = HKDF-SHA256(salt = "KVMpad-pair-v1/pw", ikm = code (6 ASCII digits), info = binding)
//   Z        = the 65-byte J-PAKE shared point, 0x04 || X || Y
//   th       = SHA-256("KVMpad-pair-v1" || 0x01 || binding || blobs in canonical order)
//   prk      = HKDF-Extract(salt = th, ikm = Z)
//   kc_d     = HKDF-Expand(prk, "KVMpad-pair-v1/kc-device", 32)
//   kc_c     = HKDF-Expand(prk, "KVMpad-pair-v1/kc-client", 32)
//   k_tok    = HKDF-Expand(prk, "KVMpad-pair-v1/token", 32)
//   n_tok    = HKDF-Expand(prk, "KVMpad-pair-v1/token-nonce", 12)
//   confirm  = HMAC-SHA256(kc_<sender role>, th)
//   token_enc= AES-256-GCM(k_tok, n_tok, aad = "KVMpad-pair-v1/token" || th, pt = token[32])
//
// A different certificate, client identifier, client name or code yields a different password, so a
// substituted certificate makes the exchange fail at key confirmation and nothing is ever stored.
// The nonce is derived, not drawn: the key is used for exactly one sealing, so a fixed nonce is
// sound and makes the whole exchange reproducible from a fixed RNG stream.
//
// Internal call order is fixed and identical for both roles, whatever order the transport uses:
//   round1_out -> round1_in -> round2_out -> round2_in -> finish
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing_platform.h"
#include "kvm_pairing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_PAKE_ROLE_DEVICE = 0, // KVMpad; J-PAKE identity "server"
    KVM_PAKE_ROLE_CLIENT = 1  // the Windows application; J-PAKE identity "client"
} kvm_pake_role_t;

// Everything the transcript binds besides the code (SEC-102). `cert_sha256` is the SHA-256 of the
// DER form of the DEVICE TLS certificate used by the current handshake -- the DEVICE takes it from
// its own certificate, the CLIENT from the certificate the handshake presented, and a mismatch
// shows up as a failed key confirmation. `client_name` is NUL-terminated and at most
// KVM_PAIR_NAME_CAP-1 bytes of UTF-8.
typedef struct {
    uint8_t cert_sha256[KVM_PAIR_SHA256_LEN];
    uint8_t client_id[KVM_PAIR_CLIENT_ID_LEN];
    char client_name[KVM_PAIR_NAME_CAP];
} kvm_pake_binding_t;

// One J-PAKE run. Treat the fields as private: they hold the password key, the derived keys and the
// round blobs. kvm_pake_free() zeroizes the struct and destroys every PSA key it owns.
typedef struct {
    kvm_pake_role_t role;
    uint8_t stage; // internal progress marker
    bool finished;

    // PSA handles, stored as uint32_t so no PSA header leaks into this interface.
    uint32_t password_key;
    uint32_t shared_key;

    // The PAKE operation object, allocated by the implementation so psa/crypto.h stays private.
    void *op;

    kvm_pake_binding_t binding;

    // Round blobs in canonical order: index 0 = DEVICE, index 1 = CLIENT.
    uint8_t round1[2][KVM_PAIR_ROUND1_MAX];
    uint16_t round1_len[2];
    uint8_t round2[2][KVM_PAIR_ROUND2_MAX];
    uint16_t round2_len[2];

    uint8_t transcript[KVM_PAIR_SHA256_LEN];
    uint8_t kc_device[KVM_PAIR_CONFIRM_LEN];
    uint8_t kc_client[KVM_PAIR_CONFIRM_LEN];
    uint8_t k_token[32];
    uint8_t n_token[KVM_PAIR_AEAD_NONCE_LEN];
} kvm_pake_t;

// Start a run. `code` is exactly KVM_PAIR_CODE_LEN ASCII digits (no NUL required). `binding` must be
// fully populated. The platform hook supplies the randomness PSA needs for the ephemeral scalars.
// On any failure the context is left zeroized and no PSA key survives.
kvm_pair_err_t kvm_pake_init(kvm_pake_t *pake, kvm_pake_role_t role, const char *code,
                             const kvm_pake_binding_t *binding);

// Produce this side's round-1 blob (6 length-prefixed chunks, at most KVM_PAIR_ROUND1_MAX bytes).
kvm_pair_err_t kvm_pake_round1_out(kvm_pake_t *pake, uint8_t *out, size_t cap, size_t *out_len);

// Consume the peer's round-1 blob. A malformed blob gives KVM_PAIR_ERR_FRAME, a blob that fails the
// Schnorr proofs gives KVM_PAIR_ERR_BAD_CODE.
kvm_pair_err_t kvm_pake_round1_in(kvm_pake_t *pake, const uint8_t *in, size_t len);

// Produce this side's round-2 blob (3 length-prefixed chunks, at most KVM_PAIR_ROUND2_MAX bytes).
kvm_pair_err_t kvm_pake_round2_out(kvm_pake_t *pake, uint8_t *out, size_t cap, size_t *out_len);

// Consume the peer's round-2 blob.
kvm_pair_err_t kvm_pake_round2_in(kvm_pake_t *pake, const uint8_t *in, size_t len);

// Derive the transcript hash and the four session keys. After this call the J-PAKE operation is
// gone and only the derived material remains.
kvm_pair_err_t kvm_pake_finish(kvm_pake_t *pake);

// This side's key-confirmation tag. Requires kvm_pake_finish().
kvm_pair_err_t kvm_pake_confirm_out(kvm_pake_t *pake, uint8_t out[KVM_PAIR_CONFIRM_LEN]);

// Verify the peer's key-confirmation tag in constant time. KVM_PAIR_ERR_BAD_CODE on mismatch --
// the only signal a wrong code, a substituted certificate or a tampered transcript produces.
kvm_pair_err_t kvm_pake_confirm_verify(kvm_pake_t *pake, const uint8_t tag[KVM_PAIR_CONFIRM_LEN]);

// Seal the 32-byte token into KVM_PAIR_TOKEN_SEALED_LEN bytes (ciphertext followed by the GCM tag).
kvm_pair_err_t kvm_pake_token_seal(kvm_pake_t *pake, const uint8_t token[KVM_PAIR_TOKEN_LEN],
                                   uint8_t *out, size_t cap, size_t *out_len);

// Open a sealed token. Any tag failure gives KVM_PAIR_ERR_BAD_CODE and leaves `token` untouched.
kvm_pair_err_t kvm_pake_token_open(kvm_pake_t *pake, const uint8_t *in, size_t len,
                                   uint8_t token[KVM_PAIR_TOKEN_LEN]);

// The transcript hash, for diagnostics and for the interop vectors. Not a secret.
kvm_pair_err_t kvm_pake_transcript(const kvm_pake_t *pake, uint8_t out[KVM_PAIR_SHA256_LEN]);

// Release every PSA key and zeroize the context. Safe on a zeroized or partly initialized context.
void kvm_pake_free(kvm_pake_t *pake);

// --- helpers the rest of the core and the CLIENT both need ---

// Install the randomness hook PSA draws from. The firmware, the tests and the CLIENT each call this
// once before any pairing run. Passing NULL restores the fail-closed default, which makes every
// random draw fail rather than fall back to a weak source.
void kvm_pake_set_platform(const kvm_pair_platform_t *platform);

// Initialize PSA crypto once per process. Idempotent.
kvm_pair_err_t kvm_pake_crypto_init(void);

// Draw `len` bytes through the installed platform hook. False when no hook is installed or the
// generator failed; the caller must then fail closed. Used for the pairing code and the token.
bool kvm_pake_platform_random(uint8_t *out, size_t len);

// SHA-256 over one buffer; used for the certificate hash and the stored token hash.
kvm_pair_err_t kvm_pair_sha256(const uint8_t *in, size_t len, uint8_t out[KVM_PAIR_SHA256_LEN]);

// Constant-time comparison. True when the buffers are equal. No early exit, no branch on content.
bool kvm_pair_equal_ct(const uint8_t *a, const uint8_t *b, size_t len);

// Overwrite `len` bytes and keep the compiler from eliding it (SEC-105).
void kvm_pair_zeroize(void *p, size_t len);

#ifdef __cplusplus
}
#endif
