// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the PAKE layer (TASK-100, AT-EXT-13): a successful exchange, a wrong code, a
// substituted certificate, a changed client identifier or name, token sealing, and the fail-closed
// behavior of the random hook.

#include <string.h>

#include "det_rng.h"
#include "kvm_pairing_pake.h"
#include "kvm_test.h"

static void binding_make(kvm_pake_binding_t *b, uint8_t cert_fill, uint8_t id_fill,
                         const char *name)
{
    memset(b, 0, sizeof(*b));
    memset(b->cert_sha256, cert_fill, sizeof(b->cert_sha256));
    memset(b->client_id, id_fill, sizeof(b->client_id));
    size_t i = 0;
    while (i + 1u < KVM_PAIR_NAME_CAP && name[i] != '\0') {
        b->client_name[i] = name[i];
        i++;
    }
    b->client_name[i] = '\0';
}

// Run a full exchange between a DEVICE and a CLIENT context and report whether both key
// confirmations passed. `shared_match` tells the caller whether the transcripts agreed.
static kvm_pair_err_t exchange(const char *dev_code, const kvm_pake_binding_t *dev_bind,
                               const char *cli_code, const kvm_pake_binding_t *cli_bind,
                               bool *transcripts_equal, uint8_t token_out[KVM_PAIR_TOKEN_LEN])
{
    kvm_pake_t dev, cli;
    uint8_t d_r1[KVM_PAIR_ROUND1_MAX], c_r1[KVM_PAIR_ROUND1_MAX];
    uint8_t d_r2[KVM_PAIR_ROUND2_MAX], c_r2[KVM_PAIR_ROUND2_MAX];
    size_t d_r1_len = 0, c_r1_len = 0, d_r2_len = 0, c_r2_len = 0;
    kvm_pair_err_t err;

    if (transcripts_equal != NULL) {
        *transcripts_equal = false;
    }

    err = kvm_pake_init(&dev, KVM_PAKE_ROLE_DEVICE, dev_code, dev_bind);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    err = kvm_pake_init(&cli, KVM_PAKE_ROLE_CLIENT, cli_code, cli_bind);
    if (err != KVM_PAIR_OK) {
        kvm_pake_free(&dev);
        return err;
    }

#define STEP(call)                     \
    do {                               \
        err = (call);                  \
        if (err != KVM_PAIR_OK) {      \
            goto out;                  \
        }                              \
    } while (0)

    STEP(kvm_pake_round1_out(&dev, d_r1, sizeof(d_r1), &d_r1_len));
    STEP(kvm_pake_round1_out(&cli, c_r1, sizeof(c_r1), &c_r1_len));
    STEP(kvm_pake_round1_in(&dev, c_r1, c_r1_len));
    STEP(kvm_pake_round1_in(&cli, d_r1, d_r1_len));
    STEP(kvm_pake_round2_out(&dev, d_r2, sizeof(d_r2), &d_r2_len));
    STEP(kvm_pake_round2_out(&cli, c_r2, sizeof(c_r2), &c_r2_len));
    STEP(kvm_pake_round2_in(&dev, c_r2, c_r2_len));
    STEP(kvm_pake_round2_in(&cli, d_r2, d_r2_len));
    STEP(kvm_pake_finish(&dev));
    STEP(kvm_pake_finish(&cli));

    uint8_t th_d[KVM_PAIR_SHA256_LEN], th_c[KVM_PAIR_SHA256_LEN];
    STEP(kvm_pake_transcript(&dev, th_d));
    STEP(kvm_pake_transcript(&cli, th_c));
    if (transcripts_equal != NULL) {
        *transcripts_equal = memcmp(th_d, th_c, sizeof(th_d)) == 0;
    }

    uint8_t c_tag[KVM_PAIR_CONFIRM_LEN], d_tag[KVM_PAIR_CONFIRM_LEN];
    STEP(kvm_pake_confirm_out(&cli, c_tag));
    STEP(kvm_pake_confirm_verify(&dev, c_tag));
    STEP(kvm_pake_confirm_out(&dev, d_tag));
    STEP(kvm_pake_confirm_verify(&cli, d_tag));

    if (token_out != NULL) {
        uint8_t token[KVM_PAIR_TOKEN_LEN];
        memset(token, 0x5a, sizeof(token));
        uint8_t sealed[KVM_PAIR_TOKEN_SEALED_LEN];
        size_t sealed_len = 0;
        STEP(kvm_pake_token_seal(&dev, token, sealed, sizeof(sealed), &sealed_len));
        STEP(kvm_pake_token_open(&cli, sealed, sealed_len, token_out));
    }
#undef STEP

out:
    kvm_pake_free(&dev);
    kvm_pake_free(&cli);
    return err;
}

