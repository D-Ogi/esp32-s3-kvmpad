// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host doubles for the TASK-110 OTA state machine: a flash slot with an otadata double, a byte-stream
// image source with fault injection, a synthetic image builder and a HID/maintenance interlock
// recorder. Header-only so each suite gets its own copy and the tests stay order-independent.
//
// The otadata double is the part that matters for AT-FAULT-08. ESP-IDF keeps two 32-byte copies with
// their own CRC and boots the highest sequence number whose CRC is valid, so a reset in the middle
// of the switch leaves the previous copy in charge. `fake_slot_boot_slot()` answers exactly that
// question, and `power_cut_after` makes any flash operation the last one that happens.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "update_image.h"
#include "update_manager.h"

#define FAKE_SLOT_BYTES (64u * 1024u) // enough for every test image; the real slot is 4 MiB

// ---- synthetic images ------------------------------------------------------------------------

typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint16_t chip_id;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint32_t desc_magic;
    uint32_t secure_version;
    const char *version;
    const char *project_name;
    size_t total_len;
} fake_image_spec_t;

#define FAKE_IMAGE_SPEC_DEFAULT                                                                      \
    {                                                                                                \
        .magic = KVM_OTA_IMAGE_MAGIC, .segment_count = 4u, .chip_id = KVM_OTA_CHIP_ID_ESP32S3,       \
        .min_chip_rev_full = 2u, .max_chip_rev_full = 0u, .desc_magic = KVM_OTA_APP_DESC_MAGIC,      \
        .secure_version = 1u, .version = "1.0.0", .project_name = "kvmpad", .total_len = 2048u,      \
    }

static inline void fake_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void fake_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/** Build an image into `buf` (capacity `cap`); returns the length written. */
static inline size_t fake_image_build(const fake_image_spec_t *s, uint8_t *buf, size_t cap)
{
    size_t len = s->total_len;
    if (len > cap) {
        len = cap;
    }
    memset(buf, 0xA5, len); // payload bytes that are neither 0x00 nor 0xFF
    if (len >= KVM_OTA_IMAGE_HEADER_LEN) {
        buf[0] = s->magic;
        buf[1] = s->segment_count;
        fake_put16(buf + 12, s->chip_id);
        fake_put16(buf + 15, s->min_chip_rev_full);
        fake_put16(buf + 17, s->max_chip_rev_full);
        buf[23] = 1u; // hash_appended
    }
    if (len >= KVM_OTA_IMAGE_PROBE_BYTES) {
        uint8_t *d = buf + KVM_OTA_APP_DESC_OFF;
        memset(d, 0, KVM_OTA_APP_DESC_LEN);
        fake_put32(d + 0, s->desc_magic);
        fake_put32(d + 4, s->secure_version);
        strncpy((char *)d + 16, s->version, KVM_OTA_STR_LEN);
        strncpy((char *)d + 48, s->project_name, KVM_OTA_STR_LEN);
        strncpy((char *)d + 112, "v6.1", KVM_OTA_STR_LEN);
    }
    return len;
}

/** The info record a running image with these properties would produce. */
static inline kvm_ota_image_info_t fake_image_info(const fake_image_spec_t *s)
{
    uint8_t buf[KVM_OTA_IMAGE_PROBE_BYTES];
    fake_image_spec_t spec = *s;
    spec.total_len = sizeof buf;
    (void)fake_image_build(&spec, buf, sizeof buf);
    kvm_ota_image_info_t info;
    kvm_ota_image_parse(buf, sizeof buf, &info);
    return info;
}

// ---- otadata + slot --------------------------------------------------------------------------

typedef struct {
    uint32_t seq;
    bool crc_ok;
    uint8_t slot; // which app slot this copy selects
} fake_otadata_copy_t;

typedef struct {
    // clock
    uint64_t now_us;

    // HID interlock. `hold_not_neutral` models a HID path that never reaches neutral;
    // `neutral_delay_us` models the real board, where RELEASE_ALL empties the ledger at once but
    // the neutral reports still have to be handed to the HOST.
    bool neutral;
    bool hold_not_neutral;
    uint64_t neutral_delay_us;
    uint64_t neutral_at_us;
    bool maintenance;
    uint32_t maintenance_on_calls;
    uint32_t maintenance_off_calls;
    uint32_t revoke_calls;
    bool wrote_while_not_neutral; // invariant 1: must stay false

    // slot
    uint8_t slot[FAKE_SLOT_BYTES];
    size_t slot_len;
    bool slot_open;
    bool begin_fails;
    bool end_invalid;   // signature / image verification fails
    bool end_errors;    // flash-level failure in esp_ota_end()
    bool write_fails_at; // non-zero: fail the write that would cross this many bytes
    bool set_boot_fails;
    bool set_boot_torn;  // the new otadata copy is written with a bad CRC (power cut mid-switch)
    bool mark_valid_fails;
    bool readback_corrupt;
    bool readback_fails;
    size_t written;
    uint32_t begin_calls;
    uint32_t write_calls;
    uint32_t end_calls;
    uint32_t abort_calls;
    uint32_t set_boot_calls;
    uint32_t mark_valid_calls;
    uint32_t rollback_calls;
    uint32_t reboot_calls;

    // otadata: two copies, as on the device
    fake_otadata_copy_t ota[2];
    uint8_t running_slot;

    // power cut: after this many flash operations nothing else happens
    uint32_t flash_ops;
    uint32_t power_cut_after; // 0 = never
    bool powered_off;

    // running image
    kvm_ota_image_info_t running;
    uint16_t chip_rev;

    // progress log
    kvm_ota_state_t last_progress_state;
    uint8_t last_percent;
    uint32_t progress_calls;
} fake_dev_t;

