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

static bool accepts_size(const monitor_wire_t *wire)
{
    if (!wire->windows_modes)
        return wire->source.width == monitor_width(wire->native) && wire->source.height == monitor_height(wire->native);
    static const monitor_size_t modes[] = {
        {240,240}, {280,240}, {320,240}, {480,272}, {480,320}, {480,480}, {640,480},
        {800,480}, {800,600}, {960,960}, {1024,600}, {1024,768}, {1280,600}, {1280,720},
        {1280,800}, {1600,1200}, {1920,1080}
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
        if (modes[i].width == wire->source.width && modes[i].height == wire->source.height) return true;
    return false;
}

void monitor_wire_feed(monitor_wire_t *wire, const uint8_t *data, size_t size,
                       uint8_t *(*reserve)(void), void (*complete)(size_t))
{
    wire->bytes += size;
    while (size) {
        size_t n;
        if (!wire->remaining) {
            n = sizeof(wire->header) - wire->used;
            if (n > size) n = size;
            memcpy(wire->header + wire->used, data, n);
            wire->used += (unsigned)n;
            if (wire->used == sizeof(wire->header)) {
                const uint8_t *h = wire->header;
                uint32_t packed = le16(h + 12) | ((uint32_t)le16(h + 14) << 16);
                wire->remaining = packed >> 10; // The low ten bits hold the wrapping frame ID.
                wire->source = (monitor_size_t){le16(h + 8), le16(h + 10)};
                ++wire->headers;
                wire->used = 0;
                wire->written = 0;
                wire->destination = NULL;
            }
        } else {
            if (!wire->written) {
                // A header can end a USB transfer. The decoder may release its
                // last input slot before the first byte of this JPEG arrives.
                const uint8_t *h = wire->header;
                // The Espressif receiver ignores crc16 and cmd. The vendor's
                // optional END record has no payload and needs no slot.
                bool jpeg = h[2] == 3 && !le16(h + 4) && !le16(h + 6) &&
                            accepts_size(wire) &&
                            wire->remaining >= 4 && wire->remaining <= MONITOR_JPEG_LIMIT;
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
