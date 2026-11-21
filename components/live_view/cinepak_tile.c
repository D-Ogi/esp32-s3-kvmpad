// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "cinepak_tile.h"
#include <string.h>

static unsigned be16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned be24(const uint8_t *p)
{
    return ((unsigned)p[0] << 16) | be16(p + 1);
}

static unsigned clip(int value)
{
    return (unsigned)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static void read_book(uint16_t book[256][4], const uint8_t *entries, unsigned count,
                      unsigned stride)
{
    for (unsigned i = 0; i < count; ++i, entries += stride) {
        int u = stride == 6 ? (int)(entries[4] ^ 128u) - 128 : 0;
        int v = stride == 6 ? (int)(entries[5] ^ 128u) - 128 : 0;
        for (unsigned j = 0; j < 4; ++j) {
            int y = entries[j];
            unsigned r = clip(y + 2 * v), g = clip(y - u / 2 - v), b = clip(y + 2 * u);
            book[i][j] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

static void draw_block(const uint16_t books[2][256][4], uint16_t *rows, const uint8_t *indices, bool fine)
{
    if (fine) {
        for (unsigned q = 0; q < 4; ++q) {
            const uint16_t *colors = books[1][indices[q]];
            uint16_t *at = rows + (q / 2) * 2 * KVM_CVID_W + (q % 2) * 2;
            memcpy(at, colors, 4);
            memcpy(at + KVM_CVID_W, colors + 2, 4);
        }
    } else {
        const uint16_t *v = books[0][indices[0]];
        for (unsigned q = 0; q < 4; ++q) {
            uint32_t pair = v[q] | ((uint32_t)v[q] << 16);
            uint16_t *at = rows + (q / 2) * 2 * KVM_CVID_W + (q % 2) * 2;
            memcpy(at, &pair, 4);
            memcpy(at + KVM_CVID_W, &pair, 4);
        }
    }
}

static bool blocks(kvm_cvid_workspace_t *work, kvm_cvid_index_t *index, const unsigned counts[2], bool mixed,
                   const uint8_t *data, const uint8_t *end, kvm_cvid_sink_t sink, void *ctx)
{
    if (index) {
        index->coarse_only = !mixed;
        if (!mixed) {
            // The Windows encoder sends one V1 index per block in raster order.
            if ((size_t)(end - data) != sizeof(index->coarse))
                return false;
            if (counts[0] < 256)
                for (size_t i = 0; i < sizeof(index->coarse); ++i)
                    if (data[i] >= counts[0]) return false;
            memcpy(index->coarse, data, sizeof(index->coarse));
            return true;
        }
        memset(index->mixed.fine, 0, sizeof(index->mixed.fine));
    }
    uint32_t flags = 0;
    unsigned left = 0;
    for (unsigned y = 0; y < KVM_CVID_H; y += 4) {
        for (unsigned x = 0; x < KVM_CVID_W; x += 4) {
            if (mixed && !left) {
                if (end - data < 4) {
                    return false;
                }
                flags = ((uint32_t)be16(data) << 16) | be16(data + 2);
                data += 4;
                left = 32;
            }
            bool fine = mixed && (flags >> 31) != 0;
            if (mixed) {
                flags <<= 1;
                --left;
            }
            unsigned n = fine ? 4u : 1u;
            if ((size_t)(end - data) < n) {
                return false;
            }
            if (sink) {
                draw_block(work->books, work->rows + x, data, fine);
            } else {
                for (unsigned i = 0; i < n; ++i) {
                    if (data[i] >= counts[fine]) {
                        return false;
                    }
                }
                if (index) {
                    unsigned b = (y / 4) * (KVM_CVID_W / 4) + x / 4;
                    if (fine)
                        index->mixed.fine[b / 8] |= 1u << (b % 8);
                    memcpy(index->mixed.indices[b], data, n);
                }
            }
            data += n;
        }
        if (sink && !sink(ctx, y, work->rows)) {
            return false;
        }
    }
    return data == end;
}

static bool unpack(kvm_cvid_workspace_t *work, kvm_cvid_index_t *index, const uint8_t *packet, size_t size,
                   kvm_cvid_sink_t sink, void *ctx)
{
    if ((!index && (!work || !sink)) || !packet || size < 22 || size > KVM_CVID_MAX_BYTES) {
        return false;
    }
    if (packet[0] != 0 || be24(packet + 1) != size || be16(packet + 4) != KVM_CVID_W ||
        be16(packet + 6) != KVM_CVID_H || be16(packet + 8) != 1 || packet[10] != 0x10 ||
        be24(packet + 11) != size - 10 || be16(packet + 14) || be16(packet + 16) ||
        be16(packet + 18) != KVM_CVID_H || be16(packet + 20) != KVM_CVID_W) {
        return false;
    }
    unsigned counts[2] = {0};
    const uint8_t *p = packet + 22, *end = packet + size;
    while (end - p >= 4) {
        unsigned type = p[0], len = be24(p + 1);
        if (len < 4 || len > (size_t)(end - p)) {
            return false;
        }
        if (type == 0x30 || type == 0x32) {
            // Validate all indices and lengths before publishing the first row.
            return p + len == end && blocks(work, index, counts, type == 0x30, p + 4, end, NULL, NULL)
                && (index || blocks(work, NULL, counts, type == 0x30, p + 4, end, sink, ctx));
        }
        if (type != 0x20 && type != 0x22 && type != 0x24 && type != 0x26) {
            return false;
        }
        unsigned book = (type & 2u) ? 0u : 1u;
        unsigned stride = (type & 4u) ? 4u : 6u;
        unsigned count = (len - 4) / stride;
        if (counts[book] || count > 256 || (len - 4) % stride) {
            return false;
        }
        read_book(index ? index->books[book] : work->books[book], p + 4, count, stride);
        counts[book] = count;
        p += len;
    }
    return false;
}

bool kvm_cvid_decode(kvm_cvid_workspace_t *work, const uint8_t *packet, size_t size,
                     kvm_cvid_sink_t sink, void *ctx)
{
    return unpack(work, NULL, packet, size, sink, ctx);
}

bool kvm_cvid_index(kvm_cvid_index_t *index, const uint8_t *packet, size_t size)
{
    if (!index)
        return false;
    return unpack(NULL, index, packet, size, NULL, NULL);
}

bool kvm_cvid_index_band(const kvm_cvid_index_t *index, unsigned y, uint16_t *rows)
{
    if (!index || !rows || y >= KVM_CVID_H || y % 4)
        return false;
    unsigned b = y / 4 * (KVM_CVID_W / 4);
    if (index->coarse_only) {
        for (unsigned x = 0; x < KVM_CVID_W; x += 4, ++b)
            draw_block(index->books, rows + x, &index->coarse[b], false);
        return true;
    }
    for (unsigned x = 0; x < KVM_CVID_W; x += 4, ++b)
        draw_block(index->books, rows + x, index->mixed.indices[b], (index->mixed.fine[b / 8] >> (b % 8)) & 1);
    return true;
}
