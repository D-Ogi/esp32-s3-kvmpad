// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "monitor_wire.h"
#include <stdbool.h>
#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static bool accepts_size(const monitor_wire_t *wire, monitor_size_t source)
{
    if (!wire->windows_modes)
        return source.width == monitor_width(wire->native) && source.height == monitor_height(wire->native);
    static const monitor_size_t modes[] = {
        {240,240}, {280,240}, {320,240}, {480,272}, {480,320}, {480,480}, {640,480},
        {800,480}, {800,600}, {960,960}, {1024,600}, {1024,768}, {1280,600}, {1280,720},
        {1280,800}, {1600,1200}, {1920,1080}
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
        if (modes[i].width == source.width && modes[i].height == source.height) return true;
    return false;
}

static monitor_size_t geometry(const uint8_t *header)
{
    return (monitor_size_t){le16(header + 8), le16(header + 10)};
}

static size_t payload_length(const uint8_t *header)
{
    uint32_t packed = le16(header + 12) | ((uint32_t)le16(header + 14) << 16);
    return packed >> 10; // The low ten bits hold the wrapping frame ID.
}

static bool is_jpeg(const monitor_wire_t *wire, const uint8_t *h)
{
    const size_t length = payload_length(h);
    return h[2] == 3 && !le16(h + 4) && !le16(h + 6) && length >= 4 &&
           length <= MONITOR_JPEG_LIMIT && accepts_size(wire, geometry(h));
}

// A mode change can interrupt a frame. Once alignment is lost, a length read
// from JPEG data is meaningless. Look for a supported header immediately
// followed by a JPEG start marker, including across USB packet boundaries.
static void search_byte(monitor_wire_t *wire, uint8_t byte, uint8_t *(*reserve)(void))
{
    uint8_t *window = wire->search;
    window[wire->search_used++] = byte;
    if (wire->search_used < sizeof(wire->search)) return;
    if (window[16] == 0xff && window[17] == 0xd8 && window[18] == 0xff && is_jpeg(wire, window)) {
        wire->source = geometry(window);
        wire->remaining = payload_length(window) - 3;
        wire->written = 3;
        wire->destination = reserve();
        if (wire->destination) memcpy(wire->destination, window + 16, 3);
        ++wire->headers;
        wire->search_used = 0;
        return;
    }
    --wire->search_used;
    memmove(window, window + 1, wire->search_used);
}

void monitor_wire_feed(monitor_wire_t *wire, const uint8_t *data, size_t size,
                       uint8_t *(*reserve)(void), void (*complete)(size_t))
{
    wire->bytes += size;
    while (size) {
        size_t n;
        if (wire->search_used) {
            search_byte(wire, *data, reserve);
            n = 1;
        } else if (!wire->remaining) {
            n = sizeof(wire->header) - wire->used;
            if (n > size) n = size;
            memcpy(wire->header + wire->used, data, n);
            wire->used += (unsigned)n;
            if (wire->used == sizeof(wire->header)) {
                const uint8_t *h = wire->header;
                wire->remaining = payload_length(h);
                wire->source = geometry(h);
                ++wire->headers;
                wire->used = 0;
                wire->written = 0;
                wire->destination = NULL;
                if (wire->windows_modes && !is_jpeg(wire, h) && (wire->remaining || h[2] != 0xff)) {
                    memcpy(wire->search, h, sizeof(wire->header));
                    wire->search_used = sizeof(wire->header);
                    wire->remaining = 0;
                    ++wire->rejected;
                }
            }
        } else {
            if (!wire->written) {
                // A header can end a USB transfer. The decoder may release its
                // last input slot before the first byte of this JPEG arrives.
                const uint8_t *h = wire->header;
                // The Espressif receiver ignores crc16 and cmd. The vendor's
                // optional END record has no payload and needs no slot.
                bool jpeg = is_jpeg(wire, h);
                wire->destination = jpeg ? reserve() : NULL;
                if (!jpeg) ++wire->rejected;
            }
            n = wire->remaining < size ? wire->remaining : size;
            if (wire->destination) memcpy(wire->destination + wire->written, data, n);
            wire->written += n;
            wire->remaining -= n;
            if (!wire->remaining && wire->destination) complete(wire->written);
        }
        data += n;
        size -= n;
    }
}
