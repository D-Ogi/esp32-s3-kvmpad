// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TouchInput contract (spec 03 §2) and the pure GT911 frame decoder. No ESP-IDF dependency:
// this file and touch_sample.c are compiled by the host unit tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_TOUCH_MAX_POINTS 5

typedef struct {
    uint8_t track_id;
    int16_t x_px;       // 0..h_res-1 after scaling/rotation (03 §2: 0..799; 0..1023 on the 7B, ADR-026)
    int16_t y_px;       // 0..v_res-1 after scaling/rotation (03 §2: 0..479; 0..599 on the 7B, ADR-026)
    uint16_t strength;  // diagnostic only, no threshold required
} kvm_touch_point_t;

typedef struct {
    uint64_t timestamp_us;
    uint32_t sample_seq;
    bool valid;          // false = controller error -> cancel
    uint8_t point_count; // 0..5
    kvm_touch_point_t point[KVM_TOUCH_MAX_POINTS];
} kvm_touch_sample_t;

/** Explicit, board-defined transform from GT911 coordinates to screen pixels (TECH-011). */
typedef struct {
    uint16_t raw_x_max;   // GT911 configured X output maximum (registers 0x8048/0x8049)
    uint16_t raw_y_max;   // GT911 configured Y output maximum (registers 0x804A/0x804B)
    uint16_t h_res;       // screen width in pixels
    uint16_t v_res;       // screen height in pixels
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} kvm_touch_transform_t;

#define GT911_REG_STATUS      0x814E
#define GT911_REG_POINTS      0x814F
#define GT911_REG_PRODUCT_ID  0x8140   // 4 ASCII bytes, then FW version (LE16), X res, Y res, vendor
#define GT911_REG_CONFIG      0x8047   // config version, X max (LE16), Y max (LE16), touch number, mode
#define GT911_POINT_BYTES     8
#define GT911_STATUS_READY    0x80
#define GT911_STATUS_COUNT    0x0F

typedef enum {
    KVM_TOUCH_DECODE_OK = 0,      // `out` holds a valid sample (possibly with 0 points)
    KVM_TOUCH_DECODE_NOT_READY,   // status bit7 clear: no new frame, `out` untouched
    KVM_TOUCH_DECODE_INVALID,     // corrupt frame: `out` is an invalid (cancel) sample
} kvm_touch_decode_t;

/**
 * Decode one GT911 frame. `points` holds `n * 8` bytes read from 0x814F where n is the count
 * in `status`. Points are scaled with `xf`, sorted stably by track_id (never by array
 * position, TECH-010) and validated: count <= 5, coordinates inside the raw range, unique
 * track_id. Timestamp and sequence are left to the caller.
 */
kvm_touch_decode_t kvm_touch_decode_gt911(uint8_t status, const uint8_t *points, size_t points_len,
                                          const kvm_touch_transform_t *xf, kvm_touch_sample_t *out);

/** Make `out` an invalid (cancel) sample with zero points. */
void kvm_touch_make_invalid(kvm_touch_sample_t *out);

/** Stable insertion sort of the points by track_id. */
void kvm_touch_sort_by_track(kvm_touch_sample_t *s);

/**
 * Contract check of an already built sample (03 §2): count range, bounds, unique track_id,
 * and a sample_seq strictly newer than `prev_seq` when `have_prev`.
 */
bool kvm_touch_sample_is_consistent(const kvm_touch_sample_t *s, uint16_t h_res, uint16_t v_res, bool have_prev,
                                    uint32_t prev_seq);

#ifdef __cplusplus
}
#endif
