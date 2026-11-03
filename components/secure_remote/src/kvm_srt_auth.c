// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Token presentation decoding (TASK-100 transport; ADR-028 §2, SEC-105). See kvm_srt_auth.h.

#include "kvm_srt_auth.h"

#include <string.h>

static const char b64url_alphabet[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Value of one base64url character, or -1. Written as a lookup over the alphabet rather than a
// range test so that '+' and '/' of the standard alphabet are refused along with everything else.
static int b64url_value(char c)
{
    for (int i = 0; i < 64; i++) {
        if (b64url_alphabet[i] == c) {
            return i;
        }
    }
    return -1;
}

bool kvm_srt_b64url_decode(const char *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (in == NULL || out == NULL) {
        return false;
    }
    // Unpadded base64 never leaves one leftover character: 1 character carries 6 bits, too few for
    // a byte. len % 4 == 1 is therefore not a short group but a malformed one.
    if ((len % 4u) == 1u) {
        return false;
    }
    const size_t need = len / 4u * 3u + (len % 4u == 0u ? 0u : (len % 4u) - 1u);
    if (need > cap) {
        return false;
    }

    size_t o = 0;
    uint32_t acc = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < len; i++) {
        const int v = b64url_value(in[i]);
        if (v < 0) {
            memset(out, 0, cap);
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            out[o++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    // The 2 or 4 bits that do not form a byte must be zero, otherwise two distinct strings would
    // decode to the same bytes and two different headers would present one token.
    if (bits != 0u && (acc & ((1u << bits) - 1u)) != 0u) {
        memset(out, 0, cap);
        return false;
    }
    if (out_len != NULL) {
        *out_len = o;
    }
    return true;
}

bool kvm_srt_b64url_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    if (in == NULL || out == NULL) {
        return false;
    }
    const size_t chars = len / 3u * 4u + (len % 3u == 0u ? 0u : (len % 3u) + 1u);
    if (cap < chars + 1u) {
        return false;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= len) {
        const uint32_t w = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = b64url_alphabet[(w >> 18) & 0x3Fu];
        out[o++] = b64url_alphabet[(w >> 12) & 0x3Fu];
        out[o++] = b64url_alphabet[(w >> 6) & 0x3Fu];
        out[o++] = b64url_alphabet[w & 0x3Fu];
        i += 3u;
    }
    if (len - i == 1u) {
        const uint32_t w = (uint32_t)in[i] << 16;
        out[o++] = b64url_alphabet[(w >> 18) & 0x3Fu];
        out[o++] = b64url_alphabet[(w >> 12) & 0x3Fu];
    } else if (len - i == 2u) {
        const uint32_t w = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = b64url_alphabet[(w >> 18) & 0x3Fu];
        out[o++] = b64url_alphabet[(w >> 12) & 0x3Fu];
        out[o++] = b64url_alphabet[(w >> 6) & 0x3Fu];
    }
    out[o] = '\0';
    return true;
}

kvm_srt_auth_err_t kvm_srt_auth_parse_bearer(const char *header, uint8_t out[KVM_PAIR_TOKEN_LEN])
{
    if (out != NULL) {
        memset(out, 0, KVM_PAIR_TOKEN_LEN);
    }
    if (out == NULL) {
        return KVM_SRT_AUTH_ERR_ENCODING;
    }
    if (header == NULL || header[0] == '\0') {
        return KVM_SRT_AUTH_ERR_ABSENT;
    }

    const char *p = header;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const size_t scheme_len = sizeof(KVM_SRT_AUTH_SCHEME) - 1u;
    if (strncmp(p, KVM_SRT_AUTH_SCHEME, scheme_len) != 0) {
        return KVM_SRT_AUTH_ERR_SCHEME;
    }
    p += scheme_len;

    // Exactly the 43 characters of a 32-byte token, and nothing after them. A longer or shorter
    // credential is refused before it reaches the register, so the walk below always costs the
    // same.
    if (strlen(p) != KVM_SRT_TOKEN_B64URL_LEN) {
        return KVM_SRT_AUTH_ERR_ENCODING;
    }
    size_t n = 0;
    if (!kvm_srt_b64url_decode(p, KVM_SRT_TOKEN_B64URL_LEN, out, KVM_PAIR_TOKEN_LEN, &n) ||
        n != KVM_PAIR_TOKEN_LEN) {
        memset(out, 0, KVM_PAIR_TOKEN_LEN);
        return KVM_SRT_AUTH_ERR_ENCODING;
    }
    return KVM_SRT_AUTH_OK;
}

const char *kvm_srt_auth_err_name(kvm_srt_auth_err_t err)
{
    switch (err) {
    case KVM_SRT_AUTH_OK:
        return "OK";
    case KVM_SRT_AUTH_ERR_ABSENT:
        return "ABSENT";
    case KVM_SRT_AUTH_ERR_SCHEME:
        return "SCHEME";
    case KVM_SRT_AUTH_ERR_ENCODING:
        return "ENCODING";
    default:
        return "UNKNOWN";
    }
}
