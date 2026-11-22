// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GUD_WIDTH = 640,
    GUD_HEIGHT = 480,
    GUD_MAX_WIDTH = 800,
    GUD_MAX_HEIGHT = 600,
    GUD_FRAME_BYTES = GUD_MAX_WIDTH / 8 * GUD_MAX_HEIGHT,
    GUD_R1_MAX_BYTES = GUD_WIDTH / 8 * GUD_HEIGHT,
};

typedef struct {
    uint16_t x, y, width, height;
} gud_rect_t;

// Called synchronously once per complete rectangle. Rows are byte padded,
// high bit first, 1 = white. Copy the data before returning if needed later.
typedef void (*gud_present_r1_t)(void *context, gud_rect_t rect, const uint8_t *rows);

typedef struct {
    gud_present_r1_t present;
    void *context;
    uint8_t *scratch;
    gud_rect_t rect;
    size_t expected, used;
    uint16_t width, height;
    unsigned checked_mode;
    bool checked, committed, controller_enabled, display_enabled;
    uint8_t status;
} gud_receiver_t;

// Single serialized owner. Scratch is caller-owned and lives for this session.
// Reinitialize after a bus reset or detach. This module declares no USB IDs.
bool gud_receiver_init(gud_receiver_t *r, uint8_t *scratch, size_t capacity,
                       gud_present_r1_t present, void *context);

// The transport must first match the registered interface at wIndex and
// vendor/interface request type (IN=0xc1 or OUT=0x41). GET returns the byte
// count or -1 for STALL; SET returns false for STALL. wValue is passed intact.
int gud_receiver_get(gud_receiver_t *r, uint8_t request, uint16_t value,
                     uint8_t *reply, size_t capacity);
bool gud_receiver_set(gud_receiver_t *r, uint8_t request, uint16_t value,
                      const uint8_t *data, size_t length);
bool gud_receiver_bulk(gud_receiver_t *r, const uint8_t *data, size_t length);
void gud_receiver_abort(gud_receiver_t *r);
