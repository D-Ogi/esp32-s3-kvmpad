// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the post-update health check (TASK-110, TECH-052: rollback on an unresponsive
// USB/GUI). The probe evaluator alone; its use by the state machine is in test_update_sm.c.
#include <stdio.h>
#include <string.h>

#include "kvm_test.h"
#include "update_health.h"

typedef struct {
    bool alive;
    uint32_t calls;
} probe_t;

static bool probe_alive(void *ctx)
{
    probe_t *p = ctx;
    p->calls++;
    return p->alive;
}

static void test_all_alive_needs_consecutive_sweeps(void)
{
    kvm_ota_health_t h;
    probe_t usb = {true, 0}, hid = {true, 0};
    kvm_ota_health_init(&h, 3u);
    CHECK(kvm_ota_health_add(&h, "usb", probe_alive, &usb, true));
    CHECK(kvm_ota_health_add(&h, "hid", probe_alive, &hid, true));
    kvm_ota_health_arm(&h, 1000u);

    CHECK_EQ(kvm_ota_health_sweep(&h, 0u), KVM_OTA_HEALTH_PENDING);
    CHECK_EQ(kvm_ota_health_sweep(&h, 1u), KVM_OTA_HEALTH_PENDING);
    CHECK_EQ(kvm_ota_health_sweep(&h, 2u), KVM_OTA_HEALTH_PASS);
    CHECK_EQ(h.sweeps, 3);
    CHECK_EQ(usb.calls, 3);
    CHECK_EQ(hid.calls, 3);
    // A settled result is sticky: the probes are not called again.
    CHECK_EQ(kvm_ota_health_sweep(&h, 3u), KVM_OTA_HEALTH_PASS);
    CHECK_EQ(usb.calls, 3);
}

static void test_a_flapping_subsystem_restarts_the_count(void)
{
    kvm_ota_health_t h;
    probe_t usb = {true, 0}, lvgl = {true, 0};
    kvm_ota_health_init(&h, 3u);
    CHECK(kvm_ota_health_add(&h, "usb", probe_alive, &usb, true));
    CHECK(kvm_ota_health_add(&h, "lvgl", probe_alive, &lvgl, true));
    kvm_ota_health_arm(&h, 1000u);

    CHECK_EQ(kvm_ota_health_sweep(&h, 0u), KVM_OTA_HEALTH_PENDING);
    CHECK_EQ(kvm_ota_health_sweep(&h, 1u), KVM_OTA_HEALTH_PENDING);
    lvgl.alive = false; // the UI owner task dies just before the image would be confirmed
    CHECK_EQ(kvm_ota_health_sweep(&h, 2u), KVM_OTA_HEALTH_PENDING);
    CHECK_EQ(h.consecutive, 0);
    lvgl.alive = true;
    CHECK_EQ(kvm_ota_health_sweep(&h, 3u), KVM_OTA_HEALTH_PENDING);
    CHECK_EQ(kvm_ota_health_sweep(&h, 4u), KVM_OTA_HEALTH_PENDING);
    CHECK_EQ(kvm_ota_health_sweep(&h, 5u), KVM_OTA_HEALTH_PASS);
}

static void test_a_dead_required_probe_times_out(void)
{
    kvm_ota_health_t h;
    probe_t usb = {false, 0}, hid = {true, 0}, lvgl = {true, 0}, touch = {true, 0};
    kvm_ota_health_init(&h, 2u);
    CHECK(kvm_ota_health_add(&h, "usb", probe_alive, &usb, true));
    CHECK(kvm_ota_health_add(&h, "hid", probe_alive, &hid, true));
    CHECK(kvm_ota_health_add(&h, "lvgl", probe_alive, &lvgl, true));
    CHECK(kvm_ota_health_add(&h, "touch", probe_alive, &touch, true));
    kvm_ota_health_arm(&h, 100u);

    for (uint64_t t = 0; t < 100u; t += 10u) {
        CHECK_EQ(kvm_ota_health_sweep(&h, t), KVM_OTA_HEALTH_PENDING);
    }
    CHECK_EQ(kvm_ota_health_sweep(&h, 100u), KVM_OTA_HEALTH_TIMEOUT);
    CHECK_EQ(kvm_ota_health_missing(&h), 1u); // probe 0 (usb) never reported alive
    CHECK_EQ(h.last_mask, 0xEu);
    CHECK(strcmp(kvm_ota_health_probe_name(&h, 0u), "usb") == 0);
    CHECK(strcmp(kvm_ota_health_probe_name(&h, 3u), "touch") == 0);
    CHECK(strcmp(kvm_ota_health_probe_name(&h, 4u), "") == 0);
}

