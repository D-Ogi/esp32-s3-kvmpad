// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Encoding helpers for the `pair.*` messages (TASK-100, ADR-029 §4/§5). The core stays free of JSON:
// the transport component builds the envelope of 03 §6.2 and fills the one or two string fields each
// `pair.*` payload carries, which these helpers produce and parse.
//
// A round blob is a self-describing concatenation of length-prefixed chunks:
//
//   chunk  := u8 length | length bytes
//   round1 := chunk(X1.key_share) chunk(X1.zk_public) chunk(X1.zk_proof)
//             chunk(X2.key_share) chunk(X2.zk_public) chunk(X2.zk_proof)
//   round2 := chunk(X2S.key_share) chunk(X2S.zk_public) chunk(X2S.zk_proof)
//
// Key shares and Schnorr public keys are uncompressed P-256 points, exactly 65 bytes, 0x04 || X || Y.
// A Schnorr proof is a big-endian scalar of at most 32 bytes; the pinned implementation strips
// leading zero bytes, so its length varies and the prefix is what makes the blob parseable. Blobs
// travel base64 (standard alphabet, padded) inside the JSON payload.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of chunks in each blob.
#define KVM_PAIR_ROUND1_CHUNKS 6u
#define KVM_PAIR_ROUND2_CHUNKS 3u

// Longest single chunk: an uncompressed P-256 point.
#define KVM_PAIR_CHUNK_MAX 65u

// Base64 length of a blob of `n` bytes, padding included, NUL excluded.
#define KVM_PAIR_B64_LEN(n) (((n) + 2u) / 3u * 4u)

// Buffer sizes, NUL included, for the encoded payload fields.
#define KVM_PAIR_ROUND1_B64_CAP (KVM_PAIR_B64_LEN(KVM_PAIR_ROUND1_MAX) + 1u)
#define KVM_PAIR_ROUND2_B64_CAP (KVM_PAIR_B64_LEN(KVM_PAIR_ROUND2_MAX) + 1u)
#define KVM_PAIR_CONFIRM_B64_CAP (KVM_PAIR_B64_LEN(KVM_PAIR_CONFIRM_LEN) + 1u)
#define KVM_PAIR_TOKEN_B64_CAP (KVM_PAIR_B64_LEN(KVM_PAIR_TOKEN_SEALED_LEN) + 1u)
#define KVM_PAIR_HEX32_CAP (2u * KVM_PAIR_SHA256_LEN + 1u)
#define KVM_PAIR_CLIENT_ID_HEX_CAP (2u * KVM_PAIR_CLIENT_ID_LEN + 1u)

// --- base64, standard alphabet with padding ---

// Encode `len` bytes into `out`, NUL-terminated. False when `cap` is too small.
bool kvm_pair_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap);

// Decode a NUL-terminated base64 string. False on a bad character, bad padding or a result longer
// than `cap`. Rejects embedded whitespace: the wire format has none.
bool kvm_pair_b64_decode(const char *in, uint8_t *out, size_t cap, size_t *out_len);

// --- lowercase hex, used for the certificate hash and the client identifier ---

bool kvm_pair_hex_encode(const uint8_t *in, size_t len, char *out, size_t cap);
bool kvm_pair_hex_decode(const char *in, uint8_t *out, size_t cap, size_t *out_len);

// --- round blobs ---

// Chunk writer. Start with kvm_pair_blob_begin(), append each chunk in order, then read `len`.
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} kvm_pair_blob_t;

void kvm_pair_blob_begin(kvm_pair_blob_t *w, uint8_t *buf, size_t cap);
bool kvm_pair_blob_put(kvm_pair_blob_t *w, const uint8_t *chunk, size_t len);

// Split a blob into exactly `chunks` chunks. `ptr[i]`/`len[i]` point into `blob`, which must stay
// alive. False when the blob is truncated, over-long, has trailing bytes or an empty chunk.
bool kvm_pair_blob_split(const uint8_t *blob, size_t blob_len, size_t chunks,
                         const uint8_t **ptr, size_t *len);

// --- mDNS service record (FR-723) ---

// Service type the DEVICE advertises.
#define KVM_PAIR_MDNS_SERVICE "_kvmpad"
#define KVM_PAIR_MDNS_PROTO "_tcp"
#define KVM_PAIR_MDNS_FQDN "_kvmpad._tcp.local"

// The three TXT keys of FR-723, in advertising order.
typedef struct {
    const char *name;  // "name", "v", "pair"
    const char *value;
} kvm_pair_txt_item_t;

#define KVM_PAIR_MDNS_TXT_ITEMS 3u

// Fill the TXT record for the current window state. `device_name` is the advertised DEVICE name
// (e.g. "KVMpad-7F3A"); `pair_open` is true only while a window is open. `items` must hold
// KVM_PAIR_MDNS_TXT_ITEMS entries and the returned values point into `scratch`, which must be at
// least 32 bytes and stay alive as long as the items are used.
bool kvm_pair_mdns_txt(const char *device_name, bool pair_open, kvm_pair_txt_item_t *items,
                       char *scratch, size_t scratch_cap);

#ifdef __cplusplus
}
#endif
