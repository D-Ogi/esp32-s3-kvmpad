// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "media_decode.h"
#include "monitor_present.h"
#include <string.h>


typedef enum { USB_JPEG_OK, USB_JPEG_INVALID, USB_JPEG_UNAVAILABLE } usb_jpeg_decode_result_t;

#ifdef KVM_USB_MOTION_CHECK
// Set before starting a trial session and left unchanged until its worker exits.
extern bool usb_jpeg_test_scalar;
extern bool usb_jpeg_test_rows_only;
extern bool usb_jpeg_test_vector;
extern bool usb_jpeg_test_cells;
#endif

#ifdef KVM_USB_SIMD_COMPARE
int usb_jpeg_equal_blocks(const void *a, const void *b, size_t groups);
#endif

// The callers below establish four-byte alignment. memcpy permits reads of
// RGB565 arrays as words without violating their effective type.
static inline uint32_t usb_jpeg_aligned_word(const uint8_t *p)
{
    uint32_t value;
#if defined(__GNUC__)
    __builtin_memcpy(&value, __builtin_assume_aligned(p, 4), sizeof(value));
#else
    memcpy(&value, p, sizeof(value));
#endif
    return value;
}

static inline bool usb_jpeg_equal_bytes(const void *old, const void *fresh, size_t size)
{
    const uint8_t *a = old, *b = fresh;
#ifdef KVM_USB_MOTION_CHECK
    if (usb_jpeg_test_scalar) return memcmp(a, b, size) == 0;
#endif
#ifdef KVM_USB_SIMD_COMPARE
    if (usb_jpeg_test_vector && size >= 64 && !(size & 63) && !(((uintptr_t)a | (uintptr_t)b) & 15))
        return usb_jpeg_equal_blocks(a, b, size / 64) != 0;
#endif
    if (size < 32 || (((uintptr_t)a | (uintptr_t)b) & 3)) return memcmp(a, b, size) == 0;
    while (size >= 16) {
        uint32_t diff = (usb_jpeg_aligned_word(a)      ^ usb_jpeg_aligned_word(b)) |
                        (usb_jpeg_aligned_word(a + 4)  ^ usb_jpeg_aligned_word(b + 4)) |
                        (usb_jpeg_aligned_word(a + 8)  ^ usb_jpeg_aligned_word(b + 8)) |
                        (usb_jpeg_aligned_word(a + 12) ^ usb_jpeg_aligned_word(b + 12));
        if (diff) return false;
        a += 16;
        b += 16;
        size -= 16;
    }
    return !size || memcmp(a, b, size) == 0;
}

enum { USB_JPEG_CELL_PIXELS = 128 };

// Both surfaces and every cell start on a four-byte boundary.
static inline bool usb_jpeg_cell_solid(const uint16_t *p, uint16_t color)
{
    uint32_t pair = (uint32_t)color << 16 | color;
    const uint8_t *bytes = (const uint8_t *)p;
    for (unsigned i = 0; i < USB_JPEG_CELL_PIXELS * sizeof(*p); i += 16) {
        if ((usb_jpeg_aligned_word(bytes+i) ^ pair) | (usb_jpeg_aligned_word(bytes+i+4) ^ pair) |
            (usb_jpeg_aligned_word(bytes+i+8) ^ pair) | (usb_jpeg_aligned_word(bytes+i+12) ^ pair)) return false;
    }
    return true;
}

