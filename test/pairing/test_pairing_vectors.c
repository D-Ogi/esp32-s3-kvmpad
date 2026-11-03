// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Re-runs every checked-in interop vector and compares it field by field (AT-EXT-13). This is what
// keeps the vectors honest: a changed label, a changed blob layout or a changed constant breaks this
// suite instead of silently breaking the Windows CLIENT.
//
// The vector directory comes from KVM_PAIRING_VECTORS, which the CMake test property sets.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvm_test.h"
#include "vector_run.h"

// Same list as gen_pairing_vectors.c; kept here so a vector cannot quietly stop being checked.
static const vector_case_t k_cases[] = {
    { .name = "pair-success-require-accept", .seed = 0x4b564d50u, .require_accept = true },
    { .name = "pair-success-auto-accept", .seed = 0x4b564d51u, .require_accept = false },
    { .name = "pair-wrong-code",
      .seed = 0x4b564d52u,
      .require_accept = true,
      .client_code_override = "000000" },
    { .name = "pair-cert-mismatch",
      .seed = 0x4b564d53u,
      .require_accept = true,
      .cert_override = true },
};

#define CASE_COUNT (sizeof(k_cases) / sizeof(k_cases[0]))

static char s_json[16384];

static bool load_vector(const char *name)
{
    const char *dir = getenv("KVM_PAIRING_VECTORS");
    if (dir == NULL) {
        dir = "vectors";
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    const size_t got = fread(s_json, 1, sizeof(s_json) - 1u, f);
    fclose(f);
    s_json[got] = '\0';
    return got > 0;
}

// Pull a string value out of the loaded JSON. The files are produced by gen_pairing_vectors.c, so a
// literal `"key": "value"` search is enough and keeps a JSON parser out of the test.
static bool json_str(const char *key, char *out, size_t cap)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\": \"", key);
    const char *at = strstr(s_json, needle);
    if (at == NULL) {
        return false;
    }
    at += strlen(needle);
    const char *end = strchr(at, '"');
    if (end == NULL || (size_t)(end - at) >= cap) {
        return false;
    }
    memcpy(out, at, (size_t)(end - at));
    out[end - at] = '\0';
    return true;
}

static bool json_has(const char *fragment)
{
    return strstr(s_json, fragment) != NULL;
}

// Compare one base64 field against live bytes.
static void expect_b64(const char *key, const uint8_t *data, size_t len)
{
    char want[1024];
    if (!json_str(key, want, sizeof(want))) {
        kvm_test_checks++;
        kvm_test_failures++;
        fprintf(stderr, "vector field missing: %s\n", key);
        return;
    }
    char got[1024];
    CHECK(kvm_pair_b64_encode(data, len, got, sizeof(got)));
    kvm_test_checks++;
    if (strcmp(want, got) != 0) {
        kvm_test_failures++;
        fprintf(stderr, "%s mismatch\n  vector: %s\n  live:   %s\n", key, want, got);
    }
}

static void expect_hex(const char *key, const uint8_t *data, size_t len)
{
    char want[128];
    if (!json_str(key, want, sizeof(want))) {
        kvm_test_checks++;
        kvm_test_failures++;
        fprintf(stderr, "vector field missing: %s\n", key);
        return;
    }
    char got[128];
    CHECK(kvm_pair_hex_encode(data, len, got, sizeof(got)));
    kvm_test_checks++;
    if (strcmp(want, got) != 0) {
        kvm_test_failures++;
        fprintf(stderr, "%s mismatch\n  vector: %s\n  live:   %s\n", key, want, got);
    }
}

static void expect_str(const char *key, const char *value)
{
    char want[128];
    if (!json_str(key, want, sizeof(want))) {
        kvm_test_checks++;
        kvm_test_failures++;
        fprintf(stderr, "vector field missing: %s\n", key);
        return;
    }
    kvm_test_checks++;
    if (strcmp(want, value) != 0) {
        kvm_test_failures++;
        fprintf(stderr, "%s mismatch\n  vector: %s\n  live:   %s\n", key, want, value);
    }
}

