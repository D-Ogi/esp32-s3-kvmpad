// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-030 / 03 §2: GT911 frame decoding — track_id order (VEC-003 precondition), empty frames,
// corrupt frames, bounds, transform. Includes a seeded random-frame property run.
#include <string.h>

#include "kvm_test.h"
#include "kvm_touch.h"

static const kvm_touch_transform_t XF = {
    .raw_x_max = 800, .raw_y_max = 480, .h_res = 800, .v_res = 480,
};

static void put_point(uint8_t *buf, int i, uint8_t id, uint16_t x, uint16_t y, uint16_t size)
{
    uint8_t *p = &buf[i * GT911_POINT_BYTES];
    p[0] = id;
    p[1] = (uint8_t)(x & 0xFF);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)(y & 0xFF);
    p[4] = (uint8_t)(y >> 8);
    p[5] = (uint8_t)(size & 0xFF);
    p[6] = (uint8_t)(size >> 8);
    p[7] = 0;
}

static void not_ready_leaves_sample_untouched(void)
{
    kvm_touch_sample_t s;
    memset(&s, 0x77, sizeof(s));
    CHECK_EQ(kvm_touch_decode_gt911(0x03, NULL, 0, &XF, &s), KVM_TOUCH_DECODE_NOT_READY);
    CHECK_EQ(s.point_count, 0x77);
}

static void empty_ready_frame_is_a_valid_release(void)
{
    kvm_touch_sample_t s = {.timestamp_us = 42, .sample_seq = 7, .point_count = 3};
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY, NULL, 0, &XF, &s), KVM_TOUCH_DECODE_OK);
    CHECK(s.valid);
    CHECK_EQ(s.point_count, 0);
    CHECK_EQ(s.timestamp_us, 42);
    CHECK_EQ(s.sample_seq, 7);
}

static void points_sorted_by_track_not_by_array(void)
{
    uint8_t buf[40];
    put_point(buf, 0, 3, 100, 100, 10);
    put_point(buf, 1, 0, 200, 200, 11);
    put_point(buf, 2, 1, 300, 300, 12);
    kvm_touch_sample_t s = {0};
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY | 3, buf, sizeof(buf), &XF, &s), KVM_TOUCH_DECODE_OK);
    CHECK_EQ(s.point_count, 3);
    CHECK_EQ(s.point[0].track_id, 0);
    CHECK_EQ(s.point[0].x_px, 200);
    CHECK_EQ(s.point[1].track_id, 1);
    CHECK_EQ(s.point[2].track_id, 3);
    CHECK_EQ(s.point[2].strength, 10);
}

static void reversed_array_order_gives_identical_sample(void)
{
    // VEC-003 precondition: the same fingers in the opposite array order must not "jump".
    uint8_t a[40];
    uint8_t b[40];
    put_point(a, 0, 1, 10, 20, 5);
    put_point(a, 1, 2, 30, 40, 6);
    put_point(b, 0, 2, 30, 40, 6);
    put_point(b, 1, 1, 10, 20, 5);
    kvm_touch_sample_t sa = {0};
    kvm_touch_sample_t sb = {0};
    kvm_touch_decode_gt911(GT911_STATUS_READY | 2, a, sizeof(a), &XF, &sa);
    kvm_touch_decode_gt911(GT911_STATUS_READY | 2, b, sizeof(b), &XF, &sb);
    CHECK(memcmp(sa.point, sb.point, sizeof(sa.point)) == 0);
}

static void five_points_accepted_six_rejected(void)
{
    uint8_t buf[48];
    for (int i = 0; i < 6; i++) {
        put_point(buf, i, (uint8_t)i, (uint16_t)(i * 10), (uint16_t)(i * 10), 1);
    }
    kvm_touch_sample_t s = {0};
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY | 5, buf, 40, &XF, &s), KVM_TOUCH_DECODE_OK);
    CHECK_EQ(s.point_count, 5);
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY | 6, buf, sizeof(buf), &XF, &s), KVM_TOUCH_DECODE_INVALID);
    CHECK(!s.valid);
    CHECK_EQ(s.point_count, 0);
}

static void duplicate_track_and_out_of_range_rejected(void)
{
    uint8_t buf[40];
    put_point(buf, 0, 4, 10, 10, 1);
    put_point(buf, 1, 4, 20, 20, 1);
    kvm_touch_sample_t s = {0};
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY | 2, buf, sizeof(buf), &XF, &s), KVM_TOUCH_DECODE_INVALID);
    put_point(buf, 0, 1, 801, 10, 1);
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY | 1, buf, sizeof(buf), &XF, &s), KVM_TOUCH_DECODE_INVALID);
    put_point(buf, 0, 1, 10, 481, 1);
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY | 1, buf, sizeof(buf), &XF, &s), KVM_TOUCH_DECODE_INVALID);
    // Truncated buffer.
    CHECK_EQ(kvm_touch_decode_gt911(GT911_STATUS_READY | 2, buf, 8, &XF, &s), KVM_TOUCH_DECODE_INVALID);
}

