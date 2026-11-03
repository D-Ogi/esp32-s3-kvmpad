// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_data directory parser: see ime_data.h.
#include "ime_data.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool name_ok(const char *n)
{
    size_t len = 0;
    while (len < IME_DATA_NAME_CAP && n[len] != '\0') {
        const char c = n[len];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
            return false;
        }
        len++;
    }
    if (len == 0 || len == IME_DATA_NAME_CAP) {
        return false; // empty or not terminated inside the field
    }
    for (size_t i = len; i < IME_DATA_NAME_CAP; i++) {
        if (n[i] != '\0') {
            return false; // padding must be zero: one image, one byte sequence
        }
    }
    return true;
}

ime_data_status_t ime_data_parse(const uint8_t *dir, size_t dir_len, uint32_t part_size, ime_data_entry_t *out,
                                 uint32_t cap, uint32_t *count)
{
    if (dir == NULL || out == NULL || count == NULL || dir_len < IME_DATA_DIR_LEN) {
        return IME_DATA_ERR_ARG;
    }
    *count = 0;
    if (memcmp(dir, IME_DATA_MAGIC, 8) != 0) {
        return IME_DATA_ERR_MAGIC;
    }
    if (rd32(dir + 8) != IME_DATA_VERSION || rd32(dir + 16) != 0u || rd32(dir + 20) != 0u) {
        return IME_DATA_ERR_VERSION;
    }
    const uint32_t n = rd32(dir + 12);
    if (n == 0u || n > IME_DATA_MAX_ENTRIES || n > cap) {
        return IME_DATA_ERR_COUNT;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = dir + IME_DATA_HEADER_LEN + i * IME_DATA_ENTRY_LEN;
        ime_data_entry_t *o = &out[i];
        memcpy(o->name, e, IME_DATA_NAME_CAP);
        o->offset = rd32(e + 24);
        o->size = rd32(e + 28);
        memcpy(o->sha256, e + 32, 32);
        if (!name_ok(o->name)) {
            return IME_DATA_ERR_NAME;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (strncmp(out[j].name, o->name, IME_DATA_NAME_CAP) == 0) {
                return IME_DATA_ERR_NAME;
            }
        }
        if (o->offset < IME_DATA_ALIGN || (o->offset % IME_DATA_ALIGN) != 0u || o->size == 0u ||
            o->offset > part_size || o->size > part_size - o->offset) {
            return IME_DATA_ERR_BOUNDS;
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint32_t a0 = out[j].offset;
            const uint32_t a1 = out[j].offset + out[j].size;
            if (o->offset < a1 && a0 < o->offset + o->size) {
                return IME_DATA_ERR_BOUNDS;
            }
        }
    }
    *count = n;
    return IME_DATA_OK;
}

const char *ime_data_status_name(ime_data_status_t s)
{
    switch (s) {
    case IME_DATA_OK: return "ok";
    case IME_DATA_ERR_ARG: return "bad-argument";
    case IME_DATA_ERR_MAGIC: return "no-image";
    case IME_DATA_ERR_VERSION: return "version";
    case IME_DATA_ERR_COUNT: return "count";
    case IME_DATA_ERR_NAME: return "name";
    case IME_DATA_ERR_BOUNDS: return "bounds";
    }
    return "?";
}
