// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// One reproducible pairing run, reduced to the fields the interop vectors record. Shared by the
// generator (gen_pairing_vectors.c) and the test that re-runs it against the checked-in files
// (test_pairing_vectors.c), so the two can never drift.
//
// Everything here is deterministic given `seed`: the pairing code, the J-PAKE ephemeral scalars, the
// client token, the derived keys and therefore every byte on the wire. A CLIENT implementation that
// feeds the same inputs must produce the same bytes.
#pragma once

#include <string.h>

#include "det_rng.h"
#include "fake_pair_store.h"
#include "kvm_pairing.h"
#include "kvm_pairing_client.h"
#include "kvm_pairing_wire.h"

// The certificate whose SHA-256 is the channel binding in every vector. A fixed string, not a real
// certificate: the binding only ever consumes its hash.
#define VECTOR_CERT_TEXT "KVMpad interop vector DEVICE certificate (DER stand-in)"

#define VECTOR_CLIENT_NAME "DESK1"
#define VECTOR_DEVICE_NAME "KVMpad-7F3A"

typedef struct {
    const char *name;
    uint64_t seed;
    bool require_accept;
    // When set, the CLIENT uses this code instead of the one the DEVICE drew: the wrong-code vector.
    const char *client_code_override;
    // When set, the CLIENT reports this certificate hash: the substituted-certificate vector.
    bool cert_override;
} vector_case_t;

typedef struct {
    char code[KVM_PAIR_CODE_CAP];
    uint8_t cert_sha[KVM_PAIR_SHA256_LEN];
    uint8_t client_id[KVM_PAIR_CLIENT_ID_LEN];

    uint8_t d_r1[KVM_PAIR_ROUND1_MAX];
    size_t d_r1_len;
    uint8_t c_r1[KVM_PAIR_ROUND1_MAX];
    size_t c_r1_len;
    uint8_t d_r2[KVM_PAIR_ROUND2_MAX];
    size_t d_r2_len;
    uint8_t c_r2[KVM_PAIR_ROUND2_MAX];
    size_t c_r2_len;
    uint8_t c_tag[KVM_PAIR_CONFIRM_LEN];
    uint8_t d_tag[KVM_PAIR_CONFIRM_LEN];
    uint8_t sealed[KVM_PAIR_TOKEN_SEALED_LEN];
    size_t sealed_len;
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    uint8_t token_sha[KVM_PAIR_SHA256_LEN];
    uint8_t transcript[KVM_PAIR_SHA256_LEN];

    int failed_step;          // 0 when the run finished, otherwise the step that failed
    kvm_pair_err_t failed_err;
    bool completed;
} vector_result_t;