// Entries are unknown (0), mixed (1), a band comparison hint (2), or a repeated
// color. Flipping bit 15 keeps both black and white representable. Reserved
// colors and hints within a band always use an exact byte comparison.
static inline bool usb_jpeg_cached_band_equal(uint16_t *cells, const uint16_t *old,
                                             const uint16_t *fresh, size_t pixels)
{
    if (pixels % USB_JPEG_CELL_PIXELS || (pixels && cells[0] == 2))
        return usb_jpeg_equal_bytes(old, fresh, pixels * sizeof(*old));
    unsigned solid = 0;
    size_t count = pixels / USB_JPEG_CELL_PIXELS;
    for (size_t cell = 0; cell < count; ++cell) {
        size_t i = cell * USB_JPEG_CELL_PIXELS;
        uint16_t tag = cells[cell];
        if (!tag) {
            uint16_t color = old[i];
            tag = color ^ 0x8000u;
            if (tag <= 2 || !usb_jpeg_cell_solid(old+i, color)) tag = 1;
            cells[cell] = tag;
        }
        if (tag <= 2) {
            if (!usb_jpeg_equal_bytes(old+i, fresh+i, USB_JPEG_CELL_PIXELS * sizeof(*old))) return false;
        } else {
            if (!usb_jpeg_cell_solid(fresh+i, tag ^ 0x8000u)) return false;
            ++solid;
        }
    }
    // Reading tags costs more than it saves on a mostly textured band. A
    // changed band invalidates the hint so the next background is reconsidered.
    if (count >= 8 && solid * 8 < count) cells[0] = 2;
    return true;
}

static inline bool usb_jpeg_any_nonzero(const void *data, size_t size)
{
    const uint8_t *p = data;
    if (!((uintptr_t)p & 3)) {
        while (size >= 16) {
            if (usb_jpeg_aligned_word(p) | usb_jpeg_aligned_word(p + 4) |
                usb_jpeg_aligned_word(p + 8) | usb_jpeg_aligned_word(p + 12)) return true;
            p += 16;
            size -= 16;
        }
    }
    while (size--) if (*p++) return true;
    return false;
}

typedef struct {
    const uint16_t *previous;
    uint16_t *cells; // Optional, native in-place comparisons only.
    uint16_t *stripe; // Native in-place decode only, 16 aligned rows of full width.
    monitor_damage_t bounds;
    monitor_damage_t above_gap, below_gap;
    int gap_rows;
    // Nearest changed columns on either side of a fixed cut in this picture.
    int cut, left_end, right_begin;
} usb_jpeg_damage_t;

static inline void include_row(monitor_damage_t *area, int left, int right, int y)
{
    if (left < area->x1) area->x1 = left;
    if (right > area->x2) area->x2 = right;
    area->y2 = y;
}

static inline int damage_pixels(monitor_damage_t area)
{
    return (area.x2 - area.x1 + 1) * (area.y2 - area.y1 + 1);
}

static inline unsigned usb_jpeg_damage_regions(const usb_jpeg_damage_t *track, monitor_damage_t areas[2])
{
    if (track->bounds.x2 < track->bounds.x1) return 0;
    areas[0] = track->bounds;
    int whole = damage_pixels(track->bounds), best = whole;
    monitor_damage_t first, second;
    if (track->gap_rows) {
        first = track->above_gap;
        second = track->below_gap;
        best = damage_pixels(first) + damage_pixels(second);
    }
    if (track->cut && track->right_begin > track->left_end + 1) {
        int columns = track->right_begin - track->left_end - 1;
        int cost = whole - columns * (track->bounds.y2 - track->bounds.y1 + 1);
        if (cost < best) {
            first = second = track->bounds;
            first.x2 = track->left_end;
            second.x1 = track->right_begin;
            best = cost;
        }
    }
    if (whole - best >= 4096 && best <= whole / 2) {
        areas[0] = first;
        areas[1] = second;
        return 2;
    }
    return 1;
}

// [left,end) contains at least one differing pixel.
static inline int first_change(const uint16_t *old, const uint16_t *fresh, int left, int end)
{
    if (old[left] == fresh[left]) {
        while (end - left >= 32 && !memcmp(old + left, fresh + left, 32 * sizeof(*old))) left += 32;
        while (old[left] == fresh[left]) ++left;
    }
    return left;
}

static inline int last_change(const uint16_t *old, const uint16_t *fresh, int left, int end)
{
    if (old[end-1] == fresh[end-1]) {
        while (end - left >= 32 && !memcmp(old + end-32, fresh + end-32, 32 * sizeof(*old))) end -= 32;
        while (old[end-1] == fresh[end-1]) --end;
    }
    return end-1;
}

