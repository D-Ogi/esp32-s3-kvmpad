// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Settings store: load, write debounce, pairing operations, factory reset (TASK-090, TECH-050).
// Pure C, no allocation, no I/O of its own; see config_store.h for the write policy and
// src/config_store_nvs.c for the firmware backend.
//
// The caller owns the clock: every mutating call takes `now_ms` (a monotonic millisecond counter)
// so the whole policy is testable with a fake clock and the component never pulls in a timer API.
// One serialization buffer of KVM_CONFIG_RECORD_MAX bytes lives on the stack of whichever task
// calls tick()/flush(); size that task's stack accordingly.

#include "config_store.h"

#include <string.h>

void kvm_config_debounce_defaults(kvm_config_debounce_t *out)
{
    if (!out) {
        return;
    }
    out->quiet_ms = 1500u;
    out->max_defer_ms = 10000u;
    out->min_interval_ms = 2000u;
}

static bool backend_usable(const kvm_config_store_t *store)
{
    return store && store->backend.ops;
}

// A deliberate change made by the person at the device. Clears the future-schema hold: once the
// user has touched a setting, keeping the newer record is no longer the safer option.
static void mark_user_change(kvm_config_store_t *store, uint64_t now_ms)
{
    store->hold_writes = false;
    store->stats.changes++;
    if (store->dirty) {
        store->stats.coalesced++;
    } else {
        store->dirty = true;
        store->dirty_since_ms = now_ms;
    }
    store->last_change_ms = now_ms;
}

// A change that did not come from the user (a record repaired or migrated at load time).
static void mark_internal_change(kvm_config_store_t *store, uint64_t now_ms)
{
    if (!store->dirty) {
        store->dirty = true;
        store->dirty_since_ms = now_ms;
    }
    store->last_change_ms = now_ms;
}

static bool write_record(kvm_config_store_t *store, uint64_t now_ms, bool immediate)
{
    uint8_t buf[KVM_CONFIG_RECORD_MAX];
    size_t len = kvm_config_serialize(&store->cfg, buf, sizeof buf);
    if (len == 0u || !backend_usable(store) || !store->backend.ops->write) {
        store->stats.write_failures++;
        return false;
    }

    kvm_config_io_t io = store->backend.ops->write(store->backend.ctx, buf, len);
    store->last_write_ms = now_ms;
    store->wrote_once = true;
    memset(buf, 0, sizeof buf);

    if (io != KVM_CONFIG_IO_OK) {
        store->stats.write_failures++;
        return false; // stays dirty: a later tick retries, the previous record is still in flash
    }
    store->dirty = false;
    store->stats.writes++;
    if (immediate) {
        store->stats.immediate_writes++;
    }
    return true;
}

kvm_config_load_status_t kvm_config_store_init(kvm_config_store_t *store,
                                              const kvm_config_backend_t *backend,
                                              const kvm_config_debounce_t *debounce,
                                              uint64_t now_ms)
{
    if (!store) {
        return KVM_CONFIG_LOAD_RECOVERED;
    }
    memset(store, 0, sizeof *store);
    kvm_config_defaults(&store->cfg);
    if (backend) {
        store->backend = *backend;
    }
    if (debounce) {
        store->debounce = *debounce;
    } else {
        kvm_config_debounce_defaults(&store->debounce);
    }

    uint8_t buf[KVM_CONFIG_RECORD_MAX];
    size_t len = 0;
    kvm_config_io_t io = KVM_CONFIG_IO_ERROR;
    if (backend_usable(store) && store->backend.ops->read) {
        io = store->backend.ops->read(store->backend.ctx, buf, sizeof buf, &len);
    }

    if (io == KVM_CONFIG_IO_ABSENT) {
        store->load_status = KVM_CONFIG_LOAD_DEFAULTS;
    } else if (io != KVM_CONFIG_IO_OK) {
        store->load_status = KVM_CONFIG_LOAD_RECOVERED;
    } else {
        store->load_status =
            kvm_config_deserialize(buf, len, &store->cfg, &store->load_repair);
    }
    memset(buf, 0, sizeof buf);

    store->recovered = (store->load_status == KVM_CONFIG_LOAD_RECOVERED);
    store->future_schema = (store->load_status == KVM_CONFIG_LOAD_FUTURE_SCHEMA);

    switch (store->load_status) {
    case KVM_CONFIG_LOAD_RECOVERED:
    case KVM_CONFIG_LOAD_MIGRATED:
    case KVM_CONFIG_LOAD_REPAIRED:
        // The stored record is broken or stale: schedule a replacement through the normal
        // debounce so a boot loop cannot turn into a write loop.
        mark_internal_change(store, now_ms);
        break;
    case KVM_CONFIG_LOAD_FUTURE_SCHEMA:
        // Defaults are in force, but the newer record stays untouched until the user changes
        // something, so a temporary downgrade does not wipe the configuration.
        mark_internal_change(store, now_ms);
        store->hold_writes = true;
        break;
    case KVM_CONFIG_LOAD_OK:
    case KVM_CONFIG_LOAD_DEFAULTS:
    default:
        break;
    }
    return store->load_status;
}

