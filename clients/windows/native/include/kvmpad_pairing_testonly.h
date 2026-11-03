// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Extra exports of kvmpad_pairing_test.dll, which is a TEST FIXTURE AND IS NEVER SHIPPED.
//
// Why a second DLL rather than a flag on the first: this one replaces the random source with the
// seeded generator of test/pairing/det_rng.c and links the DEVICE state machine as well as the
// CLIENT. Both belong in a test and neither belongs in an application that controls someone's
// computer, so they are kept out of kvmpad_pairing.dll by construction rather than by a runtime
// check that could be flipped. det_rng.h says the same thing about its generator.
//
// What it buys: the interop vectors of ADR-029 §5 are reproducible only under a fixed generator
// seed, and only when the DEVICE and the CLIENT draw from the same stream in the same order. This
// DLL runs exactly the sequence that produced the checked-in files -- the shared vector_run() of
// test/pairing/vector_run.h, which the generator and the C test also use -- so the managed tests can
// compare every recorded byte, and a changed label or layout in the core breaks the CLIENT build
// instead of breaking a user's pairing.
#ifndef KVMPAD_PAIRING_TESTONLY_H
#define KVMPAD_PAIRING_TESTONLY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define KVMPAD_TEST_ABI __declspec(dllexport)
#else
#define KVMPAD_TEST_ABI
#endif

// Fields of the last run, for kvmpad_test_vector_field().
typedef enum {
    KVMPAD_VF_CODE = 1,          // the 6 digits the DEVICE drew, as ASCII
    KVMPAD_VF_CERT_SHA256 = 2,
    KVMPAD_VF_CLIENT_ID = 3,
    KVMPAD_VF_DEVICE_ROUND1 = 4,
    KVMPAD_VF_CLIENT_ROUND1 = 5,
    KVMPAD_VF_DEVICE_ROUND2 = 6,
    KVMPAD_VF_CLIENT_ROUND2 = 7,
    KVMPAD_VF_CLIENT_CONFIRM = 8,
    KVMPAD_VF_DEVICE_CONFIRM = 9,
    KVMPAD_VF_TOKEN_SEALED = 10,
    KVMPAD_VF_TOKEN = 11,
    KVMPAD_VF_TOKEN_SHA256 = 12, // what the DEVICE stores; never the token itself (SEC-105)
    KVMPAD_VF_TRANSCRIPT = 13
} kvmpad_vector_field_t;

// Run one recorded case end to end: a real kvm_pair_session_t DEVICE and a real kvm_pair_client_t
// CLIENT, in this process, over the seeded generator.
//
// `client_code_override` is NULL except for the wrong-code case; `cert_override` is non-zero only
// for the substituted-certificate case. `failed_step` is 0 when the run finished, otherwise the step
// that failed, and `failed_err` is the kvm_pair_err_t it failed with. `completed` says whether the
// DEVICE actually stored a client, which is the only evidence that matters for SEC-102.
//
// Returns KVMPAD_PAIR_OK when the fixture ran -- including when the case was supposed to fail. A
// non-zero return means the fixture itself could not be set up. Keeps the result in static storage,
// so one run at a time; the managed tests drive it from a single collection for that reason.
KVMPAD_TEST_ABI int32_t kvmpad_test_vector_run(uint64_t seed, int32_t require_accept,
                                               const char *client_code_override,
                                               int32_t cert_override, int32_t *failed_step,
                                               int32_t *failed_err, int32_t *completed);

// Copy one field of the last run. Returns the ARG error for an unknown field or too small a buffer,
// and writes the real length through `out_len`.
KVMPAD_TEST_ABI int32_t kvmpad_test_vector_field(int32_t field, uint8_t *out, int32_t out_cap,
                                                 int32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // KVMPAD_PAIRING_TESTONLY_H