static void test_happy_path(void)
{
    det_rng_seed(1);
    kvm_pake_set_platform(det_platform());
    CHECK_EQ(kvm_pake_crypto_init(), KVM_PAIR_OK);

    kvm_pake_binding_t b;
    binding_make(&b, 0x11, 0x22, "DESK1");

    bool equal = false;
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    memset(token, 0, sizeof(token));
    CHECK_EQ(exchange("482913", &b, "482913", &b, &equal, token), KVM_PAIR_OK);
    CHECK(equal);
    // The sealed token opened to the 0x5a filler the exchange helper used.
    uint8_t want[KVM_PAIR_TOKEN_LEN];
    memset(want, 0x5a, sizeof(want));
    CHECK(memcmp(token, want, sizeof(want)) == 0);
}

static void test_wrong_code(void)
{
    det_rng_seed(2);
    kvm_pake_binding_t b;
    binding_make(&b, 0x11, 0x22, "DESK1");

    bool equal = false;
    // The J-PAKE rounds themselves still succeed with a wrong password; only key confirmation
    // detects it, which is exactly why SEC-104 has to count attempts. The transcript still matches
    // on both sides -- it covers the exchanged bytes, not the key -- so the failure can only be the
    // key, and it is reported as one code.
    CHECK_EQ(exchange("482913", &b, "000000", &b, &equal, NULL), KVM_PAIR_ERR_BAD_CODE);
    CHECK(equal);
}

static void test_cert_mismatch(void)
{
    det_rng_seed(3);
    kvm_pake_binding_t dev, cli;
    binding_make(&dev, 0x11, 0x22, "DESK1");
    binding_make(&cli, 0x99, 0x22, "DESK1"); // substituted certificate

    // Same code, different certificate hash: the password differs, so confirmation fails and
    // nothing usable is derived (SEC-102).
    CHECK_EQ(exchange("482913", &dev, "482913", &cli, NULL, NULL), KVM_PAIR_ERR_BAD_CODE);
}

static void test_client_id_and_name_bind(void)
{
    det_rng_seed(4);
    kvm_pake_binding_t dev, cli;

    binding_make(&dev, 0x11, 0x22, "DESK1");
    binding_make(&cli, 0x11, 0x23, "DESK1"); // different client identifier
    CHECK_EQ(exchange("482913", &dev, "482913", &cli, NULL, NULL), KVM_PAIR_ERR_BAD_CODE);

    binding_make(&cli, 0x11, 0x22, "DESK2"); // different client name
    CHECK_EQ(exchange("482913", &dev, "482913", &cli, NULL, NULL), KVM_PAIR_ERR_BAD_CODE);
}

static void test_transcript_is_role_independent(void)
{
    det_rng_seed(5);
    kvm_pake_binding_t b;
    binding_make(&b, 0x11, 0x22, "DESK1");
    bool equal = false;
    CHECK_EQ(exchange("111111", &b, "111111", &b, &equal, NULL), KVM_PAIR_OK);
    CHECK(equal);
}

static void test_bad_arguments(void)
{
    det_rng_seed(6);
    kvm_pake_t p;
    kvm_pake_binding_t b;
    binding_make(&b, 0x11, 0x22, "DESK1");

    CHECK_EQ(kvm_pake_init(NULL, KVM_PAKE_ROLE_DEVICE, "123456", &b), KVM_PAIR_ERR_ARG);
    CHECK_EQ(kvm_pake_init(&p, KVM_PAKE_ROLE_DEVICE, NULL, &b), KVM_PAIR_ERR_ARG);
    CHECK_EQ(kvm_pake_init(&p, KVM_PAKE_ROLE_DEVICE, "123456", NULL), KVM_PAIR_ERR_ARG);
    // Non-digit code.
    CHECK_EQ(kvm_pake_init(&p, KVM_PAKE_ROLE_DEVICE, "12a456", &b), KVM_PAIR_ERR_ARG);
    // Empty client name is not a usable binding.
    kvm_pake_binding_t empty;
    binding_make(&empty, 0x11, 0x22, "");
    CHECK_EQ(kvm_pake_init(&p, KVM_PAKE_ROLE_DEVICE, "123456", &empty), KVM_PAIR_ERR_ARG);
    // Unknown role.
    CHECK_EQ(kvm_pake_init(&p, (kvm_pake_role_t)7, "123456", &b), KVM_PAIR_ERR_ARG);
}

