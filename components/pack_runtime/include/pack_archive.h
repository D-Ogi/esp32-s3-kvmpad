// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PACK_MANIFEST, PACK_UI, PACK_BINDINGS, PACK_INTEGRITY, PACK_DOCS, PACK_SIGNATURE = PACK_DOCS, PACK_ASSETS };
enum { PACK_ARCHIVE_MAX = 256 * 1024, PACK_DOCUMENT_MAX = 65535, PACK_ENTRY_LIMIT = 64,
       PACK_EXPANDED_MAX = 2 * 1024 * 1024, PACK_NAME_MAX = 160 };

typedef struct {
    const uint8_t *data;
    size_t compressed, size;
    uint32_t crc;
    unsigned method;
    char name[PACK_NAME_MAX + 1];
    uint8_t digest[32]; // expected SHA-256, checked against each decompressed payload
} pack_member_t;

typedef struct {
    pack_member_t member[PACK_ENTRY_LIMIT + 1]; // an unsigned ZIP leaves the signature slot empty
    unsigned end;
} pack_archive_t;

// Member bytes borrow the immutable ZIP buffer. Assets follow the fixed document and signature slots.
bool pack_archive_open(const uint8_t *zip, size_t size, pack_archive_t *archive);