static void test_optional_probe_does_not_block(void)
{
    kvm_ota_health_t h;
    probe_t usb = {true, 0}, extra = {false, 0};
    kvm_ota_health_init(&h, 1u);
    CHECK(kvm_ota_health_add(&h, "usb", probe_alive, &usb, true));
    CHECK(kvm_ota_health_add(&h, "extra", probe_alive, &extra, false));
    kvm_ota_health_arm(&h, 100u);
    CHECK_EQ(kvm_ota_health_sweep(&h, 0u), KVM_OTA_HEALTH_PASS);
    CHECK_EQ(kvm_ota_health_missing(&h), 0u);
}

static void test_a_probe_coming_up_exactly_at_the_deadline_counts(void)
{
    kvm_ota_health_t h;
    probe_t usb = {false, 0};
    kvm_ota_health_init(&h, 1u);
    CHECK(kvm_ota_health_add(&h, "usb", probe_alive, &usb, true));
    kvm_ota_health_arm(&h, 100u);
    CHECK_EQ(kvm_ota_health_sweep(&h, 50u), KVM_OTA_HEALTH_PENDING);
    usb.alive = true;
    CHECK_EQ(kvm_ota_health_sweep(&h, 100u), KVM_OTA_HEALTH_PASS);
}

static void test_an_already_expired_deadline_still_gets_one_sweep(void)
{
    kvm_ota_health_t h;
    probe_t usb = {true, 0};
    kvm_ota_health_init(&h, 1u);
    CHECK(kvm_ota_health_add(&h, "usb", probe_alive, &usb, true));
    kvm_ota_health_arm(&h, 10u);
    CHECK_EQ(kvm_ota_health_sweep(&h, 9999u), KVM_OTA_HEALTH_PASS);
    CHECK_EQ(usb.calls, 1);
}

static void test_no_probes_at_all_passes(void)
{
    // A build with no registered probe must not deadlock the update: an empty required mask is
    // satisfied. The firmware registers four, and the state machine reports the mask either way.
    kvm_ota_health_t h;
    kvm_ota_health_init(&h, 1u);
    kvm_ota_health_arm(&h, 100u);
    CHECK_EQ(kvm_ota_health_sweep(&h, 0u), KVM_OTA_HEALTH_PASS);
}

static void test_table_limits_and_bad_arguments(void)
{
    kvm_ota_health_t h;
    probe_t p = {true, 0};
    kvm_ota_health_init(&h, 200u);
    CHECK_EQ(h.need_consecutive, 16); // clamped
    kvm_ota_health_init(&h, 0u);
    CHECK_EQ(h.need_consecutive, 1);

    for (unsigned i = 0; i < KVM_OTA_PROBE_MAX; i++) {
        CHECK(kvm_ota_health_add(&h, "p", probe_alive, &p, true));
    }
    CHECK(!kvm_ota_health_add(&h, "overflow", probe_alive, &p, true));
    CHECK(!kvm_ota_health_add(&h, NULL, probe_alive, &p, true));
    CHECK(!kvm_ota_health_add(&h, "null", NULL, &p, true));
    CHECK(!kvm_ota_health_add(NULL, "p", probe_alive, &p, true));
    CHECK_EQ(h.count, KVM_OTA_PROBE_MAX);
    CHECK_EQ(kvm_ota_health_sweep(NULL, 0u), KVM_OTA_HEALTH_TIMEOUT);
    CHECK_EQ(kvm_ota_health_missing(NULL), 0u);
    CHECK(strcmp(kvm_ota_health_probe_name(NULL, 0u), "") == 0);
}