const kvm_config_t *kvm_config_store_get(const kvm_config_store_t *store)
{
    return store ? &store->cfg : NULL;
}

bool kvm_config_store_is_dirty(const kvm_config_store_t *store)
{
    return store && store->dirty;
}

// ---------------------------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------------------------

bool kvm_config_store_set_field(kvm_config_store_t *store, kvm_config_field_t field, uint32_t value,
                                uint64_t now_ms)
{
    if (!store) {
        return false;
    }
    uint32_t min = 0, max = 0, def = 0;
    if (!kvm_config_field_range(field, &min, &max, &def)) {
        return false;
    }
    if (value < min || value > max) {
        return false;
    }
    if (kvm_config_get(&store->cfg, field) == value) {
        return true; // no-op: never costs a flash write
    }
    if (!kvm_config_set(&store->cfg, field, value)) {
        return false;
    }
    mark_user_change(store, now_ms);
    return true;
}

bool kvm_config_store_set_layout(kvm_config_store_t *store, const char *id, uint64_t now_ms)
{
    if (!store || !kvm_config_layout_id_valid(id)) {
        return false;
    }
    if (strcmp(store->cfg.host_layout, id) == 0) {
        return true;
    }
    if (!kvm_config_set_layout_id(&store->cfg, id)) {
        return false;
    }
    mark_user_change(store, now_ms);
    return true;
}

// The two HOST-layout lists follow set_layout(): validate on a copy of the record, write nothing
// when the list is unusable, and spend a flash write only when the stored list actually changes.
bool kvm_config_store_set_favorites(kvm_config_store_t *store,
                                    const char ids[][KVM_CONFIG_LAYOUT_ID_CAP], uint8_t count,
                                    uint64_t now_ms)
{
    if (!store) {
        return false;
    }
    kvm_config_t candidate = store->cfg;
    if (!kvm_config_set_favorites(&candidate, ids, count)) {
        return false;
    }
    if (candidate.host_fav_count == store->cfg.host_fav_count &&
        memcmp(candidate.host_fav, store->cfg.host_fav, sizeof candidate.host_fav) == 0) {
        return true; // no-op: never costs a flash write
    }
    memcpy(store->cfg.host_fav, candidate.host_fav, sizeof store->cfg.host_fav);
    store->cfg.host_fav_count = candidate.host_fav_count;
    mark_user_change(store, now_ms);
    return true;
}

bool kvm_config_store_set_recents(kvm_config_store_t *store,
                                  const char ids[][KVM_CONFIG_LAYOUT_ID_CAP], uint8_t count,
                                  uint64_t now_ms)
{
    if (!store) {
        return false;
    }
    kvm_config_t candidate = store->cfg;
    if (!kvm_config_set_recents(&candidate, ids, count)) {
        return false;
    }
    if (candidate.host_recent_count == store->cfg.host_recent_count &&
        memcmp(candidate.host_recent, store->cfg.host_recent, sizeof candidate.host_recent) == 0) {
        return true;
    }
    memcpy(store->cfg.host_recent, candidate.host_recent, sizeof store->cfg.host_recent);
    store->cfg.host_recent_count = candidate.host_recent_count;
    mark_user_change(store, now_ms);
    return true;
}

