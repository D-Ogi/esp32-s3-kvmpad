// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Exported C ABI of kvmpad_pairing.dll: the CLIENT half of the pairing exchange (TASK-105, ADR-021,
// ADR-028, ADR-029).
//
// This is a thin, flat wrapper around components/secure_remote/pairing. It exists so the Windows
// application can reach the pairing core through P/Invoke without the managed side knowing anything
// about the core's structures: a run is an opaque handle, every buffer is a pointer plus a length,
// and every call returns a kvm_pair_err_t value as int32_t. ADR-029 chose one implementation
// compiled twice precisely so the CLIENT cannot drift from the DEVICE; re-stating the core's structs
// in C# would reintroduce exactly that drift.
//
// Rules this boundary keeps:
//   * every size the managed side needs comes from kvmpad_pair_sizes(), never from a literal in C#;
//   * a run that has not verified the DEVICE tag cannot be made to open a token -- the stage gate
//     lives in the core, and this layer adds none of its own that could contradict it;
//   * nothing here returns, logs or formats the code, the derived keys or the token. The only secret
//     that crosses the boundary is the opened 32-byte token, into a caller-owned buffer.
//
// Call order (the core enforces it; a call out of order returns the STATE error):
//   kvmpad_pair_startup()
//   kvmpad_pair_client_create()      -> gives the handle and the effective client name for pair.hello
//   kvmpad_pair_client_round1()      -> consumes the DEVICE blob, produces the CLIENT blob
//   kvmpad_pair_client_round2()      -> consumes the DEVICE blob, produces the CLIENT blob
//   kvmpad_pair_client_confirm_out() -> the CLIENT tag; the CLIENT proves first (ADR-029 §3)
//   kvmpad_pair_client_confirm_in()  -> verifies the DEVICE tag
//   kvmpad_pair_client_open_token()  -> the 32-byte token
//   kvmpad_pair_client_destroy()
#ifndef KVMPAD_PAIRING_ABI_H
#define KVMPAD_PAIRING_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define KVMPAD_ABI __declspec(dllexport)
#else
#define KVMPAD_ABI
#endif

// Mirrors kvm_pair_err_t. Only the success value is named here; the managed side maps the rest by
// the name kvmpad_pair_err_name() returns, so a new code in the core cannot silently become OK.
#define KVMPAD_PAIR_OK 0

// Initialize PSA crypto and install the BCryptGenRandom platform hook. Idempotent, and safe to call
// from several threads. Every other entry point requires it.
KVMPAD_ABI int32_t kvmpad_pair_startup(void);

// The value `pair.hello` must carry in `pake`, and the wire version it must carry in `v`. Taken from
// the core's own constants so the managed side cannot disagree with it.
KVMPAD_ABI const char *kvmpad_pair_pake_name(void);
KVMPAD_ABI int32_t kvmpad_pair_wire_version(void);

// The name of a result code, e.g. "BAD_CODE". Never NULL, never a secret, safe to show and to log.
KVMPAD_ABI const char *kvmpad_pair_err_name(int32_t err);

// Sizes of everything that crosses this boundary. Any out-pointer may be NULL.
KVMPAD_ABI int32_t kvmpad_pair_sizes(int32_t *code_len, int32_t *sha256_len, int32_t *client_id_len,
                                     int32_t *token_len, int32_t *token_sealed_len,
                                     int32_t *confirm_len, int32_t *round1_max, int32_t *round2_max,
                                     int32_t *name_cap);

// SHA-256 over one buffer, through the same crypto the DEVICE uses. Exported so a test can show the
// native hash and the managed one agree, which is what makes the certificate pin comparable.
KVMPAD_ABI int32_t kvmpad_pair_sha256(const uint8_t *in, int32_t in_len, uint8_t *out,
                                      int32_t out_cap);

// Start a run. `code` is the 6 digits the user typed, NUL-terminated. `client_name` is the Windows
// host name; the core truncates it to the wire cap, and `effective_name` receives the truncated form
// that the binding actually used -- `pair.hello` must carry that, not the original, or the DEVICE
// derives a different password. `client_id` and `cert_sha256` are raw bytes of the lengths
// kvmpad_pair_sizes() reports.
KVMPAD_ABI int32_t kvmpad_pair_client_create(const char *code, const char *client_name,
                                             const uint8_t *client_id, int32_t client_id_len,
                                             const uint8_t *cert_sha256, int32_t cert_sha256_len,
                                             char *effective_name, int32_t effective_name_cap,
                                             void **handle);

KVMPAD_ABI int32_t kvmpad_pair_client_round1(void *handle, const uint8_t *in, int32_t in_len,
                                             uint8_t *out, int32_t out_cap, int32_t *out_len);

KVMPAD_ABI int32_t kvmpad_pair_client_round2(void *handle, const uint8_t *in, int32_t in_len,
                                             uint8_t *out, int32_t out_cap, int32_t *out_len);

KVMPAD_ABI int32_t kvmpad_pair_client_confirm_out(void *handle, uint8_t *out, int32_t out_cap);

KVMPAD_ABI int32_t kvmpad_pair_client_confirm_in(void *handle, const uint8_t *tag, int32_t tag_len);

KVMPAD_ABI int32_t kvmpad_pair_client_open_token(void *handle, const uint8_t *in, int32_t in_len,
                                                 uint8_t *token, int32_t token_cap);

// The transcript hash. Not a secret; used for diagnostics and by the interop vectors.
KVMPAD_ABI int32_t kvmpad_pair_client_transcript(void *handle, uint8_t *out, int32_t out_cap);

// Release the run, destroy its PSA keys and zeroize the memory. Safe on NULL, and safe to call twice
// only in the sense that the second call must pass NULL -- the handle is invalid afterwards.
KVMPAD_ABI void kvmpad_pair_client_destroy(void *handle);

#ifdef __cplusplus
}
#endif

#endif // KVMPAD_PAIRING_ABI_H