// Board #1, 2026-11-02: an image without the touch reader was confirmed. The touch counter never moved, but the
// old heartbeat called it alive for stale_us after seeding, and the four sweeps fit inside that window.
static void test_a_frozen_counter_is_dead_from_the_start(void)
{
    kvm_ota_heartbeat_t hb = {.stale_us = 3000000u};
    CHECK(!kvm_ota_heartbeat_alive(&hb, 0u, 1000000u)); // seeds
    for (uint64_t t = 1400000u; t < 10000000u; t += 400000u) {
        CHECK(!kvm_ota_heartbeat_alive(&hb, 0u, t)); // never moved: dead, inside and past stale_us
    }

    // the same through the evaluator, sweeping every 400 ms as the device does: a rollback, not a PASS
    kvm_ota_heartbeat_t touch = {.stale_us = 3000000u};
    kvm_ota_heartbeat_t hid = {.stale_us = 1000000u};
    uint32_t hid_count = 0;
    kvm_ota_health_t h;
    kvm_ota_health_init(&h, 4u);
    kvm_ota_health_arm(&h, 30000000u);
    kvm_ota_health_result_t r = KVM_OTA_HEALTH_PENDING;
    for (uint64_t t = 0; r == KVM_OTA_HEALTH_PENDING && t <= 31000000u; t += 400000u) {
        hid_count += 40u;
        const bool touch_alive = kvm_ota_heartbeat_alive(&touch, 7u, t); // frozen at 7
        const bool hid_alive = kvm_ota_heartbeat_alive(&hid, hid_count, t);
        // drive the evaluator with the two heartbeats as its probes
        probe_t pt = {touch_alive, 0}, ph = {hid_alive, 0};
        if (h.count == 0u) {
            CHECK(kvm_ota_health_add(&h, "hid", probe_alive, &ph, true));
            CHECK(kvm_ota_health_add(&h, "touch", probe_alive, &pt, true));
        }
        h.p[0].ctx = &ph;
        h.p[1].ctx = &pt;
        r = kvm_ota_health_sweep(&h, t);
    }
    CHECK_EQ(r, KVM_OTA_HEALTH_TIMEOUT);
    CHECK_EQ(kvm_ota_health_missing(&h), 0x2u); // touch
}

static void test_a_moving_counter_is_alive_until_it_stalls(void)
{
    kvm_ota_heartbeat_t hb = {.stale_us = 1000000u};
    CHECK(!kvm_ota_heartbeat_alive(&hb, 5u, 0u));       // seeds
    CHECK(!kvm_ota_heartbeat_alive(&hb, 5u, 300000u));  // not moved yet
    CHECK(kvm_ota_heartbeat_alive(&hb, 6u, 600000u));   // moved
    CHECK(kvm_ota_heartbeat_alive(&hb, 6u, 1600000u));  // last move 1.0 s ago: still fresh
    CHECK(!kvm_ota_heartbeat_alive(&hb, 6u, 1600001u)); // stalled past stale_us
    CHECK(kvm_ota_heartbeat_alive(&hb, 7u, 1700000u));  // recovers when it moves again
    CHECK(kvm_ota_heartbeat_alive(&hb, 0u, 1800000u));  // wrap-around is a move
    CHECK(!kvm_ota_heartbeat_alive(NULL, 1u, 0u));
}

int main(void)
{
    printf("test_update_health\n");
    RUN(test_a_frozen_counter_is_dead_from_the_start);
    RUN(test_a_moving_counter_is_alive_until_it_stalls);
    RUN(test_all_alive_needs_consecutive_sweeps);
    RUN(test_a_flapping_subsystem_restarts_the_count);
    RUN(test_a_dead_required_probe_times_out);
    RUN(test_optional_probe_does_not_block);
    RUN(test_a_probe_coming_up_exactly_at_the_deadline_counts);
    RUN(test_an_already_expired_deadline_still_gets_one_sweep);
    RUN(test_no_probes_at_all_passes);
    RUN(test_table_limits_and_bad_arguments);
    TEST_MAIN_END("test_update_health");
}
