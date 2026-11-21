// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_publisher.h"
#include "pack_port.h"
#include <stdlib.h>
#include "pack_publisher_keys.h"
#include "third_party/aws_lc/edwards_verify.inc"

// RFC 8032, pure Ed25519. Group and scalar operations are the portable
// verification functions from AWS-LC. All inputs here are public.
static bool verify(const uint8_t key[32], const uint8_t sig[64], uint8_t hash[64])
{
    static const uint8_t order[32] = {
        0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
    int i = 31;
    while (i >= 0 && sig[32 + i] == order[i])
        --i;
    if (i < 0 || sig[32 + i] > order[i])
        return false;
    ge_p3 point;
    uint8_t encoded[32];
    if (!x25519_ge_frombytes_vartime(&point, key))
        return false;
    ge_p3_tobytes(encoded, &point);
    if (memcmp(encoded, key, 32))
        return false;
    // Reject identity and keys outside the prime-order subgroup.
    static const uint8_t identity[32] = {1}, zero[32] = {0};
    ge_p2 result;
    if (!memcmp(key, identity, 32))
        return false;
    ge_double_scalarmult_vartime(&result, order, &point, zero);
    x25519_ge_tobytes(encoded, &result);
    if (memcmp(encoded, identity, 32))
        return false;
    x25519_sc_reduce(hash);
    fe_loose neg;
    fe_neg(&neg, &point.X);
    fe_carry(&point.X, &neg);
    fe_neg(&neg, &point.T);
    fe_carry(&point.T, &neg);
    ge_double_scalarmult_vartime(&result, hash, &point, sig + 32);
    x25519_ge_tobytes(encoded, &result);
    return memcmp(encoded, sig, 32) == 0;
}

static int by_name(const void *left, const void *right)
{
    const pack_member_t *const *a = left, *const *b = right;
    return strcmp((*a)->name, (*b)->name);
}

esp_err_t pack_publisher_verify(const pack_archive_t *archive, const uint8_t signature[64], uint8_t fingerprint[32])
{
    static const uint8_t domain[] = "KVM-PACK-SIG\0\1";
    const pack_member_t *sorted[PACK_ENTRY_LIMIT];
    unsigned count = 0;
    size_t size = 64 + sizeof(domain) - 1;
    for (unsigned i = 0; i < archive->end; ++i) {
        if (i == PACK_INTEGRITY || i == PACK_SIGNATURE) continue;
        sorted[count++] = &archive->member[i];
        size += 2 + strlen(archive->member[i].name) + 4 + 32;
    }
    qsort(sorted, count, sizeof(sorted[0]), by_name);
    uint8_t *input = pack_alloc(size);
    if (!input) return ESP_ERR_NO_MEM;
    memcpy(input, signature, 32);
    memcpy(input + 64, domain, sizeof(domain)-1);
    size_t used = 64 + sizeof(domain)-1;
    for (unsigned i = 0; i < count; ++i) {
        const pack_member_t *m = sorted[i];
        size_t n = strlen(m->name);
        input[used++] = (uint8_t)n;
        input[used++] = 0;
        memcpy(input + used, m->name, n);
        used += n;
        for (unsigned j = 0; j < 4; ++j) input[used++] = (uint8_t)((uint32_t)m->size >> (8*j));
        memcpy(input + used, m->digest, 32);
        used += 32;
    }
    esp_err_t status = ESP_ERR_INVALID_RESPONSE;
    for (unsigned i = 0; i < sizeof(pack_publisher_keys)/sizeof(pack_publisher_keys[0]); ++i) {
        const uint8_t *key = pack_publisher_keys[i];
        uint8_t hash[64];
        memcpy(input + 32, key, 32);
        if (!pack_sha512(input, size, hash)) {
            status = ESP_FAIL;
            break;
        }
        if (verify(key, signature, hash)) {
            status = pack_sha256(key, 32, fingerprint) ? ESP_OK : ESP_FAIL;
            break;
        }
    }
    pack_free(input);
    return status;
}
