// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Token presentation on the upgrade request (ADR-028 §2, 03 §6.1, TECH-042, SEC-105).
// Covers AT-SEC-04 (malformed credentials fail closed) and the AT-SEC-08 review item that no
// function here returns anything derived from the token.

#include <string.h>

#include "kvm_srt_auth.h"
#include "kvm_test.h"

static void t_b64url_roundtrip(void)
{
    uint8_t in[KVM_PAIR_TOKEN_LEN];
    for (unsigned i = 0; i < sizeof(in); i++) {
        in[i] = (uint8_t)(i * 7u + 1u);
    }
    char enc[KVM_SRT_TOKEN_B64URL_CAP];
    CHECK(kvm_srt_b64url_encode(in, sizeof(in), enc, sizeof(enc)));
    CHECK_EQ(strlen(enc), KVM_SRT_TOKEN_B64URL_LEN);
    // No padding and no character outside the URL-safe alphabet.
    CHECK(strchr(enc, '=') == NULL);
    CHECK(strchr(enc, '+') == NULL);
    CHECK(strchr(enc, '/') == NULL);

    uint8_t out[KVM_PAIR_TOKEN_LEN];
    size_t n = 0;
    CHECK(kvm_srt_b64url_decode(enc, strlen(enc), out, sizeof(out), &n));
    CHECK_EQ(n, KVM_PAIR_TOKEN_LEN);
    CHECK_EQ(memcmp(in, out, sizeof(in)), 0);
}

static void t_b64url_lengths(void)
{
    // Every input length from 0 to 48 encodes and decodes back to itself.
    for (size_t len = 0; len <= 48u; len++) {
        uint8_t in[48];
        uint8_t back[48];
        char enc[80];
        size_t n = 0;
        for (size_t i = 0; i < len; i++) {
            in[i] = (uint8_t)(i ^ 0x5Au);
        }
        CHECK(kvm_srt_b64url_encode(in, len, enc, sizeof(enc)));
        CHECK(kvm_srt_b64url_decode(enc, strlen(enc), back, sizeof(back), &n));
        CHECK_EQ(n, len);
        CHECK_EQ(memcmp(in, back, len), 0);
    }
}

static void t_b64url_rejects(void)
{
    uint8_t out[64];
    size_t n = 0;

    // Padding is not part of the format.
    CHECK(!kvm_srt_b64url_decode("QQ==", 4u, out, sizeof(out), &n));
    // Standard-alphabet characters are not URL-safe characters.
    CHECK(!kvm_srt_b64url_decode("a+bc", 4u, out, sizeof(out), &n));
    CHECK(!kvm_srt_b64url_decode("a/bc", 4u, out, sizeof(out), &n));
    // A single leftover character cannot carry a byte.
    CHECK(!kvm_srt_b64url_decode("QUJDQ", 5u, out, sizeof(out), &n));
    // Whitespace is never accepted; the CLIENT emits none.
    CHECK(!kvm_srt_b64url_decode("QU J", 4u, out, sizeof(out), &n));
    // A result that does not fit is refused rather than truncated.
    CHECK(!kvm_srt_b64url_decode("QUJDRA", 6u, out, 2u, &n));
    CHECK_EQ(n, 0);

    // Non-canonical trailing bits: "QQ" is 'A' with the low 4 bits zero. "QR" decodes to the same
    // byte but sets bits that are not part of any byte, so two header values would present one
    // token. Refuse it.
    CHECK(kvm_srt_b64url_decode("QQ", 2u, out, sizeof(out), &n));
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0], 'A');
    CHECK(!kvm_srt_b64url_decode("QR", 2u, out, sizeof(out), &n));

    // The two URL-safe characters are accepted: '-' is 62 and '_' is 63.
    CHECK(kvm_srt_b64url_decode("-_-_", 4u, out, sizeof(out), &n));
    CHECK_EQ(n, 3);
    CHECK_EQ(out[0], 0xFBu);
    CHECK_EQ(out[1], 0xFFu);
    CHECK_EQ(out[2], 0xBFu);
}

static void t_bearer_happy(void)
{
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    for (unsigned i = 0; i < sizeof(token); i++) {
        token[i] = (uint8_t)(0x10u + i);
    }
    char enc[KVM_SRT_TOKEN_B64URL_CAP];
    CHECK(kvm_srt_b64url_encode(token, sizeof(token), enc, sizeof(enc)));

    char header[80];
    snprintf(header, sizeof(header), "Bearer %s", enc);

    uint8_t got[KVM_PAIR_TOKEN_LEN];
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_OK);
    CHECK_EQ(memcmp(token, got, sizeof(token)), 0);

    // Optional leading whitespace is allowed by RFC 9110 and must not change the result.
    snprintf(header, sizeof(header), "  \tBearer %s", enc);
    memset(got, 0, sizeof(got));
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_OK);
    CHECK_EQ(memcmp(token, got, sizeof(token)), 0);
}

