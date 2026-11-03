// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Deterministic random source and a hand-driven clock for the host pairing tests.
//
// Both the pairing core and PSA crypto draw from the same generator here, so seeding it fixes every
// byte of a pairing run: the code, the J-PAKE ephemeral scalars, the token, and therefore the exact
// message bytes the interop vectors record. This generator is a test fixture; it must never reach
// the firmware or the CLIENT, which use esp_random() and BCryptGenRandom.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing_platform.h"

// Seed the generator. The stream is a SHA-256 based counter mode over the seed, so the same seed
// always produces the same bytes on every platform and compiler.
void det_rng_seed(uint64_t seed);

// Make the next `n` draws fail, to exercise the fail-closed paths.
void det_rng_fail_next(unsigned n);

// How many bytes the generator has produced since the last seeding. Useful to confirm that a code
// path drew nothing when it should not have.
uint64_t det_rng_drawn(void);

// The hand-driven clock. Starts at 1000 ms so that 0 is never a valid timestamp.
void det_clock_reset(void);
void det_clock_advance(uint64_t ms);
uint64_t det_clock_now(void);

// A platform block wired to both fixtures. `unix_time` returns a fixed stamp.
const kvm_pair_platform_t *det_platform(void);
