// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Test fixture, never shipped (see include/kvmpad_pairing_testonly.h).
//
// The run itself is test/pairing/vector_run.h, included unchanged: the vector generator, the C test
// and these managed tests then exercise one body of code, so the three cannot drift from each other
// or from the checked-in files.

#include "kvmpad_pairing_testonly.h"

#include <string.h>

#include "kvmpad_pairing_abi.h"

#include "vector_run.h" // pulls in det_rng.h, fake_pair_store.h and the pairing core

// The test DLL draws from the seeded fixture, which is what makes a run reproducible. The shipping
// DLL supplies BCryptGenRandom from win_rng.c instead; det_rng.c also provides the
// mbedtls_psa_external_get_random that PSA needs, so the J-PAKE scalars come from the same stream.
const kvm_pair_platform_t *kvmpad_pair_platform(void)
{
    return det_platform();
}

static vector_result_t s_last;
static int32_t s_have_last;

int32_t kvmpad_test_vector_run(uint64_t seed, int32_t require_accept,
                               const char *client_code_override, int32_t cert_override,
                               int32_t *failed_step, int32_t *failed_err, int32_t *completed)
{
    vector_case_t vc;
    memset(&vc, 0, sizeof(vc));
    vc.name = "managed";
    vc.seed = seed;
    vc.require_accept = require_accept != 0;
    vc.client_code_override = client_code_override;
    vc.cert_override = cert_override != 0;

    s_have_last = 0;
    memset(&s_last, 0, sizeof(s_last));
    if (!vector_run(&vc, &s_last)) {
        return (int32_t)KVM_PAIR_ERR_CRYPTO;
    }
    s_have_last = 1;

    if (failed_step != NULL) {
        *failed_step = (int32_t)s_last.failed_step;
    }
    if (failed_err != NULL) {
        *failed_err = (int32_t)s_last.failed_err;
    }
    if (completed != NULL) {
        *completed = s_last.completed ? 1 : 0;
    }
    return KVMPAD_PAIR_OK;
}

int32_t kvmpad_test_vector_field(int32_t field, uint8_t *out, int32_t out_cap, int32_t *out_len)
{
    if (out == NULL || out_len == NULL || out_cap < 0) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    *out_len = 0;
    if (s_have_last == 0) {
        return (int32_t)KVM_PAIR_ERR_STATE;
    }

    const uint8_t *src = NULL;
    size_t len = 0;

    switch ((kvmpad_vector_field_t)field) {
    case KVMPAD_VF_CODE:
        src = (const uint8_t *)s_last.code;
        len = KVM_PAIR_CODE_LEN;
        break;
    case KVMPAD_VF_CERT_SHA256:
        src = s_last.cert_sha;
        len = KVM_PAIR_SHA256_LEN;
        break;
    case KVMPAD_VF_CLIENT_ID:
        src = s_last.client_id;
        len = KVM_PAIR_CLIENT_ID_LEN;
        break;
    case KVMPAD_VF_DEVICE_ROUND1:
        src = s_last.d_r1;
        len = s_last.d_r1_len;
        break;
    case KVMPAD_VF_CLIENT_ROUND1:
        src = s_last.c_r1;
        len = s_last.c_r1_len;
        break;
    case KVMPAD_VF_DEVICE_ROUND2:
        src = s_last.d_r2;
        len = s_last.d_r2_len;
        break;
    case KVMPAD_VF_CLIENT_ROUND2:
        src = s_last.c_r2;
        len = s_last.c_r2_len;
        break;
    case KVMPAD_VF_CLIENT_CONFIRM:
        src = s_last.c_tag;
        len = KVM_PAIR_CONFIRM_LEN;
        break;
    case KVMPAD_VF_DEVICE_CONFIRM:
        src = s_last.d_tag;
        len = KVM_PAIR_CONFIRM_LEN;
        break;
    case KVMPAD_VF_TOKEN_SEALED:
        src = s_last.sealed;
        len = s_last.sealed_len;
        break;
    case KVMPAD_VF_TOKEN:
        src = s_last.token;
        len = KVM_PAIR_TOKEN_LEN;
        break;
    case KVMPAD_VF_TOKEN_SHA256:
        src = s_last.token_sha;
        len = KVM_PAIR_SHA256_LEN;
        break;
    case KVMPAD_VF_TRANSCRIPT:
        src = s_last.transcript;
        len = KVM_PAIR_SHA256_LEN;
        break;
    default:
        return (int32_t)KVM_PAIR_ERR_ARG;
    }

    *out_len = (int32_t)len;
    if (out_cap < (int32_t)len) {
        return (int32_t)KVM_PAIR_ERR_ARG;
    }
    if (len > 0u) {
        memcpy(out, src, len);
    }
    return KVMPAD_PAIR_OK;
}