static void test_out_of_order_calls(void)
{
    det_rng_seed(7);
    kvm_pake_t p;
    kvm_pake_binding_t b;
    binding_make(&b, 0x11, 0x22, "DESK1");
    CHECK_EQ(kvm_pake_init(&p, KVM_PAKE_ROLE_DEVICE, "123456", &b), KVM_PAIR_OK);

    uint8_t buf[KVM_PAIR_ROUND1_MAX];
    size_t len = 0;
    // Round 2 before round 1, finish before the rounds, confirm before finish.
    CHECK_EQ(kvm_pake_round2_out(&p, buf, sizeof(buf), &len), KVM_PAIR_ERR_STATE);
    CHECK_EQ(kvm_pake_round1_in(&p, buf, 10), KVM_PAIR_ERR_STATE);
    CHECK_EQ(kvm_pake_finish(&p), KVM_PAIR_ERR_STATE);
    uint8_t tag[KVM_PAIR_CONFIRM_LEN];
    CHECK_EQ(kvm_pake_confirm_out(&p, tag), KVM_PAIR_ERR_STATE);
    CHECK_EQ(kvm_pake_transcript(&p, tag), KVM_PAIR_ERR_STATE);

    CHECK_EQ(kvm_pake_round1_out(&p, buf, sizeof(buf), &len), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round1_out(&p, buf, sizeof(buf), &len), KVM_PAIR_ERR_STATE);
    // A truncated peer blob is a framing error, not a failed code.
    CHECK_EQ(kvm_pake_round1_in(&p, buf, len - 1u), KVM_PAIR_ERR_FRAME);
    // An over-long blob is rejected before it reaches the crypto.
    CHECK_EQ(kvm_pake_round1_in(&p, buf, KVM_PAIR_ROUND1_MAX + 1u), KVM_PAIR_ERR_FRAME);

    kvm_pake_free(&p);
    // Freeing twice, and freeing a zeroized context, must be safe.
    kvm_pake_free(&p);
}

static void test_random_hook_fails_closed(void)
{
    det_rng_seed(8);
    uint8_t buf[8];

    // With no hook installed, a draw must fail rather than fall back to a weak source. The pairing
    // code and the token both go through this path, so the session cannot open a window either
    // (see test_pairing_session.c, test_rng_failure_blocks_window).
    kvm_pake_set_platform(NULL);
    CHECK(!kvm_pake_platform_random(buf, sizeof(buf)));

    // A hook whose generator refuses is reported the same way.
    kvm_pake_set_platform(det_platform());
    det_rng_fail_next(1);
    CHECK(!kvm_pake_platform_random(buf, sizeof(buf)));
    CHECK(kvm_pake_platform_random(buf, sizeof(buf)));
}

