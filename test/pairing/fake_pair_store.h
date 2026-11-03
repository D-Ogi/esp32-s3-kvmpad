// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// In-RAM paired-client register for the host pairing tests. It mirrors what config_store already
// implements for the firmware (KVM_CONFIG_MAX_PAIRED = 8, immediate write, no token kept), so a test
// that passes here exercises the same contract the integrator wires up.
#pragma once

#include <stdbool.h>
#include <string.h>

#include "kvm_pairing_platform.h"

#define FAKE_PAIR_MAX 8u

typedef struct {
    kvm_pair_record_t rec[FAKE_PAIR_MAX];
    uint8_t count;
    bool add_fails;   // force the full/failed-write path
    uint32_t adds;
    uint32_t removes;
} fake_pair_store_t;

static int fake_pair_add(void *ctx, const kvm_pair_record_t *rec)
{
    fake_pair_store_t *s = (fake_pair_store_t *)ctx;
    if (s->add_fails || s->count >= FAKE_PAIR_MAX) {
        return -1;
    }
    s->rec[s->count] = *rec;
    s->adds++;
    return (int)s->count++;
}

static uint8_t fake_pair_count(void *ctx)
{
    return ((fake_pair_store_t *)ctx)->count;
}

static bool fake_pair_get(void *ctx, uint8_t index, kvm_pair_record_t *out)
{
    fake_pair_store_t *s = (fake_pair_store_t *)ctx;
    if (index >= s->count) {
        return false;
    }
    *out = s->rec[index];
    return true;
}

static bool fake_pair_remove(void *ctx, uint8_t index)
{
    fake_pair_store_t *s = (fake_pair_store_t *)ctx;
    if (index >= s->count) {
        return false;
    }
    for (uint8_t i = index; i + 1u < s->count; i++) {
        s->rec[i] = s->rec[i + 1u];
    }
    s->count--;
    memset(&s->rec[s->count], 0, sizeof(s->rec[s->count]));
    s->removes++;
    return true;
}

static uint8_t fake_pair_remove_all(void *ctx)
{
    fake_pair_store_t *s = (fake_pair_store_t *)ctx;
    const uint8_t gone = s->count;
    memset(s->rec, 0, sizeof(s->rec));
    s->count = 0;
    s->removes += gone;
    return gone;
}

static void fake_pair_store_init(fake_pair_store_t *s, kvm_pair_store_t *ops)
{
    memset(s, 0, sizeof(*s));
    ops->add = fake_pair_add;
    ops->count = fake_pair_count;
    ops->get = fake_pair_get;
    ops->remove = fake_pair_remove;
    ops->remove_all = fake_pair_remove_all;
    ops->ctx = s;
}