static void check_case(const vector_case_t *vc)
{
    printf("    %s\n", vc->name);
    if (!load_vector(vc->name)) {
        kvm_test_checks++;
        kvm_test_failures++;
        return;
    }

    vector_result_t r;
    CHECK(vector_run(vc, &r));

    expect_str("pake", KVM_PAIR_PAKE_NAME);
    expect_str("format", "kvmpad-pairing-vectors/1");
    expect_str("device_code", r.code);
    expect_str("client_name", VECTOR_CLIENT_NAME);
    expect_str("device_name", VECTOR_DEVICE_NAME);
    expect_hex("device_cert_sha256", r.cert_sha, sizeof(r.cert_sha));
    expect_hex("client_id", r.client_id, sizeof(r.client_id));
    expect_str("failed_error", kvm_pair_err_name(r.failed_err));

    char step[32];
    CHECK(json_has("\"failed_step\":"));
    snprintf(step, sizeof(step), "\"failed_step\": %d", r.failed_step);
    kvm_test_checks++;
    if (!json_has(step)) {
        kvm_test_failures++;
        fprintf(stderr, "failed_step mismatch, live value %d\n", r.failed_step);
    }

    if (r.failed_step != 0) {
        // A vector that is supposed to fail must not carry any key material.
        CHECK(!json_has("\"token\""));
        CHECK(!json_has("\"token_sealed\""));
        CHECK(json_has("\"completed\": false"));
        return;
    }

    CHECK(json_has("\"completed\": true"));
    expect_b64("device_round1", r.d_r1, r.d_r1_len);
    expect_b64("client_round1", r.c_r1, r.c_r1_len);
    expect_b64("device_round2", r.d_r2, r.d_r2_len);
    expect_b64("client_round2", r.c_r2, r.c_r2_len);
    expect_hex("transcript_sha256", r.transcript, sizeof(r.transcript));
    expect_b64("client_confirm", r.c_tag, sizeof(r.c_tag));
    expect_b64("device_confirm", r.d_tag, sizeof(r.d_tag));
    expect_b64("token_sealed", r.sealed, r.sealed_len);
    expect_hex("token", r.token, sizeof(r.token));
    expect_hex("token_sha256_stored", r.token_sha, sizeof(r.token_sha));

    // Shape checks the CLIENT implementer can rely on (ADR-029 §4).
    CHECK_EQ(r.d_r1_len, r.c_r1_len);
    CHECK(r.d_r1_len <= KVM_PAIR_ROUND1_MAX);
    CHECK(r.d_r2_len <= KVM_PAIR_ROUND2_MAX);
    CHECK_EQ(r.sealed_len, KVM_PAIR_TOKEN_SEALED_LEN);
    // The two confirmation tags are different values: they use different derivation labels.
    CHECK(memcmp(r.c_tag, r.d_tag, KVM_PAIR_CONFIRM_LEN) != 0);
}

static void test_runs_are_reproducible(void)
{
    // The same seed twice gives the same bytes; a different seed does not. Without this, a vector
    // file would prove nothing about reproducibility.
    vector_result_t a, b, c;
    vector_case_t vc = k_cases[0];
    CHECK(vector_run(&vc, &a));
    CHECK(vector_run(&vc, &b));
    CHECK_EQ(a.failed_step, 0);
    CHECK(memcmp(a.d_r1, b.d_r1, a.d_r1_len) == 0);
    CHECK(memcmp(a.token, b.token, sizeof(a.token)) == 0);
    CHECK(strcmp(a.code, b.code) == 0);

    vc.seed ^= 0xffffu;
    CHECK(vector_run(&vc, &c));
    CHECK(memcmp(a.d_r1, c.d_r1, a.d_r1_len) != 0);
    CHECK(memcmp(a.token, c.token, sizeof(a.token)) != 0);
}

static void test_vector_files(void)
{
    for (size_t i = 0; i < CASE_COUNT; i++) {
        check_case(&k_cases[i]);
    }
}

int main(void)
{
    printf("test_pairing_vectors\n");
    kvm_pake_set_platform(det_platform());
    if (kvm_pake_crypto_init() != KVM_PAIR_OK) {
        fprintf(stderr, "psa_crypto_init failed\n");
        return 1;
    }
    RUN(test_runs_are_reproducible);
    RUN(test_vector_files);
    TEST_MAIN_END("test_pairing_vectors");
}