static void edges_and_scaling(void)
{
    uint8_t buf[16];
    kvm_touch_sample_t s = {0};
    put_point(buf, 0, 0, 800, 480, 1); // raw max maps to the last pixel
    kvm_touch_decode_gt911(GT911_STATUS_READY | 1, buf, sizeof(buf), &XF, &s);
    CHECK_EQ(s.point[0].x_px, 799);
    CHECK_EQ(s.point[0].y_px, 479);
    const kvm_touch_transform_t big = {.raw_x_max = 1024, .raw_y_max = 600, .h_res = 800, .v_res = 480};
    put_point(buf, 0, 0, 512, 300, 1);
    kvm_touch_decode_gt911(GT911_STATUS_READY | 1, buf, sizeof(buf), &big, &s);
    CHECK_EQ(s.point[0].x_px, 400);
    CHECK_EQ(s.point[0].y_px, 240);
}

static void mirror_and_swap(void)
{
    uint8_t buf[16];
    kvm_touch_sample_t s = {0};
    kvm_touch_transform_t xf = XF;
    xf.mirror_x = true;
    xf.mirror_y = true;
    put_point(buf, 0, 0, 0, 0, 1);
    kvm_touch_decode_gt911(GT911_STATUS_READY | 1, buf, sizeof(buf), &xf, &s);
    CHECK_EQ(s.point[0].x_px, 799);
    CHECK_EQ(s.point[0].y_px, 479);
    const kvm_touch_transform_t sw = {.raw_x_max = 480, .raw_y_max = 800, .h_res = 800, .v_res = 480, .swap_xy = true};
    put_point(buf, 0, 0, 480, 0, 1);
    kvm_touch_decode_gt911(GT911_STATUS_READY | 1, buf, sizeof(buf), &sw, &s);
    CHECK_EQ(s.point[0].x_px, 0);
    CHECK_EQ(s.point[0].y_px, 479);
}

static void consistency_check(void)
{
    kvm_touch_sample_t s = {.sample_seq = 5, .valid = true, .point_count = 2};
    s.point[0] = (kvm_touch_point_t){.track_id = 1, .x_px = 0, .y_px = 0};
    s.point[1] = (kvm_touch_point_t){.track_id = 2, .x_px = 799, .y_px = 479};
    CHECK(kvm_touch_sample_is_consistent(&s, 800, 480, true, 4));
    CHECK(!kvm_touch_sample_is_consistent(&s, 800, 480, true, 5)); // not newer
    s.point[1].track_id = 1;
    CHECK(!kvm_touch_sample_is_consistent(&s, 800, 480, false, 0));
    kvm_touch_sample_t inv = {.sample_seq = 9};
    kvm_touch_make_invalid(&inv);
    CHECK(kvm_touch_sample_is_consistent(&inv, 800, 480, true, 8));
    CHECK_EQ(inv.sample_seq, 9);
}

static uint32_t rng = 0x12345678u; // fixed seed: reproducible
static uint32_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void property_random_frames(void)
{
    // Any byte soup either decodes to a contract-consistent sample or to an invalid one.
    uint8_t buf[40];
    for (int it = 0; it < 200000; it++) {
        for (size_t i = 0; i < sizeof(buf); i++) {
            buf[i] = (uint8_t)next();
        }
        uint8_t status = (uint8_t)next();
        kvm_touch_sample_t s = {.sample_seq = (uint32_t)it + 1};
        kvm_touch_decode_t d = kvm_touch_decode_gt911(status, buf, sizeof(buf), &XF, &s);
        if (d == KVM_TOUCH_DECODE_NOT_READY) {
            continue;
        }
        CHECK(kvm_touch_sample_is_consistent(&s, 800, 480, true, (uint32_t)it));
        for (uint8_t k = 1; k < s.point_count; k++) {
            if (s.point[k - 1].track_id >= s.point[k].track_id) {
                CHECK(0);
            }
        }
    }
}

int main(void)
{
    printf("test_touch_sample\n");
    RUN(not_ready_leaves_sample_untouched);
    RUN(empty_ready_frame_is_a_valid_release);
    RUN(points_sorted_by_track_not_by_array);
    RUN(reversed_array_order_gives_identical_sample);
    RUN(five_points_accepted_six_rejected);
    RUN(duplicate_track_and_out_of_range_rejected);
    RUN(edges_and_scaling);
    RUN(mirror_and_swap);
    RUN(consistency_check);
    RUN(property_random_frames);
    TEST_MAIN_END("test_touch_sample");
}
