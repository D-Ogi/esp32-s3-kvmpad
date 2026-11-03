// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// In-memory settings backend for the host tests, with fault injection.
//
// Two different failure shapes matter for TASK-090:
//   * `power_loss` — the write reports an error and the stored record is left exactly as it was.
//     This is what NVS gives us (a blob is replaced by writing the new value and then moving the
//     index entry), so the store must come back up on the previous record.
//   * `torn_write` — the write reports an error and leaves a truncated record behind. NVS should
//     never do this, but the store must survive a backend that does: the CRC check turns it into
//     a recovery to the factory defaults instead of a crash or a half-read record.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config_store.h"

typedef struct {
    uint8_t blob[KVM_CONFIG_RECORD_MAX];
    size_t len;
    bool present;

    // fault injection
    bool fail_read;
    bool fail_erase;
    int fail_writes;  // the next N write calls fail and keep the stored record
    bool power_loss;  // the next write fails and keeps the stored record (one shot)
    bool torn_write;  // the next write fails after storing a truncated record (one shot)

    // counters
    unsigned reads;
    unsigned writes;
    unsigned erases;
    unsigned write_errors;
} fake_nvs_t;

static inline void fake_nvs_reset(fake_nvs_t *f)
{
    memset(f, 0, sizeof *f);
}

// Seed the backend with a raw blob, as if a previous firmware had written it.
static inline void fake_nvs_seed(fake_nvs_t *f, const void *blob, size_t len)
{
    if (len > sizeof f->blob) {
        len = sizeof f->blob;
    }
    memcpy(f->blob, blob, len);
    f->len = len;
    f->present = true;
}

static inline kvm_config_io_t fake_nvs_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    fake_nvs_t *f = (fake_nvs_t *)ctx;
    f->reads++;
    if (f->fail_read) {
        return KVM_CONFIG_IO_ERROR;
    }
    if (!f->present) {
        return KVM_CONFIG_IO_ABSENT;
    }
    if (f->len > cap) {
        return KVM_CONFIG_IO_ERROR; // mirrors ESP_ERR_NVS_INVALID_LENGTH
    }
    memcpy(buf, f->blob, f->len);
    *out_len = f->len;
    return KVM_CONFIG_IO_OK;
}

static inline kvm_config_io_t fake_nvs_write(void *ctx, const void *buf, size_t len)
{
    fake_nvs_t *f = (fake_nvs_t *)ctx;
    f->writes++;

    if (f->power_loss) {
        f->power_loss = false;
        f->write_errors++;
        return KVM_CONFIG_IO_ERROR; // stored record untouched
    }
    if (f->torn_write) {
        f->torn_write = false;
        f->write_errors++;
        size_t half = len / 2u;
        memcpy(f->blob, buf, half);
        f->len = half;
        f->present = true;
        return KVM_CONFIG_IO_ERROR;
    }
    if (f->fail_writes > 0) {
        f->fail_writes--;
        f->write_errors++;
        return KVM_CONFIG_IO_ERROR;
    }
    if (len > sizeof f->blob) {
        f->write_errors++;
        return KVM_CONFIG_IO_ERROR;
    }

    // Clear first, so a shorter record leaves no tail of the previous one behind and
    // fake_nvs_contains() answers only about what is stored now.
    memset(f->blob, 0, sizeof f->blob);
    memcpy(f->blob, buf, len);
    f->len = len;
    f->present = true;
    return KVM_CONFIG_IO_OK;
}

static inline kvm_config_io_t fake_nvs_erase(void *ctx)
{
    fake_nvs_t *f = (fake_nvs_t *)ctx;
    f->erases++;
    if (f->fail_erase) {
        return KVM_CONFIG_IO_ERROR;
    }
    if (!f->present) {
        return KVM_CONFIG_IO_ABSENT;
    }
    memset(f->blob, 0, sizeof f->blob);
    f->len = 0;
    f->present = false;
    return KVM_CONFIG_IO_OK;
}

static inline kvm_config_backend_t fake_nvs_backend(fake_nvs_t *f)
{
    static const kvm_config_backend_ops_t ops = {
        .read = fake_nvs_read,
        .write = fake_nvs_write,
        .erase = fake_nvs_erase,
    };
    kvm_config_backend_t b;
    b.ops = &ops;
    b.ctx = f;
    return b;
}

// True when `needle` appears anywhere in the stored record. Used to prove that a secret the
// caller held never reached flash (AT-SEC-08).
static inline bool fake_nvs_contains(const fake_nvs_t *f, const void *needle, size_t n)
{
    if (n == 0u || n > f->len) {
        return false;
    }
    for (size_t i = 0; i + n <= f->len; i++) {
        if (memcmp(f->blob + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}
