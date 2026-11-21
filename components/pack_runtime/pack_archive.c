// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_archive.h"

#include <string.h>

static const char *const documents[PACK_ASSETS] = {
    "manifest.json", "ui.json", "bindings.json", "integrity.json", "signature.ed25519"};

static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

static uint8_t fold(uint8_t c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

static bool folded_equal(const uint8_t *name, size_t n, const char *other)
{
    if (strlen(other) != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (fold(name[i]) != fold((uint8_t)other[i])) return false;
    return true;
}

static bool reserved(const uint8_t *p, size_t n)
{
    static const char *const devices[] = {"aux", "con", "nul", "prn"};
    for (unsigned i = 0; i < sizeof(devices)/sizeof(devices[0]); ++i)
        if (folded_equal(p, n, devices[i])) return true;
    return n == 4 && p[3] >= '1' && p[3] <= '9' &&
           (folded_equal(p, 3, "com") || folded_equal(p, 3, "lpt"));
}

static bool asset_path(const uint8_t *name, size_t n)
{
    if (n < 12 || n > PACK_NAME_MAX || memcmp(name, "assets/", 7) || memcmp(name+n-4, ".png", 4))
        return false;
    size_t start = 7, segment = start;
    for (size_t i = start; i <= n; ++i) {
        uint8_t c = i == n ? '/' : name[i];
        if (c == '.' || c == '/') {
            if (i == segment || (segment == start && reserved(name + start, i - start)))
                return false;
            segment = i + 1;
            if (c == '/') start = segment;
        } else if (!((fold(c) >= 'a' && fold(c) <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool pack_archive_open(const uint8_t *zip, size_t size, pack_archive_t *archive)
{
    if (!zip || !archive || size < 22 || size > PACK_ARCHIVE_MAX) {
        return false;
    }
    memset(archive, 0, sizeof(*archive));
    archive->end = PACK_ASSETS;
    pack_member_t *docs = archive->member;
    const size_t end = size - 22;
    const uint8_t *eocd = zip + end;
    const unsigned count = le16(eocd + 10);
    if (le32(eocd) != 0x06054b50 || le32(eocd + 4) || le16(eocd + 8) != count ||
        count < PACK_DOCS || count > PACK_ENTRY_LIMIT || le16(eocd + 20)) {
        return false;
    }
    const size_t cd = le32(eocd + 16);
    if (cd > end || le32(eocd + 12) != end - cd) {
        return false;
    }
    size_t pos = cd;
    size_t offsets[PACK_ENTRY_LIMIT], ends[PACK_ENTRY_LIMIT], expanded = 0;
    for (unsigned n = 0; n < count; ++n) {
        if (end - pos < 46) {
            return false;
        }
        const uint8_t *c = zip + pos;
        unsigned name_size = le16(c + 28), method = le16(c + 10);
        size_t variable = name_size + le16(c + 30) + le16(c + 32);
        unsigned mode = le32(c + 38) >> 28;
        if (le32(c) != 0x02014b50 || le16(c + 8) & ~0x800u || (method != 0 && method != 8) || le16(c + 34) ||
            (mode && mode != 8) || (le32(c + 38) & 0x10) || variable > end - pos - 46) {
            return false;
        }
        const uint8_t *name = c + 46;
        unsigned id = 0;
        while (id < PACK_ASSETS &&
               (strlen(documents[id]) != name_size || memcmp(name, documents[id], name_size))) {
            ++id;
        }
        if (id == PACK_ASSETS) {
            if (!asset_path(name, name_size) || archive->end >= PACK_ENTRY_LIMIT + 1) return false;
            for (unsigned i = PACK_ASSETS; i < archive->end; ++i)
                if (folded_equal(name, name_size, docs[i].name)) return false;
            id = archive->end++;
        }
        if (docs[id].data) {
            return false;
        }
        size_t off = le32(c + 42), packed = le32(c + 20), plain = le32(c + 24);
        if (off > cd || cd - off < 30 || !plain || plain > PACK_EXPANDED_MAX - expanded || !packed ||
            (id < PACK_DOCS && plain > PACK_DOCUMENT_MAX) ||
            (id == PACK_SIGNATURE && plain != 64) ||
            (method == 0 && packed != plain)) {
            return false;
        }
        const uint8_t *local = zip + off;
        size_t extra = le16(local + 28);
        if (le32(local) != 0x04034b50 || le16(local + 6) != le16(c + 8) || le16(local + 8) != method ||
            memcmp(local + 14, c + 16, 12) || name_size != le16(local + 26) || name_size + extra > cd - off - 30 ||
            memcmp(local + 30, name, name_size)) {
            return false;
        }
        size_t data = off + 30 + name_size + extra;
        if (packed > cd - data) {
            return false;
        }
        offsets[n] = off;
        ends[n] = data + packed;
        expanded += plain;
        docs[id].data = zip + data;
        docs[id].compressed = packed;
        docs[id].size = plain;
        docs[id].crc = le32(c + 16);
        docs[id].method = method;
        memcpy(docs[id].name, name, name_size);
        pos += 46 + variable;
    }
    // Require exactly the declared local records, regardless of directory order.
    size_t next = 0;
    for (unsigned n = 0; n < count; ++n) {
        unsigned i = 0;
        while (i < count && offsets[i] != next) {
            ++i;
        }
        if (i == count) {
            return false;
        }
        next = ends[i];
    }
    for (unsigned i = 0; i < PACK_DOCS; ++i)
        if (!docs[i].data) return false;
    return pos == end && next == cd;
}