bool kvm_config_store_pin_toggle(kvm_config_store_t *store, const char *id, uint64_t now_ms)
{
    if (!store) {
        return false;
    }
    kvm_config_t candidate = store->cfg;
    if (!kvm_config_pin_toggle(&candidate, id)) {
        return false;
    }
    memcpy(store->cfg.host_fav, candidate.host_fav, sizeof store->cfg.host_fav);
    store->cfg.host_fav_count = candidate.host_fav_count;
    mark_user_change(store, now_ms);
    return true;
}

bool kvm_config_store_note_layout_use(kvm_config_store_t *store, const char *id, uint64_t now_ms)
{
    if (!store) {
        return false;
    }
    kvm_config_t candidate = store->cfg;
    if (!kvm_config_recent_push(&candidate, id)) {
        return false;
    }
    if (candidate.host_recent_count == store->cfg.host_recent_count &&
        memcmp(candidate.host_recent, store->cfg.host_recent, sizeof candidate.host_recent) == 0) {
        return true; // the newest already: no flash write
    }
    memcpy(store->cfg.host_recent, candidate.host_recent, sizeof store->cfg.host_recent);
    store->cfg.host_recent_count = candidate.host_recent_count;
    mark_user_change(store, now_ms);
    return true;
}

bool kvm_config_store_apply(kvm_config_store_t *store, const kvm_config_t *next, uint64_t now_ms,
                            kvm_config_repair_t *out_repair)
{
    if (!store || !next) {
        return false;
    }
    kvm_config_t candidate = *next;
    kvm_config_repair_t rep = kvm_config_validate(&candidate);
    if (out_repair) {
        *out_repair = rep;
    }
    if (memcmp(&store->cfg, &candidate, sizeof candidate) == 0) {
        return true;
    }
    store->cfg = candidate;
    mark_user_change(store, now_ms);
    return true;
}

bool kvm_config_store_set_wifi_cache(kvm_config_store_t *store, const uint8_t bssid[6],
                                     uint8_t channel, uint64_t now_ms)
{
    if (!store || !bssid || channel < 1u || channel > 14u) {
        return false;
    }
    kvm_wifi_cache_t next;
    memset(&next, 0, sizeof next);
    next.valid = 1u;
    memcpy(next.bssid, bssid, sizeof next.bssid);
    next.channel = channel;

    kvm_config_t candidate = store->cfg;
    candidate.wifi = next;
    kvm_config_repair_t rep = kvm_config_validate(&candidate);
    if (rep.wifi_repaired) {
        return false; // not a usable BSSID
    }
    if (memcmp(&store->cfg.wifi, &candidate.wifi, sizeof candidate.wifi) == 0) {
        return true;
    }
    store->cfg.wifi = candidate.wifi;
    mark_user_change(store, now_ms);
    return true;
}

void kvm_config_store_clear_wifi_cache(kvm_config_store_t *store, uint64_t now_ms)
{
    if (!store || store->cfg.wifi.valid == 0u) {
        if (store) {
            memset(&store->cfg.wifi, 0, sizeof store->cfg.wifi);
        }
        return;
    }
    memset(&store->cfg.wifi, 0, sizeof store->cfg.wifi);
    mark_user_change(store, now_ms);
}

// ---------------------------------------------------------------------------------------------
// Pairing register — written immediately (FR-726: a revoke takes effect at once)
// ---------------------------------------------------------------------------------------------

