// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "gud_receiver.h"
#include <string.h>

// Wire layout: Linux v6.12 include/drm/gud.h and drivers/gpu/drm/gud/gud_pipe.c.
// All multi-byte wire fields are little endian and may be unaligned.
#define LE16(n) ((n) & 255), (((n) >> 8) & 255)
#define LE32(n) LE16(n), LE16((n) >> 16)

enum {
    GET_STATUS = 0x00, GET_DESCRIPTOR = 0x01,
    GET_FORMATS = 0x40, GET_PROPERTIES = 0x41,
    GET_CONNECTORS = 0x50, GET_CONNECTOR_PROPERTIES = 0x51,
    FORCE_DETECT = 0x53, GET_CONNECTOR_STATUS = 0x54,
    GET_MODES = 0x55, GET_EDID = 0x56,
    SET_BUFFER = 0x60, STATE_CHECK = 0x61, STATE_COMMIT = 0x62,
    CONTROLLER_ENABLE = 0x63, DISPLAY_ENABLE = 0x64,
    REQUEST_NOT_SUPPORTED = 2, INVALID_PARAMETER = 4,
};

static const uint8_t descriptor[] = {
    LE32(0x1d50614d), 1, LE32(0), 0, LE32(GUD_R1_MAX_BYTES),
    LE32(GUD_WIDTH), LE32(GUD_MAX_WIDTH), LE32(GUD_HEIGHT), LE32(GUD_MAX_HEIGHT)
};

// VESA DMT timings. Only the VGA fallback carries the preferred flag.
static const uint8_t modes[][24] = {
    {LE32(25175), LE16(GUD_WIDTH), LE16(656), LE16(752), LE16(800),
     LE16(GUD_HEIGHT), LE16(490), LE16(492), LE16(525), LE32(0x40a)},
    {LE32(40000), LE16(GUD_MAX_WIDTH), LE16(840), LE16(968), LE16(1056),
     LE16(GUD_MAX_HEIGHT), LE16(601), LE16(605), LE16(628), LE32(0x05)},
};

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool gud_receiver_init(gud_receiver_t *r, uint8_t *scratch, size_t capacity,
                       gud_present_r1_t present, void *context)
{
    if (!r) return false;
    *r = (gud_receiver_t){0};
    if (!scratch || capacity < GUD_R1_MAX_BYTES || !present) return false;
    r->scratch = scratch;
    r->present = present;
    r->context = context;
    return true;
}

void gud_receiver_abort(gud_receiver_t *r)
{
    r->expected = 0;
    r->used = 0;
}

int gud_receiver_get(gud_receiver_t *r, uint8_t request, uint16_t value,
                     uint8_t *reply, size_t capacity)
{
    static const uint8_t panel[5] = {0}; // fixed connected panel, no polling
    static const uint8_t r1 = 1, connected = 1;
    const uint8_t *src = NULL;
    size_t size = 0;
    if (!r->scratch) return -1;
    if (value || (capacity && !reply)) {
        r->status = INVALID_PARAMETER;
        return -1;
    }
    switch (request) {
    case GET_STATUS: src = &r->status; size = 1; break;
    case GET_DESCRIPTOR: src = descriptor; size = sizeof(descriptor); break;
    case GET_FORMATS: src = &r1; size = 1; break;
    case GET_CONNECTORS: src = panel; size = sizeof(panel); break;
    case GET_CONNECTOR_STATUS: src = &connected; size = 1; break;
    case GET_MODES: src = modes[0]; size = sizeof(modes); break;
    case GET_PROPERTIES:
    case GET_CONNECTOR_PROPERTIES:
    case GET_EDID: break;
    default: r->status = REQUEST_NOT_SUPPORTED; return -1;
    }
    if (size > capacity) size = capacity;
    if (size) memcpy(reply, src, size);
    if (request != GET_STATUS) r->status = 0;
    return (int)size;
}

static bool start_rect(gud_receiver_t *r, const uint8_t *p, size_t length)
{
    if (!r->committed || !r->controller_enabled || length != 25) return false;
    uint32_t x = read32(p), y = read32(p + 4);
    uint32_t w = read32(p + 8), h = read32(p + 12);
    if (x >= r->width || y >= r->height || !w || !h ||
        w > r->width - x || h > r->height - y || p[20] || read32(p + 21))
        return false;
    size_t bytes = ((w + 7) / 8) * h;
    if (bytes > GUD_R1_MAX_BYTES || bytes != read32(p + 16)) return false;
    r->rect = (gud_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h};
    r->expected = bytes;
    return true;
}

bool gud_receiver_set(gud_receiver_t *r, uint8_t request, uint16_t value,
                      const uint8_t *data, size_t length)
{
    if (!r->scratch) return false;
    if (request == SET_BUFFER) gud_receiver_abort(r);
    if (request == STATE_CHECK) r->checked = false;
    r->status = INVALID_PARAMETER;
    if (value || (length && !data)) return false;
    switch (request) {
    case FORCE_DETECT:
        if (length) return false;
        break;
    case STATE_CHECK:
        if (length != sizeof(modes[0]) + 2 || data[24] != 1 || data[25] != 0) return false;
        for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            if (!memcmp(modes[i], data, 20) &&
                ((read32(modes[i] + 20) ^ read32(data + 20)) & ~0x400u) == 0) {
                r->checked_mode = i;
                r->checked = true;
                break;
            }
        }
        if (!r->checked) return false;
        break;
    case STATE_COMMIT:
        if (length || !r->checked) return false;
        {
            const uint8_t *mode = modes[r->checked_mode];
            uint16_t width = mode[4] | ((uint16_t)mode[5] << 8);
            uint16_t height = mode[12] | ((uint16_t)mode[13] << 8);
            if (width != r->width || height != r->height) gud_receiver_abort(r);
            r->width = width;
            r->height = height;
        }
        r->committed = true;
        break;
    case CONTROLLER_ENABLE:
    case DISPLAY_ENABLE:
        if (length != 1 || data[0] > 1) return false;
        if (request == CONTROLLER_ENABLE) {
            r->controller_enabled = data[0] != 0;
            if (!r->controller_enabled) gud_receiver_abort(r);
        } else {
            r->display_enabled = data[0] != 0;
        }
        break;
    case SET_BUFFER:
        if (!start_rect(r, data, length)) return false;
        break;
    default: r->status = REQUEST_NOT_SUPPORTED; return false;
    }
    r->status = 0;
    return true;
}

bool gud_receiver_bulk(gud_receiver_t *r, const uint8_t *data, size_t length)
{
    if (!r->expected || length > r->expected - r->used || (!data && length)) {
        gud_receiver_abort(r);
        return false;
    }
    if (length) memcpy(r->scratch + r->used, data, length);
    r->used += length;
    if (r->used == r->expected) {
        gud_receiver_abort(r);
        r->present(r->context, r->rect, r->scratch);
    }
    return true;
}