static void test_token_tamper(void)
{
    det_rng_seed(9);
    kvm_pake_set_platform(det_platform());
    kvm_pake_binding_t b;
    binding_make(&b, 0x11, 0x22, "DESK1");

    kvm_pake_t dev, cli;
    uint8_t d_r1[KVM_PAIR_ROUND1_MAX], c_r1[KVM_PAIR_ROUND1_MAX];
    uint8_t d_r2[KVM_PAIR_ROUND2_MAX], c_r2[KVM_PAIR_ROUND2_MAX];
    size_t a = 0, bb = 0, c = 0, d = 0;

    CHECK_EQ(kvm_pake_init(&dev, KVM_PAKE_ROLE_DEVICE, "482913", &b), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_init(&cli, KVM_PAKE_ROLE_CLIENT, "482913", &b), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round1_out(&dev, d_r1, sizeof(d_r1), &a), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round1_out(&cli, c_r1, sizeof(c_r1), &bb), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round1_in(&dev, c_r1, bb), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round1_in(&cli, d_r1, a), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round2_out(&dev, d_r2, sizeof(d_r2), &c), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round2_out(&cli, c_r2, sizeof(c_r2), &d), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round2_in(&dev, c_r2, d), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_round2_in(&cli, d_r2, c), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_finish(&dev), KVM_PAIR_OK);
    CHECK_EQ(kvm_pake_finish(&cli), KVM_PAIR_OK);

    uint8_t token[KVM_PAIR_TOKEN_LEN];
    memset(token, 0x33, sizeof(token));
    uint8_t sealed[KVM_PAIR_TOKEN_SEALED_LEN];
    size_t sealed_len = 0;
    CHECK_EQ(kvm_pake_token_seal(&dev, token, sealed, sizeof(sealed), &sealed_len), KVM_PAIR_OK);
    CHECK_EQ(sealed_len, KVM_PAIR_TOKEN_SEALED_LEN);
    // The sealed token must not contain the plaintext.
    CHECK(memcmp(sealed, token, KVM_PAIR_TOKEN_LEN) != 0);

    // Flip one bit of the ciphertext, then one of the tag.
    uint8_t out[KVM_PAIR_TOKEN_LEN];
    uint8_t broken[KVM_PAIR_TOKEN_SEALED_LEN];
    memcpy(broken, sealed, sizeof(broken));
    broken[0] ^= 0x01u;
    CHECK_EQ(kvm_pake_token_open(&cli, broken, sizeof(broken), out), KVM_PAIR_ERR_BAD_CODE);
    memcpy(broken, sealed, sizeof(broken));
    broken[KVM_PAIR_TOKEN_SEALED_LEN - 1u] ^= 0x01u;
    CHECK_EQ(kvm_pake_token_open(&cli, broken, sizeof(broken), out), KVM_PAIR_ERR_BAD_CODE);
    // Wrong length.
    CHECK_EQ(kvm_pake_token_open(&cli, sealed, sealed_len - 1u, out), KVM_PAIR_ERR_FRAME);
    // The intact one still opens.
    CHECK_EQ(kvm_pake_token_open(&cli, sealed, sealed_len, out), KVM_PAIR_OK);
    CHECK(memcmp(out, token, sizeof(token)) == 0);

    kvm_pake_free(&dev);
    kvm_pake_free(&cli);
}

static void test_constant_time_compare(void)
{
    uint8_t a[32], b[32];
    memset(a, 0x5a, sizeof(a));
    memcpy(b, a, sizeof(b));
    CHECK(kvm_pair_equal_ct(a, b, sizeof(a)));
    b[31] ^= 0x01u;
    CHECK(!kvm_pair_equal_ct(a, b, sizeof(a)));
    b[31] ^= 0x01u;
    b[0] ^= 0x80u;
    CHECK(!kvm_pair_equal_ct(a, b, sizeof(a)));
    CHECK(!kvm_pair_equal_ct(NULL, b, sizeof(a)));
    CHECK(kvm_pair_equal_ct(a, b, 0));
}

static void test_sha256_known_vector(void)
{
    uint8_t out[KVM_PAIR_SHA256_LEN];
    CHECK_EQ(kvm_pair_sha256((const uint8_t *)"abc", 3, out), KVM_PAIR_OK);
    static const uint8_t want[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    CHECK(memcmp(out, want, sizeof(want)) == 0);
}

int main(void)
{
    printf("test_pairing_pake\n");
    kvm_pake_set_platform(det_platform());
    if (kvm_pake_crypto_init() != KVM_PAIR_OK) {
        fprintf(stderr, "psa_crypto_init failed\n");
        return 1;
    }
    RUN(test_sha256_known_vector);
    RUN(test_constant_time_compare);
    RUN(test_happy_path);
    RUN(test_wrong_code);
    RUN(test_cert_mismatch);
    RUN(test_client_id_and_name_bind);
    RUN(test_transcript_is_role_independent);
    RUN(test_bad_arguments);
    RUN(test_out_of_order_calls);
    RUN(test_token_tamper);
    RUN(test_random_hook_fails_closed);
    TEST_MAIN_END("test_pairing_pake");
}
