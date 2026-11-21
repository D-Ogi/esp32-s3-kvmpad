// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "../../tools/cmake/kvmpad_rotate_rgb565.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(unsigned pw, unsigned ph, unsigned x, unsigned y, unsigned w, unsigned h,
                  unsigned stride, unsigned src_offset, unsigned dst_offset)
{
    size_t src_size = src_offset + 2u * ((h - 1) * stride + w);
    size_t fb_size = dst_offset + 2u * pw * ph + 8;
    uint8_t *src = malloc(src_size), *got = malloc(fb_size), *expected = malloc(fb_size);
    assert(src && got && expected);
    for (size_t i = 0; i < src_size; i++) src[i] = (uint8_t)((i * 71u) ^ (i >> 5));
    memset(got, 0x93, fb_size);
    memset(expected, 0x93, fb_size);
    for (unsigned j = 0; j < h; j++) {
        for (unsigned i = 0; i < w; i++) {
            unsigned physical = (ph - 1 - (x + i)) * pw + y + j;
            memcpy(expected + dst_offset + 2u * physical, src + src_offset + 2u * (j * stride + i), 2);
        }
    }
    kvmpad_rotate_rgb565_270(src + src_offset, got + dst_offset, x, y, w, h, stride, pw, ph);
    assert(!memcmp(got, expected, fb_size));
    free(expected);
    free(got);
    free(src);
}

int main(void)
{
    for (unsigned w = 1; w <= 11; w++) {
        for (unsigned h = 1; h <= 21; h++) {
            for (unsigned s = 0; s < 4; s++) {
                for (unsigned d = 0; d < 4; d++) {
                    check(21, 11, 11-w, 21-h, w, h, w + (s ^ d), s, d);
                    check(24, 12, 0, 0, w, h, w, s, d);
                }
            }
        }
    }
    check(1024, 600, 0, 0, 600, 20, 600, 0, 0);
    check(1024, 600, 8, 161, 584, 21, 584, 0, 0);
    check(1024, 600, 9, 1003, 591, 21, 592, 2, 2);
    check(1024, 600, 599, 0, 1, 1024, 3, 0, 0);
    check(1024, 600, 0, 0, 600, 1024, 600, 0, 0);
    puts("PASS 7397 RGB565 rotations, stride, edge rectangles, byte offsets, and unchanged surrounding bytes");
    return 0;
}
