// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include <string.h>

#include "kvm_touch.h"

void kvm_touch_make_invalid(kvm_touch_sample_t *out)
{
    uint64_t ts = out->timestamp_us;
    uint32_t seq = out->sample_seq;
    memset(out, 0, sizeof(*out));
    out->timestamp_us = ts;
    out->sample_seq = seq;
    out->valid = false;
}

void kvm_touch_sort_by_track(kvm_touch_sample_t *s)
{
    for (uint8_t i = 1; i < s->point_count; i++) {
        kvm_touch_point_t key = s->point[i];
        int j = (int)i - 1;
        while (j >= 0 && s->point[j].track_id > key.track_id) {
            s->point[j + 1] = s->point[j];
            j--;
        }
        s->point[j + 1] = key;
    }
}

static int16_t scale(uint32_t raw, uint16_t raw_max, uint16_t res)
{
    if (raw_max == 0 || res == 0) {
        return 0;
    }
    // raw in [0, raw_max] maps onto [0, res-1]; raw == raw_max is the last pixel.
    uint32_t px = (raw * (uint32_t)(res - 1u) + raw_max / 2u) / raw_max;
    if (px > (uint32_t)(res - 1u)) {
        px = res - 1u;
    }
    return (int16_t)px;
}

kvm_touch_decode_t kvm_touch_decode_gt911(uint8_t status, const uint8_t *points, size_t points_len,
                                          const kvm_touch_transform_t *xf, kvm_touch_sample_t *out)
{
    if ((status & GT911_STATUS_READY) == 0) {
        return KVM_TOUCH_DECODE_NOT_READY;
    }
    uint8_t n = status & GT911_STATUS_COUNT;
    if (xf == NULL || out == NULL || n > KVM_TOUCH_MAX_POINTS || points_len < (size_t)n * GT911_POINT_BYTES ||
        (n > 0 && points == NULL)) {
        if (out != NULL) {
            kvm_touch_make_invalid(out);
        }
        return KVM_TOUCH_DECODE_INVALID;
    }

    uint64_t ts = out->timestamp_us;
    uint32_t seq = out->sample_seq;
    memset(out, 0, sizeof(*out));
    out->timestamp_us = ts;
    out->sample_seq = seq;

    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *p = &points[(size_t)i * GT911_POINT_BYTES];
        uint8_t id = p[0];
        uint32_t rx = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
        uint32_t ry = (uint32_t)p[3] | ((uint32_t)p[4] << 8);
        uint16_t size = (uint16_t)(p[5] | (p[6] << 8));
        if (rx > xf->raw_x_max || ry > xf->raw_y_max) {
            kvm_touch_make_invalid(out);
            return KVM_TOUCH_DECODE_INVALID;
        }
        for (uint8_t k = 0; k < i; k++) {
            if (out->point[k].track_id == id) {
                kvm_touch_make_invalid(out);
                return KVM_TOUCH_DECODE_INVALID;
            }
        }
        int16_t x;
        int16_t y;
        if (xf->swap_xy) {
            x = scale(ry, xf->raw_y_max, xf->h_res);
            y = scale(rx, xf->raw_x_max, xf->v_res);
        } else {
            x = scale(rx, xf->raw_x_max, xf->h_res);
            y = scale(ry, xf->raw_y_max, xf->v_res);
        }
        if (xf->mirror_x) {
            x = (int16_t)(xf->h_res - 1 - x);
        }
        if (xf->mirror_y) {
            y = (int16_t)(xf->v_res - 1 - y);
        }
        out->point[i].track_id = id;
        out->point[i].x_px = x;
        out->point[i].y_px = y;
        out->point[i].strength = size;
    }
    out->point_count = n;
    out->valid = true;
    kvm_touch_sort_by_track(out);
    return KVM_TOUCH_DECODE_OK;
}

bool kvm_touch_sample_is_consistent(const kvm_touch_sample_t *s, uint16_t h_res, uint16_t v_res, bool have_prev,
                                    uint32_t prev_seq)
{
    if (s == NULL || s->point_count > KVM_TOUCH_MAX_POINTS) {
        return false;
    }
    if (have_prev && (int32_t)(s->sample_seq - prev_seq) <= 0) {
        return false;
    }
    if (!s->valid) {
        return s->point_count == 0;
    }
    for (uint8_t i = 0; i < s->point_count; i++) {
        const kvm_touch_point_t *p = &s->point[i];
        if (p->x_px < 0 || p->x_px >= (int16_t)h_res || p->y_px < 0 || p->y_px >= (int16_t)v_res) {
            return false;
        }
        for (uint8_t k = 0; k < i; k++) {
            if (s->point[k].track_id == p->track_id) {
                return false;
            }
        }
    }
    return true;
}
