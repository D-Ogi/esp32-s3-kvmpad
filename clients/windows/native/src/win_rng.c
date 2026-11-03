// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Platform hooks for the shipping kvmpad_pairing.dll: randomness, a monotonic clock and wall time
// (kvm_pairing_platform.h).
//
// Two consumers draw from BCryptGenRandom here. The pairing core draws through the platform hook,
// and PSA crypto draws the J-PAKE ephemeral scalars through mbedtls_psa_external_get_random, which
// the pinned tree calls because MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG is set in win_psa_config.h -- the
// same arrangement the firmware uses with esp_random() and the host tests use with their seeded
// fixture. There is no fallback: a failing generator makes the core fail closed with a crypto error
// rather than continue on weak material.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <windows.h>

#include <bcrypt.h>
#include <psa/crypto.h>

#include "kvm_pairing_platform.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

static bool win_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    if (out == NULL) {
        return false;
    }
    if (len == 0u) {
        return true;
    }
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG means no algorithm handle has to be opened or kept, and the
    // call cannot be made to use a weaker provider by anything in this process.
    if (len > (size_t)0xffffffffu) {
        return false;
    }
    const NTSTATUS st =
        BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st == STATUS_SUCCESS;
}

static uint64_t win_now_ms(void *ctx)
{
    (void)ctx;
    // The core requires a monotonic source: every deadline it keeps is a difference of two
    // readings, so a clock that can jump backwards -- as wall time can -- could extend a pairing
    // window. QueryPerformanceCounter is monotonic across the whole system.
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        return (uint64_t)GetTickCount64();
    }
    if (!QueryPerformanceCounter(&now)) {
        return (uint64_t)GetTickCount64();
    }
    // Scale before dividing so that the result is milliseconds and not a truncated count of
    // seconds; the counter is well below the range where this overflows a 64-bit value.
    return (uint64_t)((now.QuadPart / freq.QuadPart) * 1000) +
           (uint64_t)(((now.QuadPart % freq.QuadPart) * 1000) / freq.QuadPart);
}

static uint64_t win_unix_time(void *ctx)
{
    (void)ctx;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    // FILETIME counts 100 ns intervals since 1601-01-01; 11644473600 s separate that from the Unix
    // epoch.
    const uint64_t hundred_ns = v.QuadPart;
    return (hundred_ns / 10000000ull) - 11644473600ull;
}

const kvm_pair_platform_t *kvmpad_pair_platform(void)
{
    static const kvm_pair_platform_t p = {
        .random = win_random,
        .now_ms = win_now_ms,
        .unix_time = win_unix_time,
        .ctx = NULL,
    };
    return &p;
}

// PSA draws the J-PAKE ephemeral scalars through this hook.
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context,
                                             uint8_t *output, size_t output_size,
                                             size_t *output_length)
{
    (void)context;
    if (!win_random(NULL, output, output_size)) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}
