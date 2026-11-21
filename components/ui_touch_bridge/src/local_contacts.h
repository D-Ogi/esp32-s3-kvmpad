// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include "kvm_touch.h"

typedef struct {
    int16_t x, y;
    bool pressed;
} local_pointer_t;

typedef struct {
    uint64_t timestamp_us;
    unsigned count;
    kvm_touch_point_t points[2];
    local_pointer_t pointer;
} local_contacts_t;

typedef struct {
    local_contacts_t pending[8];
    unsigned used;
    bool blocked;
    uint32_t last_seq;
    uint64_t last_time;
    kvm_touch_point_t origin[2];
    local_contacts_t last;
} local_contacts_queue_t;

void local_contacts_cancel(local_contacts_queue_t *q);
void local_contacts_expire(local_contacts_queue_t *q, uint64_t now_us);
void local_contacts_post(local_contacts_queue_t *q, const kvm_touch_sample_t *s, local_pointer_t pointer);