int kvm_config_store_pair_add(kvm_config_store_t *store, const char *name,
                              const uint8_t token_sha[KVM_CONFIG_TOKEN_SHA_LEN],
                              uint64_t paired_at_unix, bool trusted, uint64_t now_ms)
{
    if (!store) {
        return -1;
    }
    int at = kvm_config_pair_add(&store->cfg, name, token_sha, paired_at_unix, trusted);
    if (at < 0) {
        return -1;
    }
    mark_user_change(store, now_ms);
    (void)write_record(store, now_ms, true);
    return at;
}

bool kvm_config_store_pair_set_trusted(kvm_config_store_t *store, uint8_t index, bool trusted,
                                       uint64_t now_ms)
{
    if (!store || !kvm_config_pair_set_trusted(&store->cfg, index, trusted)) {
        return false;
    }
    mark_user_change(store, now_ms);
    return write_record(store, now_ms, true);
}

bool kvm_config_store_pair_remove(kvm_config_store_t *store, uint8_t index, uint64_t now_ms)
{
    if (!store || !kvm_config_pair_remove(&store->cfg, index)) {
        return false;
    }
    mark_user_change(store, now_ms);
    return write_record(store, now_ms, true);
}

uint8_t kvm_config_store_pair_remove_all(kvm_config_store_t *store, uint64_t now_ms)
{
    if (!store) {
        return 0;
    }
    uint8_t had = kvm_config_pair_remove_all(&store->cfg);
    if (had == 0u) {
        return 0;
    }
    mark_user_change(store, now_ms);
    (void)write_record(store, now_ms, true);
    return had;
}

bool kvm_config_store_factory_reset(kvm_config_store_t *store, uint64_t now_ms)
{
    if (!store) {
        return false;
    }
    bool erased = false;
    if (backend_usable(store) && store->backend.ops->erase) {
        kvm_config_io_t io = store->backend.ops->erase(store->backend.ctx);
        erased = (io == KVM_CONFIG_IO_OK || io == KVM_CONFIG_IO_ABSENT);
        if (erased) {
            store->stats.erases++;
        }
    }

    kvm_config_defaults(&store->cfg);
    store->recovered = false;
    store->future_schema = false;
    store->hold_writes = false;
    store->load_status = KVM_CONFIG_LOAD_DEFAULTS;
    memset(&store->load_repair, 0, sizeof store->load_repair);
    mark_user_change(store, now_ms);

    bool written = write_record(store, now_ms, true);
    return erased && written;
}

// ---------------------------------------------------------------------------------------------
// Write policy
// ---------------------------------------------------------------------------------------------

static uint64_t write_deadline(const kvm_config_store_t *store)
{
    uint64_t quiet = store->last_change_ms + store->debounce.quiet_ms;
    uint64_t defer = store->dirty_since_ms + store->debounce.max_defer_ms;
    uint64_t at = quiet < defer ? quiet : defer;
    if (store->wrote_once) {
        uint64_t earliest = store->last_write_ms + store->debounce.min_interval_ms;
        if (earliest > at) {
            at = earliest;
        }
    }
    return at;
}

uint32_t kvm_config_store_ms_until_write(const kvm_config_store_t *store, uint64_t now_ms)
{
    if (!store || !store->dirty || store->hold_writes) {
        return UINT32_MAX;
    }
    uint64_t at = write_deadline(store);
    if (at <= now_ms) {
        return 0u;
    }
    uint64_t left = at - now_ms;
    return left > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)left;
}

bool kvm_config_store_tick(kvm_config_store_t *store, uint64_t now_ms)
{
    if (!store || !store->dirty) {
        return false;
    }
    if (store->hold_writes) {
        store->stats.holds++;
        return false;
    }
    if (write_deadline(store) > now_ms) {
        return false;
    }
    return write_record(store, now_ms, false);
}

bool kvm_config_store_flush(kvm_config_store_t *store, uint64_t now_ms)
{
    if (!store) {
        return false;
    }
    if (!store->dirty) {
        return true;
    }
    if (store->hold_writes) {
        store->stats.holds++;
        return false;
    }
    return write_record(store, now_ms, true);
}
