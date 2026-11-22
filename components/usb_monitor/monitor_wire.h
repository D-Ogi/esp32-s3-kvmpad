// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MONITOR_WIDTH = 640, MONITOR_HEIGHT = 480, MONITOR_JPEG_LIMIT = 96 * 1024 };
static inline unsigned monitor_width(bool native) { return native ? 1024u : MONITOR_WIDTH; }
static inline unsigned monitor_height(bool native) { return native ? 600u : MONITOR_HEIGHT; }
typedef struct { uint16_t width, height; } monitor_size_t;
typedef struct {
    bool native, windows_modes;
    monitor_size_t source;
    uint64_t bytes;
    uint32_t headers, rejected;
    uint8_t header[16];
    unsigned used;
    // After a discontinuity, retain a possible header followed by a JPEG prefix.
    uint8_t search[19];
    unsigned search_used;
    size_t remaining, written;
    uint8_t *destination;
} monitor_wire_t;

// Start with zeroed state. reserve returns NULL to consume and drop one
// complete payload. Bytes can cross endpoint, transfer and frame boundaries.
void monitor_wire_feed(monitor_wire_t *wire, const uint8_t *data, size_t size,
                       uint8_t *(*reserve)(void), void (*complete)(size_t));