static void t_bearer_rejects(void)
{
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    memset(token, 0x33, sizeof(token));
    char enc[KVM_SRT_TOKEN_B64URL_CAP];
    CHECK(kvm_srt_b64url_encode(token, sizeof(token), enc, sizeof(enc)));

    uint8_t got[KVM_PAIR_TOKEN_LEN];
    char header[128];

    CHECK_EQ(kvm_srt_auth_parse_bearer(NULL, got), KVM_SRT_AUTH_ERR_ABSENT);
    CHECK_EQ(kvm_srt_auth_parse_bearer("", got), KVM_SRT_AUTH_ERR_ABSENT);

    // Scheme must be exactly "Bearer " -- not Basic, not lower case, not without the space.
    snprintf(header, sizeof(header), "Basic %s", enc);
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_SCHEME);
    snprintf(header, sizeof(header), "bearer %s", enc);
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_SCHEME);
    snprintf(header, sizeof(header), "Bearer%s", enc);
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_SCHEME);

    // Wrong credential length, in both directions.
    snprintf(header, sizeof(header), "Bearer %.*s", (int)KVM_SRT_TOKEN_B64URL_LEN - 1, enc);
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_ENCODING);
    snprintf(header, sizeof(header), "Bearer %sA", enc);
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_ENCODING);
    // A trailing space makes the credential 44 characters: refused, not trimmed.
    snprintf(header, sizeof(header), "Bearer %s ", enc);
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_ENCODING);
    // Two credentials.
    snprintf(header, sizeof(header), "Bearer %s, Bearer %s", enc, enc);
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_ENCODING);

    // A bad character at the right length.
    snprintf(header, sizeof(header), "Bearer %s", enc);
    header[7] = '!';
    CHECK_EQ(kvm_srt_auth_parse_bearer(header, got), KVM_SRT_AUTH_ERR_ENCODING);
}

static void t_bearer_zeroizes_on_failure(void)
{
    uint8_t got[KVM_PAIR_TOKEN_LEN];
    memset(got, 0xEE, sizeof(got));
    CHECK_EQ(kvm_srt_auth_parse_bearer("Bearer nope", got), KVM_SRT_AUTH_ERR_ENCODING);
    // SEC-105: a refused credential leaves nothing behind for the caller to use by accident.
    uint8_t zero[KVM_PAIR_TOKEN_LEN];
    memset(zero, 0, sizeof(zero));
    CHECK_EQ(memcmp(got, zero, sizeof(zero)), 0);

    memset(got, 0xEE, sizeof(got));
    CHECK_EQ(kvm_srt_auth_parse_bearer(NULL, got), KVM_SRT_AUTH_ERR_ABSENT);
    CHECK_EQ(memcmp(got, zero, sizeof(zero)), 0);
}

static void t_err_names(void)
{
    // Names are diagnostics only and must never embed a credential.
    CHECK_EQ(strcmp(kvm_srt_auth_err_name(KVM_SRT_AUTH_OK), "OK"), 0);
    CHECK_EQ(strcmp(kvm_srt_auth_err_name(KVM_SRT_AUTH_ERR_ABSENT), "ABSENT"), 0);
    CHECK_EQ(strcmp(kvm_srt_auth_err_name(KVM_SRT_AUTH_ERR_SCHEME), "SCHEME"), 0);
    CHECK_EQ(strcmp(kvm_srt_auth_err_name(KVM_SRT_AUTH_ERR_ENCODING), "ENCODING"), 0);
}

static void t_constants(void)
{
    // 32 bytes in base64url without padding is 43 characters; ADR-028 §2 states that length and
    // the CLIENT relies on it.
    CHECK_EQ(KVM_SRT_TOKEN_B64URL_LEN, 43);
    CHECK_EQ(KVM_PAIR_TOKEN_LEN, 32);
    // The URIs and subprotocols of ADR-028 §2, byte for byte.
    CHECK_EQ(strcmp(KVM_SRT_URI_CONTROL, "/v1/ws"), 0);
    CHECK_EQ(strcmp(KVM_SRT_URI_PAIRING, "/v1/pair"), 0);
    CHECK_EQ(strcmp(KVM_SRT_SUBPROTO_CONTROL, "kvmpad.v1"), 0);
    CHECK_EQ(strcmp(KVM_SRT_SUBPROTO_PAIRING, "kvmpad.pair.v1"), 0);
}

int main(void)
{
    RUN(t_b64url_roundtrip);
    RUN(t_b64url_lengths);
    RUN(t_b64url_rejects);
    RUN(t_bearer_happy);
    RUN(t_bearer_rejects);
    RUN(t_bearer_zeroizes_on_failure);
    RUN(t_err_names);
    RUN(t_constants);
    TEST_MAIN_END("test_srt_auth");
}
