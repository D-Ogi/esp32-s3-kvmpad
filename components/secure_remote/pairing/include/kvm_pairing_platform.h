// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Platform hooks of the pairing core (TASK-100, ADR-022/ADR-029).
//
// The pairing core is plain C on top of the PSA crypto API and nothing else: it has no ESP-IDF,
// FreeRTOS, socket or logging dependency, so the same sources build for the firmware, for the host
// tests and for the Windows CLIENT. Everything the core cannot compute itself -- randomness, time
// and the paired-client register -- arrives through the two interfaces below.
//
// Mapping used by each build:
//   firmware    random = esp_random() / esp_fill_random(), now_ms = esp_timer_get_time()/1000,
//               store  = config_store pairing register (kvm_config_store_pair_*)
//   host tests  random = a seeded test DRBG, clock = a counter the test moves by hand,
//               store  = an in-RAM fake
//   CLIENT      random = BCryptGenRandom, clock = QueryPerformanceCounter / GetSystemTimeAsFileTime,
//               store  = DPAPI / Credential Manager (the CLIENT stores the token, not its hash)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Randomness, time and -- for the paired-at stamp only -- wall clock.
//
// `random` MUST be a cryptographically strong generator: it produces the pairing code, the client
// token and the J-PAKE ephemeral scalars. A generator that returns false makes the core fail closed
// (KVM_PAIR_ERR_CRYPTO) instead of continuing with weak material.
//
// `now_ms` MUST be monotonic; every deadline in the core is a difference of two readings, so a
// wall-clock jump cannot extend a pairing window. `unix_time` is only stamped into a stored record
// and may return 0 when the device has no time source yet.
typedef struct {
    bool (*random)(void *ctx, uint8_t *out, size_t len);
    uint64_t (*now_ms)(void *ctx);
    uint64_t (*unix_time)(void *ctx);
    void *ctx;
} kvm_pair_platform_t;

// One record of the paired-client register as the DEVICE keeps it (FR-726, SEC-105): a display
// name, the SHA-256 of the client token -- never the token -- and when the pairing happened.
typedef struct {
    char name[KVM_PAIR_NAME_CAP];
    uint8_t token_sha256[KVM_PAIR_SHA256_LEN];
    uint64_t paired_at_unix;
} kvm_pair_record_t;

// Persistence of the paired-client register. The integrator maps this onto config_store, which
// already owns the on-flash layout, the immediate (non-debounced) write of a pairing change and the
// factory reset (TASK-090); the core never touches flash itself.
//
// `add` returns the index of the new record, or a negative value when the register is full or the
// write failed. `get` fills `out` and returns false for an out-of-range index. `remove_all` returns
// how many records it dropped; the caller must then revoke the live session and raise RELEASE_ALL.
typedef struct {
    int (*add)(void *ctx, const kvm_pair_record_t *rec);
    uint8_t (*count)(void *ctx);
    bool (*get)(void *ctx, uint8_t index, kvm_pair_record_t *out);
    bool (*remove)(void *ctx, uint8_t index);
    uint8_t (*remove_all)(void *ctx);
    void *ctx;
} kvm_pair_store_t;

#ifdef __cplusplus
}
#endif
