// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Encoding helpers for the `pair.*` messages (TASK-100, ADR-029 §4). Self-contained on purpose: the
// Windows CLIENT links these sources, and the firmware must not depend on a base64 implementation
// that lives in a TLS library.

#include "kvm_pairing_wire.h"

#include <string.h>

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool kvm_pair_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    if (out == NULL || (in == NULL && len != 0)) {
        return false;
    }
    const size_t need = KVM_PAIR_B64_LEN(len) + 1u;
    if (cap < need) {
        return false;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= len) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = k_b64[(v >> 18) & 0x3fu];
        out[o++] = k_b64[(v >> 12) & 0x3fu];
        out[o++] = k_b64[(v >> 6) & 0x3fu];
        out[o++] = k_b64[v & 0x3fu];
        i += 3u;
    }
    const size_t rest = len - i;
    if (rest == 1u) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = k_b64[(v >> 18) & 0x3fu];
        out[o++] = k_b64[(v >> 12) & 0x3fu];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rest == 2u) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = k_b64[(v >> 18) & 0x3fu];
        out[o++] = k_b64[(v >> 12) & 0x3fu];
        out[o++] = k_b64[(v >> 6) & 0x3fu];
        out[o++] = '=';
    }
    out[o] = '\0';
    return true;
}

// Alphabet value of one character, or -1. No whitespace is tolerated: an attacker must not be able
// to pad a frame out to a different length and still decode.
static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool kvm_pair_b64_decode(const char *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (in == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    const size_t n = strlen(in);
    if (n == 0 || (n % 4u) != 0u) {
        return false;
    }
    size_t pad = 0;
    if (in[n - 1] == '=') {
        pad = (in[n - 2] == '=') ? 2u : 1u;
    }
    const size_t produced = n / 4u * 3u - pad;
    if (produced > cap) {
        return false;
    }

    size_t o = 0;
    for (size_t i = 0; i < n; i += 4u) {
        int v[4];
        for (size_t j = 0; j < 4u; j++) {
            const char c = in[i + j];
            if (c == '=') {
                // Padding is only legal in the last group, and only in the last two positions.
                if (i + 4u != n || j < 2u) {
                    return false;
                }
                v[j] = 0;
            } else {
                v[j] = b64_value(c);
                if (v[j] < 0) {
                    return false;
                }
            }
        }
        const uint32_t word = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                              ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        if (o < produced) {
            out[o++] = (uint8_t)((word >> 16) & 0xffu);
        }
        if (o < produced) {
            out[o++] = (uint8_t)((word >> 8) & 0xffu);
        }
        if (o < produced) {
            out[o++] = (uint8_t)(word & 0xffu);
        }
    }
    *out_len = produced;
    return true;
}

bool kvm_pair_hex_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    if (out == NULL || (in == NULL && len != 0)) {
        return false;
    }
    if (cap < len * 2u + 1u) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2u] = digits[(in[i] >> 4) & 0x0fu];
        out[i * 2u + 1u] = digits[in[i] & 0x0fu];
    }
    out[len * 2u] = '\0';
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool kvm_pair_hex_decode(const char *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (in == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    const size_t n = strlen(in);
    if (n == 0 || (n % 2u) != 0u || n / 2u > cap) {
        return false;
    }
    for (size_t i = 0; i < n; i += 2u) {
        const int hi = hex_value(in[i]);
        const int lo = hex_value(in[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2u] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2u;
    return true;
}

void kvm_pair_blob_begin(kvm_pair_blob_t *w, uint8_t *buf, size_t cap)
{
    if (w == NULL) {
        return;
    }
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

bool kvm_pair_blob_put(kvm_pair_blob_t *w, const uint8_t *chunk, size_t len)
{
    if (w == NULL || w->buf == NULL || chunk == NULL) {
        return false;
    }
    if (len == 0 || len > KVM_PAIR_CHUNK_MAX) {
        w->overflow = true;
        return false;
    }
    if (w->len + 1u + len > w->cap) {
        w->overflow = true;
        return false;
    }
    w->buf[w->len++] = (uint8_t)len;
    memcpy(w->buf + w->len, chunk, len);
    w->len += len;
    return true;
}

bool kvm_pair_blob_split(const uint8_t *blob, size_t blob_len, size_t chunks, const uint8_t **ptr,
                         size_t *len)
{
    if (blob == NULL || ptr == NULL || len == NULL || chunks == 0) {
        return false;
    }
    size_t at = 0;
    for (size_t i = 0; i < chunks; i++) {
        if (at >= blob_len) {
            return false;
        }
        const size_t clen = blob[at];
        at += 1u;
        if (clen == 0 || clen > KVM_PAIR_CHUNK_MAX || at + clen > blob_len) {
            return false;
        }
        ptr[i] = blob + at;
        len[i] = clen;
        at += clen;
    }
    // Trailing bytes are a malformed frame, not something to ignore (03 §6.5).
    return at == blob_len;
}

bool kvm_pair_mdns_txt(const char *device_name, bool pair_open, kvm_pair_txt_item_t *items,
                       char *scratch, size_t scratch_cap)
{
    if (device_name == NULL || items == NULL || scratch == NULL || scratch_cap < 4u) {
        return false;
    }
    if (device_name[0] == '\0') {
        return false;
    }
    scratch[0] = '1';
    scratch[1] = '\0';
    scratch[2] = pair_open ? '1' : '0';
    scratch[3] = '\0';

    items[0].name = "name";
    items[0].value = device_name;
    items[1].name = "v";
    items[1].value = &scratch[0];
    items[2].name = "pair";
    items[2].value = &scratch[2];
    return true;
}