static inline void choose_column_cut(usb_jpeg_damage_t *track, int width, int left, int right)
{
#ifdef KVM_USB_MOTION_CHECK
    if (usb_jpeg_test_rows_only) return;
#endif
    // Everything preceding this row lies inside bounds. Seed both sides
    // conservatively if the earlier bounds straddle the chosen column.
    monitor_damage_t b = track->bounds;
    bool prior = b.x2 >= b.x1;
    int low = prior && b.x1 < left ? b.x1 : left;
    int high = prior && b.x2 > right ? b.x2 : right;
    if (high - low < 64) return;
    int cut = (low + high + 1) / 2;
    track->cut = cut;
    track->left_end = prior && b.x1 < cut ? (b.x2 < cut ? b.x2 : cut-1) : -1;
    track->right_begin = prior && b.x2 >= cut ? (b.x1 >= cut ? b.x1 : cut) : width;
}

// The rows are tightly packed. A completely unchanged band needs one comparison
// and no stores to the native surface. Only changed rows need their edges.
static inline void usb_jpeg_damage_band(usb_jpeg_damage_t *track, const uint16_t *fresh, uint16_t *writeback,
                                        unsigned width, unsigned top, unsigned rows)
{
    const uint16_t *old = track->previous + (size_t)top * width;
    size_t pixels = (size_t)rows * width;
    if (track->cells) {
        uint16_t *cells = track->cells + (size_t)top * width / USB_JPEG_CELL_PIXELS;
        if (usb_jpeg_cached_band_equal(cells, old, fresh, pixels)) return;
        memset(cells, 0, pixels / USB_JPEG_CELL_PIXELS * sizeof(*cells));
    } else if (usb_jpeg_equal_bytes(old, fresh, pixels * sizeof(*old))) return;
    for (unsigned line = 0; line < rows; ++line, old += width, fresh += width) {
        if (usb_jpeg_equal_bytes(old, fresh, width * sizeof(*old))) continue;
        int left = first_change(old, fresh, 0, (int)width);
        int right = last_change(old, fresh, left, (int)width);
        int y = (int)(top + line);
        monitor_damage_t *b = &track->bounds;
        monitor_damage_t row = {left, y, right, y};
        if (!track->cut) choose_column_cut(track, (int)width, left, right);
        int last_left = right, first_right = left;
        if (track->cut && track->right_begin > track->left_end + 1) {
            if (left < track->cut && right >= track->cut) {
                last_left = last_change(old, fresh, left, track->cut);
                first_right = first_change(old, fresh, track->cut, right+1);
            }
            if (left < track->cut && last_left > track->left_end) track->left_end = last_left;
            if (right >= track->cut && first_right < track->right_begin) track->right_begin = first_right;
        }
        if (b->x2 < b->x1) *b = row;
        else {
            int gap = y - b->y2 - 1;
            if (gap > track->gap_rows) {
                track->above_gap = *b;
                track->below_gap = row;
                track->gap_rows = gap;
            } else if (track->gap_rows) {
                include_row(&track->below_gap, row.x1, row.x2, y);
            }
            include_row(b, row.x1, row.x2, y);
        }
        if (writeback) {
            uint16_t *dest = writeback + (size_t)line * width;
            if (first_right - last_left >= 32) {
                memcpy(dest + left, fresh + left, (size_t)(last_left - left + 1) * sizeof(*old));
                left = first_right;
            }
            memcpy(dest + left, fresh + left, (size_t)(right - left + 1) * sizeof(*old));
        }
    }
}

// The caller owns both the mutable compressed input copy and the 16-byte
// aligned output surface. A final partial MCU row may use padding in capacity.
usb_jpeg_decode_result_t usb_jpeg_decode(uint8_t *input, size_t length, const kvm_media_target_t *target,
                                        size_t capacity, bool *has_nonblack, usb_jpeg_damage_t *tracking);
