// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Deterministic random source and hand-driven clock for the host pairing tests (see det_rng.h).

#include "det_rng.h"

#include <psa/crypto.h>
#include <string.h>

#define FIXED_UNIX_TIME 1767225600ull // 2026-01-01T00:00:00Z, so a stored stamp is reproducible

static uint64_t s_seed;
static uint64_t s_counter;
static uint64_t s_drawn;
static unsigned s_fail_next;

static uint8_t s_block[64];
static size_t s_block_left;

static uint64_t s_now_ms = 1000;

// The stream is the ChaCha20 block function in counter mode over the seed. It is implemented here
// rather than called through PSA for two reasons: PSA asks for entropy while psa_crypto_init() is
// still running, and re-entering PSA from inside its own random hook is not something the pinned
// library promises to survive.

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void quarter(uint32_t *st, int a, int b, int c, int d)
{
    st[a] += st[b]; st[d] ^= st[a]; st[d] = ROTL32(st[d], 16);
    st[c] += st[d]; st[b] ^= st[c]; st[b] = ROTL32(st[b], 12);
    st[a] += st[b]; st[d] ^= st[a]; st[d] = ROTL32(st[d], 8);
    st[c] += st[d]; st[b] ^= st[c]; st[b] = ROTL32(st[b], 7);
}

static void refill(void)
{
    static const uint32_t sigma[4] = { 0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u };
    uint32_t st[16];
    uint32_t work[16];

    st[0] = sigma[0]; st[1] = sigma[1]; st[2] = sigma[2]; st[3] = sigma[3];
    // Key = the 64-bit seed repeated, so a seed fully determines the stream.
    for (int i = 0; i < 4; i++) {
        st[4 + i] = (uint32_t)(s_seed >> (32 * (i & 1)));
        st[8 + i] = (uint32_t)(s_seed >> (32 * ((i + 1) & 1))) ^ 0x9e3779b9u;
    }
    st[12] = (uint32_t)s_counter;
    st[13] = (uint32_t)(s_counter >> 32);
    st[14] = 0x6b766d70u; // "kvmp"
    st[15] = 0x61646574u; // "adet"

    memcpy(work, st, sizeof(work));
    for (int round = 0; round < 10; round++) {
        quarter(work, 0, 4, 8, 12);
        quarter(work, 1, 5, 9, 13);
        quarter(work, 2, 6, 10, 14);
        quarter(work, 3, 7, 11, 15);
        quarter(work, 0, 5, 10, 15);
        quarter(work, 1, 6, 11, 12);
        quarter(work, 2, 7, 8, 13);
        quarter(work, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) {
        const uint32_t v = work[i] + st[i];
        s_block[i * 4 + 0] = (uint8_t)(v & 0xffu);
        s_block[i * 4 + 1] = (uint8_t)((v >> 8) & 0xffu);
        s_block[i * 4 + 2] = (uint8_t)((v >> 16) & 0xffu);
        s_block[i * 4 + 3] = (uint8_t)((v >> 24) & 0xffu);
    }
    s_block_left = sizeof(s_block);
    s_counter++;
}

static bool det_fill(uint8_t *out, size_t len)
{
    if (s_fail_next > 0) {
        s_fail_next--;
        return false;
    }
    while (len > 0) {
        if (s_block_left == 0) {
            refill();
            if (s_block_left == 0) {
                return false;
            }
        }
        const size_t take = (len < s_block_left) ? len : s_block_left;
        memcpy(out, s_block + (sizeof(s_block) - s_block_left), take);
        s_block_left -= take;
        out += take;
        len -= take;
        s_drawn += take;
    }
    return true;
}

void det_rng_seed(uint64_t seed)
{
    s_seed = seed;
    s_counter = 0;
    s_drawn = 0;
    s_fail_next = 0;
    s_block_left = 0;
    memset(s_block, 0, sizeof(s_block));
}

void det_rng_fail_next(unsigned n)
{
    s_fail_next = n;
}

uint64_t det_rng_drawn(void)
{
    return s_drawn;
}

void det_clock_reset(void)
{
    s_now_ms = 1000;
}

void det_clock_advance(uint64_t ms)
{
    s_now_ms += ms;
}

uint64_t det_clock_now(void)
{
    return s_now_ms;
}

static bool plat_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    return det_fill(out, len);
}

static uint64_t plat_now(void *ctx)
{
    (void)ctx;
    return s_now_ms;
}

static uint64_t plat_unix(void *ctx)
{
    (void)ctx;
    return FIXED_UNIX_TIME;
}

const kvm_pair_platform_t *det_platform(void)
{
    static const kvm_pair_platform_t p = {
        .random = plat_random,
        .now_ms = plat_now,
        .unix_time = plat_unix,
        .ctx = NULL,
    };
    return &p;
}

// PSA draws its J-PAKE ephemeral scalars through this hook, which is why a seeded run is
// byte-for-byte reproducible. The firmware's hook is esp_random(); the CLIENT's is BCryptGenRandom.
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_length)
{
    (void)context;
    if (!det_fill(output, output_size)) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}