// Run one case. Returns false only when the fixture itself could not be set up; a case that is
// supposed to fail reports that through `failed_step` / `failed_err`.
static bool vector_run(const vector_case_t *vc, vector_result_t *r)
{
    kvm_pair_session_t device;
    kvm_pair_client_t client;
    fake_pair_store_t store;
    kvm_pair_store_t store_ops;
    kvm_pair_config_t cfg;
    kvm_pair_hello_t hello;
    kvm_pair_out_t out;
    kvm_pair_err_t err;

    memset(r, 0, sizeof(*r));
    det_rng_seed(vc->seed);
    det_clock_reset();
    kvm_pake_set_platform(det_platform());
    if (kvm_pake_crypto_init() != KVM_PAIR_OK) {
        return false;
    }
    if (kvm_pair_sha256((const uint8_t *)VECTOR_CERT_TEXT, sizeof(VECTOR_CERT_TEXT) - 1,
                        r->cert_sha) != KVM_PAIR_OK) {
        return false;
    }
    for (size_t i = 0; i < KVM_PAIR_CLIENT_ID_LEN; i++) {
        r->client_id[i] = (uint8_t)(0xa0u + i);
    }

    fake_pair_store_init(&store, &store_ops);
    memset(&cfg, 0, sizeof(cfg));
    cfg.require_accept = vc->require_accept;
    memcpy(cfg.device_name, VECTOR_DEVICE_NAME, sizeof(VECTOR_DEVICE_NAME));
    memcpy(cfg.cert_sha256, r->cert_sha, sizeof(cfg.cert_sha256));
    if (kvm_pair_session_init(&device, &cfg, det_platform(), &store_ops) != KVM_PAIR_OK) {
        return false;
    }

    kvm_pair_preconditions_t pre = { .remote_enabled = true, .wifi_connected = true };
    if (kvm_pair_window_open(&device, &pre) != KVM_PAIR_OK) {
        kvm_pair_session_free(&device);
        return false;
    }
    const char *code = kvm_pair_window_code(&device);
    if (code == NULL) {
        kvm_pair_session_free(&device);
        return false;
    }
    memcpy(r->code, code, KVM_PAIR_CODE_CAP);

    const char *client_code = (vc->client_code_override != NULL) ? vc->client_code_override
                                                                 : r->code;
    uint8_t client_cert[KVM_PAIR_SHA256_LEN];
    memcpy(client_cert, r->cert_sha, sizeof(client_cert));
    if (vc->cert_override) {
        memset(client_cert, 0x7e, sizeof(client_cert));
    }

#define VSTEP(n, call)                 \
    do {                               \
        r->failed_step = (n);          \
        err = (call);                  \
        if (err != KVM_PAIR_OK) {      \
            r->failed_err = err;       \
            goto done;                 \
        }                              \
    } while (0)

    VSTEP(1, kvm_pair_client_begin(&client, client_code, VECTOR_CLIENT_NAME, r->client_id,
                                   client_cert, &hello));
    VSTEP(1, kvm_pair_handle_hello(&device, &hello, &out));
    memcpy(r->d_r1, out.blob, out.blob_len);
    r->d_r1_len = out.blob_len;

    VSTEP(2, kvm_pair_client_round1(&client, r->d_r1, r->d_r1_len, r->c_r1, sizeof(r->c_r1),
                                    &r->c_r1_len));
    VSTEP(2, kvm_pair_handle_round1(&device, r->c_r1, r->c_r1_len, &out));
    memcpy(r->d_r2, out.blob, out.blob_len);
    r->d_r2_len = out.blob_len;

    VSTEP(3, kvm_pair_client_round2(&client, r->d_r2, r->d_r2_len, r->c_r2, sizeof(r->c_r2),
                                    &r->c_r2_len));
    VSTEP(3, kvm_pair_handle_round2(&device, r->c_r2, r->c_r2_len, &out));

    VSTEP(4, kvm_pair_client_confirm_out(&client, r->c_tag));
    VSTEP(4, kvm_pair_handle_confirm(&device, r->c_tag, KVM_PAIR_CONFIRM_LEN, &out));
    memcpy(r->d_tag, out.blob, KVM_PAIR_CONFIRM_LEN);
    if (out.kind == KVM_PAIR_OUT_CONFIRM_ACCEPTED) {
        memcpy(r->sealed, out.token, out.token_len);
        r->sealed_len = out.token_len;
    }

    VSTEP(5, kvm_pair_client_confirm_in(&client, r->d_tag, KVM_PAIR_CONFIRM_LEN));
    VSTEP(5, kvm_pair_client_transcript(&client, r->transcript));

    if (vc->require_accept) {
        VSTEP(6, kvm_pair_local_accept(&device, &out));
        memcpy(r->sealed, out.token, out.token_len);
        r->sealed_len = out.token_len;
    }
    VSTEP(7, kvm_pair_client_open_token(&client, r->sealed, r->sealed_len, r->token));
    VSTEP(7, kvm_pair_sha256(r->token, KVM_PAIR_TOKEN_LEN, r->token_sha));

    r->failed_step = 0;
    r->completed = store.count == 1;
#undef VSTEP

done:
    kvm_pair_client_free(&client);
    kvm_pair_session_free(&device);
    return true;
}
