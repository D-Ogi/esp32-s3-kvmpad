// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// CLIENT side of the pairing exchange (TASK-100/TASK-105, ADR-021, ADR-029).
//
// This is the reason the pairing core is written without a single firmware dependency: the Windows
// application links these very sources, built as a DLL against the same pinned crypto, so the two
// ends cannot drift. The CLIENT supplies the code the user typed, the certificate it pinned during
// the handshake and its own identifier; the core produces the blobs, verifies the DEVICE
// confirmation and opens the sealed token.
//
// Call order:
//   kvm_pair_client_begin()        -> fills `pair.hello`
//   kvm_pair_client_round1(...)    -> consumes the DEVICE blob, produces the CLIENT blob
//   kvm_pair_client_round2(...)    -> consumes the DEVICE blob, produces the CLIENT blob
//   kvm_pair_client_confirm_out()  -> the CLIENT tag; send it, then wait
//   kvm_pair_client_confirm_in()   -> verifies the DEVICE tag
//   kvm_pair_client_open_token()   -> the 32-byte token, to be stored in DPAPI / Credential Manager
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing.h"
#include "kvm_pairing_pake.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    kvm_pake_t pake;
    bool live;
    uint8_t stage;
} kvm_pair_client_t;

// Start a run. `code` is the 6 digits the user typed; `client_name` the Windows host name (FR-724);
// `client_id` the stable per-installation identifier; `cert_sha256` the SHA-256 of the DER
// certificate the TLS handshake presented, which the CLIENT pins only if pairing succeeds (SEC-102).
// `hello` is filled with exactly what `pair.hello` must carry.
kvm_pair_err_t kvm_pair_client_begin(kvm_pair_client_t *c, const char *code,
                                     const char *client_name,
                                     const uint8_t client_id[KVM_PAIR_CLIENT_ID_LEN],
                                     const uint8_t cert_sha256[KVM_PAIR_SHA256_LEN],
                                     kvm_pair_hello_t *hello);

// Consume the DEVICE round-1 blob and produce the CLIENT round-1 blob.
kvm_pair_err_t kvm_pair_client_round1(kvm_pair_client_t *c, const uint8_t *in, size_t in_len,
                                      uint8_t *out, size_t cap, size_t *out_len);

// Consume the DEVICE round-2 blob and produce the CLIENT round-2 blob. This completes the key
// exchange on the CLIENT side.
kvm_pair_err_t kvm_pair_client_round2(kvm_pair_client_t *c, const uint8_t *in, size_t in_len,
                                      uint8_t *out, size_t cap, size_t *out_len);

// The CLIENT key-confirmation tag.
kvm_pair_err_t kvm_pair_client_confirm_out(kvm_pair_client_t *c,
                                           uint8_t out[KVM_PAIR_CONFIRM_LEN]);

// Verify the DEVICE tag. KVM_PAIR_ERR_BAD_CODE means a wrong code or a substituted certificate: the
// CLIENT must pin nothing and store nothing.
kvm_pair_err_t kvm_pair_client_confirm_in(kvm_pair_client_t *c, const uint8_t *tag, size_t len);

// Open the sealed token from `pair.accepted`.
kvm_pair_err_t kvm_pair_client_open_token(kvm_pair_client_t *c, const uint8_t *in, size_t in_len,
                                          uint8_t token[KVM_PAIR_TOKEN_LEN]);

// The transcript hash, for diagnostics and the interop vectors.
kvm_pair_err_t kvm_pair_client_transcript(const kvm_pair_client_t *c,
                                          uint8_t out[KVM_PAIR_SHA256_LEN]);

// Release the run and zeroize everything.
void kvm_pair_client_free(kvm_pair_client_t *c);

#ifdef __cplusplus
}
#endif