static inline void fake_dev_init(fake_dev_t *d, const kvm_ota_image_info_t *running)
{
    memset(d, 0, sizeof *d);
    d->neutral = true;
    d->chip_rev = 2u; // board #1 measured v0.2 -> min_chip_rev_full 2 (hardware/boards.md)
    d->ota[0] = (fake_otadata_copy_t){.seq = 1u, .crc_ok = true, .slot = 0u};
    d->ota[1] = (fake_otadata_copy_t){.seq = 0u, .crc_ok = false, .slot = 1u};
    d->running_slot = 0u;
    d->power_cut_after = 0u;
    if (running != NULL) {
        d->running = *running;
    }
}

/** Which app slot the bootloader would pick: the highest sequence with a valid CRC. */
static inline uint8_t fake_slot_boot_slot(const fake_dev_t *d)
{
    const fake_otadata_copy_t *best = NULL;
    for (unsigned i = 0; i < 2u; i++) {
        if (!d->ota[i].crc_ok) {
            continue;
        }
        if (best == NULL || d->ota[i].seq > best->seq) {
            best = &d->ota[i];
        }
    }
    return best != NULL ? best->slot : 0u; // no valid copy -> the factory/first app slot
}

/** True when exactly one otadata copy is authoritative and it is readable. */
static inline bool fake_otadata_consistent(const fake_dev_t *d)
{
    unsigned valid = 0u;
    for (unsigned i = 0; i < 2u; i++) {
        if (d->ota[i].crc_ok) {
            valid++;
        }
    }
    if (valid == 0u) {
        return false;
    }
    if (valid == 2u && d->ota[0].seq == d->ota[1].seq) {
        return false; // two copies claiming the same sequence: ambiguous
    }
    return true;
}

static inline bool fake_flash_op(fake_dev_t *d)
{
    if (d->powered_off) {
        return false;
    }
    d->flash_ops++;
    if (d->power_cut_after != 0u && d->flash_ops > d->power_cut_after) {
        d->powered_off = true;
        return false;
    }
    return true;
}

// ---- platform hooks --------------------------------------------------------------------------

static inline uint64_t fk_now(void *ctx)
{
    return ((fake_dev_t *)ctx)->now_us;
}

static inline void fk_maintenance(void *ctx, bool on)
{
    fake_dev_t *d = ctx;
    d->maintenance = on;
    if (on) {
        d->maintenance_on_calls++;
        if (!d->hold_not_neutral) {
            if (d->neutral_delay_us != 0u) {
                d->neutral_at_us = d->now_us + d->neutral_delay_us;
            } else {
                d->neutral = true; // hid_core RELEASE_ALL empties the ledger synchronously
            }
        }
    } else {
        d->maintenance_off_calls++;
    }
}

static inline bool fk_neutral(void *ctx)
{
    fake_dev_t *d = ctx;
    if (d->hold_not_neutral) {
        return false;
    }
    if (d->neutral_at_us != 0u) {
        return d->now_us >= d->neutral_at_us;
    }
    return d->neutral;
}

static inline void fk_revoke(void *ctx)
{
    ((fake_dev_t *)ctx)->revoke_calls++;
}

static inline void fk_progress(void *ctx, kvm_ota_state_t st, uint8_t pct)
{
    fake_dev_t *d = ctx;
    d->last_progress_state = st;
    d->last_percent = pct;
    d->progress_calls++;
}

static inline size_t fk_slot_size(void *ctx)
{
    (void)ctx;
    return FAKE_SLOT_BYTES;
}

static inline kvm_ota_slot_rc_t fk_slot_begin(void *ctx, size_t image_size)
{
    fake_dev_t *d = ctx;
    if (!fake_flash_op(d)) {
        return KVM_OTA_SLOT_ERROR;
    }
    d->begin_calls++;
    if (d->begin_fails || image_size > FAKE_SLOT_BYTES) {
        return KVM_OTA_SLOT_ERROR;
    }
    d->slot_open = true;
    d->written = 0u;
    d->slot_len = 0u;
    memset(d->slot, 0xFF, sizeof d->slot); // erased flash
    return KVM_OTA_SLOT_OK;
}

