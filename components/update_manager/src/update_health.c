// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Post-update health check (TASK-110). Pure C; contract in update_health.h.
#include "update_health.h"

#include <string.h>

bool kvm_ota_heartbeat_alive(kvm_ota_heartbeat_t *h, uint32_t counter, uint64_t now_us)
{
    if (h == NULL) {
        return false;
    }
    if (!h->seeded) {
        h->seeded = true;
        h->moved = false;
        h->last = counter;
        h->last_change_us = now_us;
        return false; // the first sample only seeds; nothing is claimed from one sample
    }
    if (counter != h->last) {
        h->last = counter;
        h->last_change_us = now_us;
        h->moved = true;
    }
    return h->moved && now_us >= h->last_change_us && now_us - h->last_change_us <= h->stale_us;
}

void kvm_ota_health_init(kvm_ota_health_t *h, uint8_t need_consecutive)
{
    if (h == NULL) {
        return;
    }
    memset(h, 0, sizeof *h);
    if (need_consecutive < 1u) {
        need_consecutive = 1u;
    }
    if (need_consecutive > 16u) {
        need_consecutive = 16u;
    }
    h->need_consecutive = need_consecutive;
    h->result = KVM_OTA_HEALTH_PENDING;
}

bool kvm_ota_health_add(kvm_ota_health_t *h, const char *name, bool (*alive)(void *), void *ctx, bool required)
{
    if (h == NULL || name == NULL || alive == NULL || h->count >= KVM_OTA_PROBE_MAX) {
        return false;
    }
    uint8_t i = h->count;
    h->p[i].name = name;
    h->p[i].alive = alive;
    h->p[i].ctx = ctx;
    h->p[i].required = required;
    if (required) {
        h->required_mask |= (uint32_t)1u << i;
    }
    h->count = (uint8_t)(i + 1u);
    return true;
}

void kvm_ota_health_arm(kvm_ota_health_t *h, uint64_t deadline_us)
{
    if (h == NULL) {
        return;
    }
    h->deadline_us = deadline_us;
    h->consecutive = 0u;
    h->ever_mask = 0u;
    h->last_mask = 0u;
    h->sweeps = 0u;
    h->result = KVM_OTA_HEALTH_PENDING;
}

kvm_ota_health_result_t kvm_ota_health_sweep(kvm_ota_health_t *h, uint64_t now_us)
{
    if (h == NULL) {
        return KVM_OTA_HEALTH_TIMEOUT;
    }
    if (h->result != KVM_OTA_HEALTH_PENDING) {
        return h->result;
    }

    uint32_t mask = 0u;
    for (uint8_t i = 0; i < h->count; i++) {
        if (h->p[i].alive(h->p[i].ctx)) {
            mask |= (uint32_t)1u << i;
        }
    }
    h->last_mask = mask;
    h->ever_mask |= mask;
    h->sweeps++;

    if ((mask & h->required_mask) == h->required_mask) {
        h->consecutive = (uint8_t)(h->consecutive + 1u);
        if (h->consecutive >= h->need_consecutive) {
            h->result = KVM_OTA_HEALTH_PASS;
            return h->result;
        }
    } else {
        h->consecutive = 0u; // a dropped subsystem restarts the count, it does not shorten it
    }

    // The deadline is checked after the sweep: a probe that comes up exactly at the deadline still
    // counts, and a check armed with a deadline already in the past still gets one honest sweep.
    if (now_us >= h->deadline_us) {
        h->result = KVM_OTA_HEALTH_TIMEOUT;
    }
    return h->result;
}

uint32_t kvm_ota_health_missing(const kvm_ota_health_t *h)
{
    if (h == NULL) {
        return 0u;
    }
    return h->required_mask & ~h->ever_mask;
}

const char *kvm_ota_health_probe_name(const kvm_ota_health_t *h, uint8_t idx)
{
    if (h == NULL || idx >= h->count || h->p[idx].name == NULL) {
        return "";
    }
    return h->p[idx].name;
}
