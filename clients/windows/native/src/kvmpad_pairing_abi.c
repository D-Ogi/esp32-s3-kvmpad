// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Implementation of the exported C ABI (see include/kvmpad_pairing_abi.h).
//
// Every function here is a bounds check plus one call into the pairing core. It deliberately holds no
// protocol logic of its own: ADR-029 put the protocol in one place so the two ends cannot diverge,
// and a shim that decided anything would be a second place.

#include "kvmpad_pairing_abi.h"

#include <stdlib.h>
#include <string.h>

#include "kvm_pairing_client.h"
#include "kvm_pairing_pake.h"
#include "kvm_pairing_types.h"

// Supplied by the RNG translation unit of whichever DLL is being linked: BCryptGenRandom in the
// shipping one, the seeded fixture in the test-only one.
const kvm_pair_platform_t *kvmpad_pair_platform(void);

static int32_t s_started;

int32_t kvmpad_pair_startup(void)
{
    // kvm_pake_crypto_init() is documented idempotent, and installing the same platform block twice
    // is a pointer assignment, so repeating the pair is harmless. The flag only avoids the work.
    if (s_started != 0) {
        return KVMPAD_PAIR_OK;
    }
    kvm_pake_set_platform(kvmpad_pair_platform());
    const kvm_pair_err_t err = kvm_pake_crypto_init();
    if (err != KVM_PAIR_OK) {
        // Leave the platform installed but unflagged: a retry after a transient PSA failure is
        // allowed, and nothing has been derived.
        return (int32_t)err;
    }
    s_started = 1;
    return KVMPAD_PAIR_OK;
}

const char *kvmpad_pair_pake_name(void)
{
    return KVM_PAIR_PAKE_NAME;
}

int32_t kvmpad_pair_wire_version(void)
{
    return (int32_t)KVM_PAIR_WIRE_VERSION;
}

const char *kvmpad_pair_err_name(int32_t err)
{
    return kvm_pair_err_name((kvm_pair_err_t)err);
}

int32_t kvmpad_pair_sizes(int32_t *code_len, int32_t *sha256_len, int32_t *client_id_len,
                          int32_t *token_len, int32_t *token_sealed_len, int32_t *confirm_len,
                          int32_t *round1_max, int32_t *round2_max, int32_t *name_cap)
{
    if (code_len != NULL) {
        *code_len = (int32_t)KVM_PAIR_CODE_LEN;
    }
    if (sha256_len != NULL) {
        *sha256_len = (int32_t)KVM_PAIR_SHA256_LEN;
    }
    if (client_id_len != NULL) {
        *client_id_len = (int32_t)KVM_PAIR_CLIENT_ID_LEN;
    }
    if (token_len != NULL) {
        *token_len = (int32_t)KVM_PAIR_TOKEN_LEN;
    }
    if (token_sealed_len != NULL) {
        *token_sealed_len = (int32_t)KVM_PAIR_TOKEN_SEALED_LEN;
    }
    if (confirm_len != NULL) {
        *confirm_len = (int32_t)KVM_PAIR_CONFIRM_LEN;
    }
    if (round1_max != NULL) {
        *round1_max = (int32_t)KVM_PAIR_ROUND1_MAX;
    }
    if (round2_max != NULL) {
        *round2_max = (int32_t)KVM_PAIR_ROUND2_MAX;
    }
    if (name_cap != NULL) {
        *name_cap = (int32_t)KVM_PAIR_NAME_CAP;
    }
    return KVMPAD_PAIR_OK;
}

int32_t kvmpad_pair_sha256(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
    if (in == NULL || out == NULL || in_len < 0 || out_cap < (int32_t)KVM_PAIR_SHA256_LEN) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    return (int32_t)kvm_pair_sha256(in, (size_t)in_len, out);
}