static inline kvm_ota_slot_rc_t fk_slot_write(void *ctx, const void *data, size_t len)
{
    fake_dev_t *d = ctx;
    if (!fake_flash_op(d)) {
        return KVM_OTA_SLOT_ERROR;
    }
    d->write_calls++;
    if (!fk_neutral(d)) {
        d->wrote_while_not_neutral = true; // invariant 1 violated
    }
    if (!d->slot_open || d->written + len > sizeof d->slot) {
        return KVM_OTA_SLOT_ERROR;
    }
    if (d->write_fails_at && d->written + len >= (size_t)KVM_OTA_IMAGE_PROBE_BYTES * 2u) {
        return KVM_OTA_SLOT_ERROR;
    }
    memcpy(d->slot + d->written, data, len);
    d->written += len;
    d->slot_len = d->written;
    return KVM_OTA_SLOT_OK;
}

static inline kvm_ota_slot_rc_t fk_slot_end(void *ctx)
{
    fake_dev_t *d = ctx;
    if (!fake_flash_op(d)) {
        return KVM_OTA_SLOT_ERROR;
    }
    d->end_calls++;
    d->slot_open = false;
    if (d->end_invalid) {
        return KVM_OTA_SLOT_INVALID;
    }
    if (d->end_errors) {
        return KVM_OTA_SLOT_ERROR;
    }
    return KVM_OTA_SLOT_OK;
}

static inline void fk_slot_abort(void *ctx)
{
    fake_dev_t *d = ctx;
    d->abort_calls++;
    d->slot_open = false;
}

static inline kvm_ota_slot_rc_t fk_slot_read(void *ctx, size_t offset, void *buf, size_t len)
{
    fake_dev_t *d = ctx;
    if (d->readback_fails || offset + len > sizeof d->slot) {
        return KVM_OTA_SLOT_ERROR;
    }
    memcpy(buf, d->slot + offset, len);
    if (d->readback_corrupt) {
        ((uint8_t *)buf)[KVM_OTA_APP_DESC_OFF + 4u] ^= 0xFFu; // flip the secure_version low byte
    }
    return KVM_OTA_SLOT_OK;
}

static inline kvm_ota_slot_rc_t fk_slot_set_boot(void *ctx)
{
    fake_dev_t *d = ctx;
    if (!fake_flash_op(d)) {
        return KVM_OTA_SLOT_ERROR;
    }
    d->set_boot_calls++;
    if (d->set_boot_fails) {
        return KVM_OTA_SLOT_ERROR; // otadata untouched
    }
    // esp_ota_set_boot_partition() rewrites the *inactive* copy with seq+1, so the active copy
    // stays readable the whole time.
    unsigned active = d->ota[0].seq >= d->ota[1].seq ? 0u : 1u;
    unsigned spare = active ^ 1u;
    uint32_t next = d->ota[active].seq + 1u;
    d->ota[spare].seq = next;
    d->ota[spare].slot = (uint8_t)(d->running_slot ^ 1u);
    d->ota[spare].crc_ok = !d->set_boot_torn;
    return d->set_boot_torn ? KVM_OTA_SLOT_ERROR : KVM_OTA_SLOT_OK;
}

static inline void fk_running_image(void *ctx, kvm_ota_image_info_t *out)
{
    *out = ((fake_dev_t *)ctx)->running;
}

static inline uint16_t fk_chip_rev(void *ctx)
{
    return ((fake_dev_t *)ctx)->chip_rev;
}

static inline kvm_ota_slot_rc_t fk_mark_valid(void *ctx)
{
    fake_dev_t *d = ctx;
    d->mark_valid_calls++;
    if (d->mark_valid_fails) {
        return KVM_OTA_SLOT_ERROR;
    }
    for (unsigned i = 0; i < 2u; i++) {
        if (d->ota[i].slot == d->running_slot) {
            d->ota[i].crc_ok = true;
        }
    }
    return KVM_OTA_SLOT_OK;
}

static inline void fk_rollback(void *ctx)
{
    fake_dev_t *d = ctx;
    d->rollback_calls++;
    // The running copy is marked invalid, so the bootloader falls back to the other slot.
    for (unsigned i = 0; i < 2u; i++) {
        if (d->ota[i].slot == d->running_slot) {
            d->ota[i].crc_ok = false;
        }
    }
    d->reboot_calls++;
}

static inline void fk_reboot(void *ctx)
{
    ((fake_dev_t *)ctx)->reboot_calls++;
}

