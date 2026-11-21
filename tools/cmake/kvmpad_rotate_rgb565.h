// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define KVM_ROTATE_IRAM IRAM_ATTR __attribute__((noinline))
#else
#define KVM_ROTATE_IRAM
#endif

// The framebuffer is RGB565 storage; GNU may_alias permits paired pixel stores.
typedef uint32_t kvmpad_pixel_pair_t __attribute__((__may_alias__));

// Source is the flushed tile's top-left pixel, including its actual row stride.
// Destination is physical pixel (logical_y, physical_height - 1 - logical_x).
static void KVM_ROTATE_IRAM kvmpad_rotate_rgb565_270(const void *source, void *framebuffer,
                                                     unsigned left, unsigned top, unsigned width, unsigned height,
                                                     unsigned source_stride, unsigned physical_width,
                                                     unsigned physical_height)
{
    unsigned first_row = physical_height - left - width;
    if (((uintptr_t)source | (uintptr_t)framebuffer) & 1u) {
        // Some external buffers need byte loads or stores. The adapter buffers are pixel aligned.
        for (unsigned line = 0; line < width; line++) {
            uint8_t *dst = (uint8_t *)framebuffer + 2u * ((first_row + line) * physical_width + top);
            const unsigned col = width - 1 - line;
            for (unsigned y = 0; y < height; y++) {
                const uint8_t *pixel = (const uint8_t *)source + 2u * (y * source_stride + col);
                dst[2u * y] = pixel[0];
                dst[2u * y + 1] = pixel[1];
            }
        }
        return;
    }

    for (unsigned line = 0; line < width; line++) {
        const uint16_t *src = (const uint16_t *)source + (width - 1 - line);
        uint16_t *dst = (uint16_t *)framebuffer + (first_row + line) * physical_width + top;
        unsigned y = 0;
        if (height && ((uintptr_t)dst & 2u)) {
            *dst++ = src[0];
            y++;
        }
        // Walk a physical scanline in order. Aligned word stores write adjacent
        // pixel pairs without string-library calls or a temporary transposed tile.
        for (; y + 3 < height; y += 4, dst += 4) {
            const uint16_t *p = src + y * source_stride;
            uint32_t pair0 = p[0] | ((uint32_t)p[source_stride] << 16);
            uint32_t pair1 = p[2 * source_stride] | ((uint32_t)p[3 * source_stride] << 16);
            kvmpad_pixel_pair_t *words = (kvmpad_pixel_pair_t *)dst;
            words[0] = pair0;
            words[1] = pair1;
        }
        if (y + 1 < height) {
            const uint16_t *p = src + y * source_stride;
            uint32_t pair = p[0] | ((uint32_t)p[source_stride] << 16);
            *(kvmpad_pixel_pair_t *)dst = pair;
            y += 2;
            dst += 2;
        }
        if (y < height) *dst = src[y * source_stride];
    }
}
#undef KVM_ROTATE_IRAM