int32_t kvmpad_pair_client_create(const char *code, const char *client_name,
                                  const uint8_t *client_id, int32_t client_id_len,
                                  const uint8_t *cert_sha256, int32_t cert_sha256_len,
                                  char *effective_name, int32_t effective_name_cap, void **handle)
{
    if (handle == NULL) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    *handle = NULL;
    if (code == NULL || client_name == NULL || client_id == NULL || cert_sha256 == NULL) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    // The lengths are checked rather than trusted: a wrong one on the managed side would otherwise
    // read past the end of a pinned array.
    if (client_id_len != (int32_t)KVM_PAIR_CLIENT_ID_LEN ||
        cert_sha256_len != (int32_t)KVM_PAIR_SHA256_LEN) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    if (effective_name != NULL && effective_name_cap < (int32_t)KVM_PAIR_NAME_CAP) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    if (strlen(code) != KVM_PAIR_CODE_LEN) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }

    const int32_t started = kvmpad_pair_startup();
    if (started != KVMPAD_PAIR_OK) {
        return started;
    }

    kvm_pair_client_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return (int32_t)KVM_PAIR_ERR_CRYPTO;
    }

    kvm_pair_hello_t hello;
    const kvm_pair_err_t err =
        kvm_pair_client_begin(c, code, client_name, client_id, cert_sha256, &hello);
    if (err != KVM_PAIR_OK) {
        kvm_pair_client_free(c);
        free(c);
        return (int32_t)err;
    }

    if (effective_name != NULL) {
        memcpy(effective_name, hello.client_name, KVM_PAIR_NAME_CAP);
        effective_name[KVM_PAIR_NAME_CAP - 1u] = '\0';
    }
    // `hello` holds no secret -- v, the algorithm name, the client name, the client identifier and
    // the certificate hash, all of which the caller already supplied -- but it is wiped anyway so
    // that nothing from a run lingers on this frame.
    kvm_pair_zeroize(&hello, sizeof(hello));

    *handle = c;
    return KVMPAD_PAIR_OK;
}

// Both rounds have the same shape, so they share one body.
static int32_t client_round(kvm_pair_client_t *c, int round, const uint8_t *in, int32_t in_len,
                           uint8_t *out, int32_t out_cap, int32_t *out_len)
{
    if (c == NULL || in == NULL || out == NULL || out_len == NULL || in_len < 0 || out_cap < 0) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    *out_len = 0;

    size_t produced = 0;
    const kvm_pair_err_t err =
        (round == 1)
            ? kvm_pair_client_round1(c, in, (size_t)in_len, out, (size_t)out_cap, &produced)
            : kvm_pair_client_round2(c, in, (size_t)in_len, out, (size_t)out_cap, &produced);
    if (err != KVM_PAIR_OK) {
        return (int32_t)err;
    }
    *out_len = (int32_t)produced;
    return KVMPAD_PAIR_OK;
}

int32_t kvmpad_pair_client_round1(void *handle, const uint8_t *in, int32_t in_len, uint8_t *out,
                                  int32_t out_cap, int32_t *out_len)
{
    return client_round((kvm_pair_client_t *)handle, 1, in, in_len, out, out_cap, out_len);
}

int32_t kvmpad_pair_client_round2(void *handle, const uint8_t *in, int32_t in_len, uint8_t *out,
                                  int32_t out_cap, int32_t *out_len)
{
    return client_round((kvm_pair_client_t *)handle, 2, in, in_len, out, out_cap, out_len);
}

int32_t kvmpad_pair_client_confirm_out(void *handle, uint8_t *out, int32_t out_cap)
{
    if (handle == NULL || out == NULL || out_cap < (int32_t)KVM_PAIR_CONFIRM_LEN) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    return (int32_t)kvm_pair_client_confirm_out((kvm_pair_client_t *)handle, out);
}

int32_t kvmpad_pair_client_confirm_in(void *handle, const uint8_t *tag, int32_t tag_len)
{
    if (handle == NULL || tag == NULL || tag_len < 0) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    // A wrong length is passed through to the core, which answers FRAME: the length check belongs
    // where the protocol is, not here.
    return (int32_t)kvm_pair_client_confirm_in((kvm_pair_client_t *)handle, tag, (size_t)tag_len);
}

int32_t kvmpad_pair_client_open_token(void *handle, const uint8_t *in, int32_t in_len,
                                      uint8_t *token, int32_t token_cap)
{
    if (handle == NULL || in == NULL || token == NULL || in_len < 0 ||
        token_cap < (int32_t)KVM_PAIR_TOKEN_LEN) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    return (int32_t)kvm_pair_client_open_token((kvm_pair_client_t *)handle, in, (size_t)in_len,
                                               token);
}

int32_t kvmpad_pair_client_transcript(void *handle, uint8_t *out, int32_t out_cap)
{
    if (handle == NULL || out == NULL || out_cap < (int32_t)KVM_PAIR_SHA256_LEN) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    return (int32_t)kvm_pair_client_transcript((kvm_pair_client_t *)handle, out);
}

void kvmpad_pair_client_destroy(void *handle)
{
    if (handle == NULL) {
        return;
    }
    kvm_pair_client_t *c = (kvm_pair_client_t *)handle;
    kvm_pair_client_free(c); // destroys the PSA keys and zeroizes the struct
    free(c);
}
