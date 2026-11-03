// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Two in-process peers wired to each other, so a host test can run a whole pairing exchange the way
// the transport would: DEVICE session on one side, CLIENT driver on the other, every message passed
// as the exact bytes that would travel the wire.
#pragma once

#include <stdio.h>
#include <string.h>

#include "det_rng.h"
#include "fake_pair_store.h"
#include "kvm_pairing.h"
#include "kvm_pairing_client.h"

// The certificate the DEVICE presents in the test; its SHA-256 is the channel binding (SEC-102).
static const char k_test_cert[] = "KVMpad test DEVICE certificate (DER stand-in), 2026-11-02";

typedef struct {
    kvm_pair_session_t device;
    fake_pair_store_t store;
    kvm_pair_store_t store_ops;
    kvm_pair_client_t client;

    uint8_t cert_sha[KVM_PAIR_SHA256_LEN];
    uint8_t client_id[KVM_PAIR_CLIENT_ID_LEN];

    // The last bytes each message carried, kept so a test (and the vector generator) can inspect
    // them: d_r1 = DEVICE round 1, c_r1 = CLIENT round 1, and so on.
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
    uint8_t transcript[KVM_PAIR_SHA256_LEN];
    char code[KVM_PAIR_CODE_CAP];
} pair_harness_t;

// Set up a DEVICE session with a fresh store. `require_accept` follows ADR-023.
static kvm_pair_err_t harness_init(pair_harness_t *h, bool require_accept, uint64_t seed)
{
    memset(h, 0, sizeof(*h));
    det_rng_seed(seed);
    det_clock_reset();

    kvm_pake_set_platform(det_platform());
    if (kvm_pake_crypto_init() != KVM_PAIR_OK) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    if (kvm_pair_sha256((const uint8_t *)k_test_cert, sizeof(k_test_cert) - 1, h->cert_sha) !=
        KVM_PAIR_OK) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    for (size_t i = 0; i < KVM_PAIR_CLIENT_ID_LEN; i++) {
        h->client_id[i] = (uint8_t)(0xa0u + i);
    }

    fake_pair_store_init(&h->store, &h->store_ops);

    kvm_pair_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.require_accept = require_accept;
    memcpy(cfg.device_name, "KVMpad-7F3A", sizeof("KVMpad-7F3A"));
    memcpy(cfg.cert_sha256, h->cert_sha, sizeof(cfg.cert_sha256));

    return kvm_pair_session_init(&h->device, &cfg, det_platform(), &h->store_ops);
}

static kvm_pair_err_t harness_open(pair_harness_t *h)
{
    kvm_pair_preconditions_t pre = { .remote_enabled = true, .wifi_connected = true };
    kvm_pair_err_t err = kvm_pair_window_open(&h->device, &pre);
    if (err == KVM_PAIR_OK) {
        const char *code = kvm_pair_window_code(&h->device);
        if (code == NULL) {
            return KVM_PAIR_ERR_STATE;
        }
        memcpy(h->code, code, KVM_PAIR_CODE_CAP);
    }
    return err;
}

// Run the exchange up to and including key confirmation, using `code` and `cert_sha` on the CLIENT
// side -- a test passes a wrong code or a substituted certificate here. Returns the first error.
// `stop_err` reports which step failed: 1 hello, 2 round1, 3 round2, 4 client confirm, 5 device
// confirm. `out` carries the DEVICE result of the last call.
static kvm_pair_err_t harness_run_to_confirm(pair_harness_t *h, const char *code,
                                             const uint8_t cert_sha[KVM_PAIR_SHA256_LEN],
                                             int *step, kvm_pair_out_t *out)
{
    kvm_pair_hello_t hello;
    kvm_pair_out_t o;
    kvm_pair_err_t err;

    if (step != NULL) {
        *step = 1;
    }
    err = kvm_pair_client_begin(&h->client, code, "DESK1", h->client_id, cert_sha, &hello);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pair_handle_hello(&h->device, &hello, &o);
    if (out != NULL) {
        *out = o;
    }
    if (err != KVM_PAIR_OK) {
        return err;
    }
    memcpy(h->d_r1, o.blob, o.blob_len);
    h->d_r1_len = o.blob_len;

    if (step != NULL) {
        *step = 2;
    }
    err = kvm_pair_client_round1(&h->client, h->d_r1, h->d_r1_len, h->c_r1, sizeof(h->c_r1),
                                 &h->c_r1_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pair_handle_round1(&h->device, h->c_r1, h->c_r1_len, &o);
    if (out != NULL) {
        *out = o;
    }
    if (err != KVM_PAIR_OK) {
        return err;
    }
    memcpy(h->d_r2, o.blob, o.blob_len);
    h->d_r2_len = o.blob_len;

    if (step != NULL) {
        *step = 3;
    }
    err = kvm_pair_client_round2(&h->client, h->d_r2, h->d_r2_len, h->c_r2, sizeof(h->c_r2),
                                 &h->c_r2_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pair_handle_round2(&h->device, h->c_r2, h->c_r2_len, &o);
    if (out != NULL) {
        *out = o;
    }
    if (err != KVM_PAIR_OK) {
        return err;
    }

    if (step != NULL) {
        *step = 4;
    }
    err = kvm_pair_client_confirm_out(&h->client, h->c_tag);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pair_handle_confirm(&h->device, h->c_tag, sizeof(h->c_tag), &o);
    if (out != NULL) {
        *out = o;
    }
    if (err != KVM_PAIR_OK) {
        return err;
    }
    memcpy(h->d_tag, o.blob, KVM_PAIR_CONFIRM_LEN);
    if (o.kind == KVM_PAIR_OUT_CONFIRM_ACCEPTED) {
        memcpy(h->sealed, o.token, o.token_len);
        h->sealed_len = o.token_len;
    }

    if (step != NULL) {
        *step = 5;
    }
    err = kvm_pair_client_confirm_in(&h->client, h->d_tag, KVM_PAIR_CONFIRM_LEN);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    (void)kvm_pair_client_transcript(&h->client, h->transcript);
    if (step != NULL) {
        *step = 0;
    }
    return KVM_PAIR_OK;
}

// Open the sealed token on the CLIENT side.
static kvm_pair_err_t harness_open_token(pair_harness_t *h)
{
    return kvm_pair_client_open_token(&h->client, h->sealed, h->sealed_len, h->token);
}

static void harness_free(pair_harness_t *h)
{
    kvm_pair_client_free(&h->client);
    kvm_pair_session_free(&h->device);
}
