// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Post-update health check (TASK-110, TECH-052: "rollback on an unresponsive USB/GUI").
//
// Pure C. The subsystems that decide whether a freshly booted image is usable — the USB device
// stack, the HID scheduler, the LVGL owner task and the touch reader — are injected as probes, so
// the host suite can drive every pass, fail and timeout path without ESP-IDF.
//
// Rule: a probe is believed only when it reports alive on `need_consecutive` separate sweeps. One
// lucky sample must not confirm an image whose USB task dies a second later.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_OTA_PROBE_MAX 8u

typedef struct {
    const char *name;            // short, stable, log-safe
    bool (*alive)(void *ctx);    // must not block; called from the update task
    void *ctx;
    bool required;               // a required probe that never reports alive forces a rollback
} kvm_ota_probe_t;

typedef enum {
    KVM_OTA_HEALTH_PENDING = 0,
    KVM_OTA_HEALTH_PASS,
    KVM_OTA_HEALTH_TIMEOUT,
} kvm_ota_health_result_t;

typedef struct {
    kvm_ota_probe_t p[KVM_OTA_PROBE_MAX];
    uint8_t count;
    uint8_t need_consecutive;    // sweeps with every required probe alive before PASS
    uint8_t consecutive;         // sweeps achieved so far
    uint32_t required_mask;      // bit i: probe i is required
    uint32_t ever_mask;          // bit i: probe i reported alive at least once
    uint32_t last_mask;          // bit i: probe i was alive on the last sweep
    uint32_t sweeps;
    uint64_t deadline_us;
    kvm_ota_health_result_t result;
} kvm_ota_health_t;

/**
 * Heartbeat over a counter that advances on its own while a subsystem runs. Alive only when the counter has moved
 * since the first sample AND its last move is at most `stale_us` old. A counter that never moves is dead, however
 * soon after seeding it is asked (found on board #1: an image without the touch reader passed the check because a
 * frozen counter counted as alive for `stale_us` after seeding).
 */
typedef struct {
    uint32_t last;
    uint64_t last_change_us;
    uint64_t stale_us;
    bool seeded;
    bool moved; // the counter changed at least once after the seeding sample
} kvm_ota_heartbeat_t;

/** One sample of `counter` at `now_us` (monotonic). The first sample only seeds and returns false. */
bool kvm_ota_heartbeat_alive(kvm_ota_heartbeat_t *h, uint32_t counter, uint64_t now_us);

/** Reset the evaluator. `need_consecutive` is clamped to 1..16. */
void kvm_ota_health_init(kvm_ota_health_t *h, uint8_t need_consecutive);

/** Add a probe. Returns false when the table is full or the arguments are unusable. */
bool kvm_ota_health_add(kvm_ota_health_t *h, const char *name, bool (*alive)(void *), void *ctx, bool required);

/** Arm the check: the deadline is absolute on the same monotonic clock as kvm_ota_health_sweep(). */
void kvm_ota_health_arm(kvm_ota_health_t *h, uint64_t deadline_us);

/** One sweep of every probe. PENDING until enough consecutive clean sweeps or the deadline passes. */
kvm_ota_health_result_t kvm_ota_health_sweep(kvm_ota_health_t *h, uint64_t now_us);

/** Required probes that have never reported alive (bitmask over the probe table). */
uint32_t kvm_ota_health_missing(const kvm_ota_health_t *h);

/** Name of probe `idx`, or "" when out of range. Used for the status line and the console. */
const char *kvm_ota_health_probe_name(const kvm_ota_health_t *h, uint8_t idx);

#ifdef __cplusplus
}
#endif
