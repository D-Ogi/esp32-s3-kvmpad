// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// CLIENT side of the pairing exchange (TASK-100/TASK-105, ADR-021, ADR-029). Thin on purpose: the
// whole protocol lives in kvm_pairing_pake.c, which both ends build from the same sources.

#include "kvm_pairing_client.h"

#include <string.h>

enum { CSTAGE_INIT = 0, CSTAGE_HELLO, CSTAGE_R1, CSTAGE_R2, CSTAGE_CONFIRMED };

kvm_pair_err_t kvm_pair_client_begin(kvm_pair_client_t *c, const char *code,
                                     const char *client_name,
                                     const uint8_t client_id[KVM_PAIR_CLIENT_ID_LEN],
                                     const uint8_t cert_sha256[KVM_PAIR_SHA256_LEN],
                                     kvm_pair_hello_t *hello)
{
    if (c == NULL || code == NULL || client_name == NULL || client_id == NULL ||
        cert_sha256 == NULL || hello == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (client_name[0] == '\0') {
        return KVM_PAIR_ERR_ARG;
    }

    memset(c, 0, sizeof(*c));

    kvm_pake_binding_t binding;
    memset(&binding, 0, sizeof(binding));
    memcpy(binding.cert_sha256, cert_sha256, KVM_PAIR_SHA256_LEN);
    memcpy(binding.client_id, client_id, KVM_PAIR_CLIENT_ID_LEN);
    size_t i = 0;
    while (i + 1u < KVM_PAIR_NAME_CAP && client_name[i] != '\0') {
        binding.client_name[i] = client_name[i];
        i++;
    }
    binding.client_name[i] = '\0';

    kvm_pair_err_t err = kvm_pake_init(&c->pake, KVM_PAKE_ROLE_CLIENT, code, &binding);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    c->live = true;
    c->stage = CSTAGE_HELLO;

    memset(hello, 0, sizeof(*hello));
    hello->v = KVM_PAIR_WIRE_VERSION;
    hello->pake = KVM_PAIR_PAKE_NAME;
    memcpy(hello->client_name, binding.client_name, sizeof(hello->client_name));
    memcpy(hello->client_id, binding.client_id, KVM_PAIR_CLIENT_ID_LEN);
    memcpy(hello->cert_sha256, binding.cert_sha256, KVM_PAIR_SHA256_LEN);
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_client_round1(kvm_pair_client_t *c, const uint8_t *in, size_t in_len,
                                      uint8_t *out, size_t cap, size_t *out_len)
{
    if (c == NULL || in == NULL || out == NULL || out_len == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!c->live || c->stage != CSTAGE_HELLO) {
        return KVM_PAIR_ERR_STATE;
    }
    // The internal order is round1_out before round1_in for both roles, whatever the transport did;
    // round-1 output does not depend on the peer, so buffering the DEVICE blob costs nothing.
    kvm_pair_err_t err = kvm_pake_round1_out(&c->pake, out, cap, out_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pake_round1_in(&c->pake, in, in_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    c->stage = CSTAGE_R1;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_client_round2(kvm_pair_client_t *c, const uint8_t *in, size_t in_len,
                                      uint8_t *out, size_t cap, size_t *out_len)
{
    if (c == NULL || in == NULL || out == NULL || out_len == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!c->live || c->stage != CSTAGE_R1) {
        return KVM_PAIR_ERR_STATE;
    }
    kvm_pair_err_t err = kvm_pake_round2_out(&c->pake, out, cap, out_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pake_round2_in(&c->pake, in, in_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pake_finish(&c->pake);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    c->stage = CSTAGE_R2;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_client_confirm_out(kvm_pair_client_t *c, uint8_t out[KVM_PAIR_CONFIRM_LEN])
{
    if (c == NULL || out == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!c->live || c->stage < CSTAGE_R2) {
        return KVM_PAIR_ERR_STATE;
    }
    return kvm_pake_confirm_out(&c->pake, out);
}

kvm_pair_err_t kvm_pair_client_confirm_in(kvm_pair_client_t *c, const uint8_t *tag, size_t len)
{
    if (c == NULL || tag == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!c->live || c->stage < CSTAGE_R2) {
        return KVM_PAIR_ERR_STATE;
    }
    if (len != KVM_PAIR_CONFIRM_LEN) {
        return KVM_PAIR_ERR_FRAME;
    }
    kvm_pair_err_t err = kvm_pake_confirm_verify(&c->pake, tag);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    c->stage = CSTAGE_CONFIRMED;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_client_open_token(kvm_pair_client_t *c, const uint8_t *in, size_t in_len,
                                          uint8_t token[KVM_PAIR_TOKEN_LEN])
{
    if (c == NULL || in == NULL || token == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    // Opening the token before the DEVICE tag has been verified would mean trusting material from an
    // unauthenticated peer, so the stage gate is deliberately strict here.
    if (!c->live || c->stage != CSTAGE_CONFIRMED) {
        return KVM_PAIR_ERR_STATE;
    }
    return kvm_pake_token_open(&c->pake, in, in_len, token);
}

kvm_pair_err_t kvm_pair_client_transcript(const kvm_pair_client_t *c,
                                          uint8_t out[KVM_PAIR_SHA256_LEN])
{
    if (c == NULL || out == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    return kvm_pake_transcript(&c->pake, out);
}

void kvm_pair_client_free(kvm_pair_client_t *c)
{
    if (c == NULL) {
        return;
    }
    kvm_pake_free(&c->pake);
    kvm_pair_zeroize(c, sizeof(*c));
}