static inline kvm_ota_platform_t fake_platform(fake_dev_t *d)
{
    kvm_ota_platform_t p = {
        .ctx = d,
        .now_us = fk_now,
        .maintenance = fk_maintenance,
        .hid_neutral = fk_neutral,
        .revoke_sessions = fk_revoke,
        .progress = fk_progress,
        .slot_size = fk_slot_size,
        .slot_begin = fk_slot_begin,
        .slot_write = fk_slot_write,
        .slot_end = fk_slot_end,
        .slot_abort = fk_slot_abort,
        .slot_read = fk_slot_read,
        .slot_set_boot = fk_slot_set_boot,
        .running_image = fk_running_image,
        .chip_rev_full = fk_chip_rev,
        .mark_valid = fk_mark_valid,
        .rollback_and_reboot = fk_rollback,
        .reboot = fk_reboot,
    };
    return p;
}

// ---- image source ----------------------------------------------------------------------------

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t declared;    // what open() reports; 0 = unknown
    size_t pos;
    size_t cut_at;      // 0 = never: stop delivering after this many bytes (truncated transfer)
    bool open_fails;
    bool read_fails_at_end;
    size_t read_error_at; // 0 = never: return an error once pos reaches this
    uint32_t open_calls;
    uint32_t read_calls;
    uint32_t close_calls;
    size_t max_chunk;   // 0 = whatever the caller asks for
} fake_src_t;

static inline int fs_open(void *ctx, const char *url, size_t *out_size)
{
    fake_src_t *s = ctx;
    s->open_calls++;
    if (s->open_fails || url == NULL) {
        return -1;
    }
    *out_size = s->declared;
    return 0;
}

static inline int fs_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    fake_src_t *s = ctx;
    s->read_calls++;
    if (s->read_error_at != 0u && s->pos >= s->read_error_at) {
        return -1;
    }
    size_t limit = s->cut_at != 0u && s->cut_at < s->len ? s->cut_at : s->len;
    size_t left = s->pos < limit ? limit - s->pos : 0u;
    if (left == 0u) {
        if (s->read_fails_at_end) {
            return -1;
        }
        *out_len = 0u;
        return 0;
    }
    size_t take = cap < left ? cap : left;
    if (s->max_chunk != 0u && take > s->max_chunk) {
        take = s->max_chunk;
    }
    memcpy(buf, s->data + s->pos, take);
    s->pos += take;
    *out_len = take;
    return 0;
}

static inline void fs_close(void *ctx)
{
    ((fake_src_t *)ctx)->close_calls++;
}

static inline kvm_ota_source_t fake_source(fake_src_t *s)
{
    kvm_ota_source_t src = {.ctx = s, .open = fs_open, .read = fs_read, .close = fs_close};
    return src;
}

// ---- health probes ---------------------------------------------------------------------------

typedef struct {
    bool usb;
    bool hid;
    bool lvgl;
    bool touch;
} fake_probes_t;

static inline bool pr_usb(void *ctx) { return ((fake_probes_t *)ctx)->usb; }
static inline bool pr_hid(void *ctx) { return ((fake_probes_t *)ctx)->hid; }
static inline bool pr_lvgl(void *ctx) { return ((fake_probes_t *)ctx)->lvgl; }
static inline bool pr_touch(void *ctx) { return ((fake_probes_t *)ctx)->touch; }

static inline void fake_add_probes(kvm_ota_t *m, fake_probes_t *p)
{
    (void)kvm_ota_add_probe(m, "usb", pr_usb, p, true);
    (void)kvm_ota_add_probe(m, "hid", pr_hid, p, true);
    (void)kvm_ota_add_probe(m, "lvgl", pr_lvgl, p, true);
    (void)kvm_ota_add_probe(m, "touch", pr_touch, p, true);
}

static inline void fake_probes_all_up(fake_probes_t *p)
{
    p->usb = true;
    p->hid = true;
    p->lvgl = true;
    p->touch = true;
}

// ---- driver ----------------------------------------------------------------------------------

/** Poll until the state changes or `max` polls pass; the clock advances `step_us` per poll. */
static inline uint32_t fake_run(kvm_ota_t *m, fake_dev_t *d, uint32_t max, uint64_t step_us)
{
    kvm_ota_state_t from = m->state;
    uint32_t n = 0u;
    while (n < max && m->state == from) {
        d->now_us += step_us;
        kvm_ota_poll(m, d->now_us);
        n++;
    }
    return n;
}

/** Poll until `want` is reached or `max` polls pass. Returns true when `want` was reached. */
static inline bool fake_run_until(kvm_ota_t *m, fake_dev_t *d, kvm_ota_state_t want, uint32_t max, uint64_t step_us)
{
    for (uint32_t n = 0u; n < max; n++) {
        if (m->state == want) {
            return true;
        }
        d->now_us += step_us;
        kvm_ota_poll(m, d->now_us);
    }
    return m->state == want;
}
