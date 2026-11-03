// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Writes the pairing interop vectors (ADR-029 §6) as JSON. They are checked in, so this tool only
// needs running when the wire format, a derivation label or a constant changes -- and
// test_pairing_vectors then proves the checked-in files still match a live run.
//
//   gen_pairing_vectors <output directory>
//
// The vectors are what a CLIENT implementation tests against: feed the recorded code, certificate
// hash, client identifier and client name, and every recorded byte must come out. The fixed RNG
// stream that makes a run reproducible is a test fixture (det_rng.c); a real DEVICE and a real
// CLIENT draw from esp_random() and BCryptGenRandom, so their blobs differ every run while the
// derivation of the keys, tags and token from those blobs stays exactly the same.

#include <stdio.h>
#include <string.h>

#include "kvm_pairing_wire.h"
#include "vector_run.h"

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

static void put_b64(FILE *f, const char *key, const uint8_t *data, size_t len, bool comma)
{
    char enc[KVM_PAIR_B64_LEN(KVM_PAIR_ROUND1_MAX) + 1u];
    if (!kvm_pair_b64_encode(data, len, enc, sizeof(enc))) {
        enc[0] = '\0';
    }
    fprintf(f, "    \"%s\": \"%s\"%s\n", key, enc, comma ? "," : "");
}

static void put_hex(FILE *f, const char *key, const uint8_t *data, size_t len, bool comma)
{
    char enc[2u * KVM_PAIR_SHA256_LEN + 1u];
    if (!kvm_pair_hex_encode(data, len, enc, sizeof(enc))) {
        enc[0] = '\0';
    }
    fprintf(f, "    \"%s\": \"%s\"%s\n", key, enc, comma ? "," : "");
}

static int write_case(const char *dir, const vector_case_t *vc)
{
    vector_result_t r;
    if (!vector_run(vc, &r)) {
        fprintf(stderr, "%s: fixture setup failed\n", vc->name);
        return 1;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", dir, vc->name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }

    const bool ok = r.failed_step == 0;

    fprintf(f, "{\n");
    fprintf(f, "  \"vector\": \"%s\",\n", vc->name);
    fprintf(f, "  \"format\": \"kvmpad-pairing-vectors/1\",\n");
    fprintf(f, "  \"pake\": \"%s\",\n", KVM_PAIR_PAKE_NAME);
    fprintf(f, "  \"wire_version\": %u,\n", (unsigned)KVM_PAIR_WIRE_VERSION);
    fprintf(f, "  \"note\": \"Deterministic run from a fixed test generator seed; a live DEVICE "
               "and CLIENT draw real randomness, so only the derivation of the tags, keys and "
               "sealed token from the recorded blobs is reproducible outside this fixture.\",\n");
    fprintf(f, "  \"input\": {\n");
    fprintf(f, "    \"generator_seed\": %llu,\n", (unsigned long long)vc->seed);
    fprintf(f, "    \"require_accept\": %s,\n", vc->require_accept ? "true" : "false");
    fprintf(f, "    \"device_name\": \"%s\",\n", VECTOR_DEVICE_NAME);
    fprintf(f, "    \"client_name\": \"%s\",\n", VECTOR_CLIENT_NAME);
    fprintf(f, "    \"device_code\": \"%s\",\n", r.code);
    fprintf(f, "    \"client_code\": \"%s\",\n",
            vc->client_code_override != NULL ? vc->client_code_override : r.code);
    fprintf(f, "    \"cert_text\": \"%s\",\n", VECTOR_CERT_TEXT);
    put_hex(f, "device_cert_sha256", r.cert_sha, sizeof(r.cert_sha), true);
    if (vc->cert_override) {
        uint8_t other[KVM_PAIR_SHA256_LEN];
        memset(other, 0x7e, sizeof(other));
        put_hex(f, "client_cert_sha256", other, sizeof(other), true);
    } else {
        put_hex(f, "client_cert_sha256", r.cert_sha, sizeof(r.cert_sha), true);
    }
    put_hex(f, "client_id", r.client_id, sizeof(r.client_id), false);
    fprintf(f, "  },\n");

    fprintf(f, "  \"expect\": {\n");
    fprintf(f, "    \"completed\": %s,\n", r.completed ? "true" : "false");
    fprintf(f, "    \"failed_step\": %d,\n", r.failed_step);
    fprintf(f, "    \"failed_error\": \"%s\"%s\n", kvm_pair_err_name(r.failed_err),
            ok ? "," : "");
    if (ok) {
        put_b64(f, "device_round1", r.d_r1, r.d_r1_len, true);
        put_b64(f, "client_round1", r.c_r1, r.c_r1_len, true);
        put_b64(f, "device_round2", r.d_r2, r.d_r2_len, true);
        put_b64(f, "client_round2", r.c_r2, r.c_r2_len, true);
        put_hex(f, "transcript_sha256", r.transcript, sizeof(r.transcript), true);
        put_b64(f, "client_confirm", r.c_tag, sizeof(r.c_tag), true);
        put_b64(f, "device_confirm", r.d_tag, sizeof(r.d_tag), true);
        put_b64(f, "token_sealed", r.sealed, r.sealed_len, true);
        put_hex(f, "token", r.token, sizeof(r.token), true);
        put_hex(f, "token_sha256_stored", r.token_sha, sizeof(r.token_sha), false);
    }
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);
    printf("wrote %s (%s)\n", path, ok ? "complete run" : kvm_pair_err_name(r.failed_err));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: gen_pairing_vectors <output directory>\n");
        return 2;
    }
    int bad = 0;
    for (size_t i = 0; i < CASE_COUNT; i++) {
        bad += write_case(argv[1], &k_cases[i]);
    }
    return bad == 0 ? 0 : 1;
}
