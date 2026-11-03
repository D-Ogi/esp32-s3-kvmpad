// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Settings store (TASK-090, TECH-050): the in-RAM copy of the record from kvm_config.h plus the
// write policy that keeps flash alive. Pure C; the storage itself is a three-call backend so the
// firmware plugs in NVS (src/config_store_nvs.c) and the host tests plug in a fake.
//
// Write policy (TECH-050 "write debounce; do not write flash on every tap"):
//   * a settings change only marks the record dirty and is coalesced with every other change;
//   * kvm_config_store_tick() writes once the record has been quiet for `quiet_ms`, or at the
//     latest `max_defer_ms` after the first pending change, and never sooner than
//     `min_interval_ms` after the previous write attempt;
//   * pairing changes and the factory reset bypass the debounce, because a revoke has to take
//     effect immediately (FR-726) and must survive an instant power cut;
//   * a failed write keeps the record dirty and is retried on a later tick, no faster than
//     `min_interval_ms`.
//
// Recovery (AT-FAULT-02/03/10):
//   * a record that fails the magic, length or CRC check is discarded; the store comes up on the
//     factory defaults with `recovered` set, and the firmware keeps working on LOCAL USB;
//   * a record written by newer firmware is not parsed and not overwritten: the store runs on
//     defaults with `future_schema` set and holds every write back until the user changes a
//     setting, so downgrading a device does not destroy its configuration;
//   * a factory reset erases the backing store, clears the pairing register and the Wi-Fi cache
//     and writes the defaults back immediately.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Result of one backend call. ABSENT is the first boot, not an error.
typedef enum {
    KVM_CONFIG_IO_OK = 0,
    KVM_CONFIG_IO_ABSENT,
    KVM_CONFIG_IO_ERROR
} kvm_config_io_t;

// Storage backend. `read` fills at most `cap` bytes and reports the record length; it must leave
// `*out_len` untouched when it does not return OK. `write` replaces the record atomically from the
// reader's point of view: an interrupted write leaves the previous record readable. `erase` removes
// the record (and anything else in the backend's own namespace).
typedef struct {
    kvm_config_io_t (*read)(void *ctx, void *buf, size_t cap, size_t *out_len);
    kvm_config_io_t (*write)(void *ctx, const void *buf, size_t len);
    kvm_config_io_t (*erase)(void *ctx);
} kvm_config_backend_ops_t;

typedef struct {
    const kvm_config_backend_ops_t *ops;
    void *ctx;
} kvm_config_backend_t;

// Debounce tuning, all in milliseconds.
typedef struct {
    uint32_t quiet_ms;        // settle time after the last change
    uint32_t max_defer_ms;    // upper bound on holding a pending change back
    uint32_t min_interval_ms; // minimum distance between two write attempts
} kvm_config_debounce_t;

// quiet 1500 ms, max_defer 10000 ms, min_interval 2000 ms: a slider dragged for ten seconds costs
// at most two writes, and a single tap costs one, 1.5 s after the user stops.
void kvm_config_debounce_defaults(kvm_config_debounce_t *out);

typedef struct {
    uint32_t changes;          // accepted mutations
    uint32_t coalesced;        // mutations that joined an already pending write
    uint32_t writes;           // successful backend writes
    uint32_t write_failures;   // backend write errors
    uint32_t immediate_writes; // writes that bypassed the debounce
    uint32_t erases;           // factory resets
    uint32_t holds;            // writes suppressed because of a future schema
} kvm_config_store_stats_t;

typedef struct {
    kvm_config_t cfg;
    kvm_config_backend_t backend;
    kvm_config_debounce_t debounce;

    kvm_config_load_status_t load_status;
    kvm_config_repair_t load_repair;
    bool recovered;     // the stored record was unusable and was replaced by defaults
    bool future_schema; // the stored record came from newer firmware
    bool hold_writes;   // do not touch the stored record until the user changes a setting

    bool dirty;
    uint64_t dirty_since_ms;
    uint64_t last_change_ms;
    uint64_t last_write_ms;
    bool wrote_once;

    kvm_config_store_stats_t stats;
} kvm_config_store_t;

// Load the record through `backend` and set up the write policy. `debounce` may be NULL for the
// defaults. Returns the load status; the store always ends up with a usable record.
kvm_config_load_status_t kvm_config_store_init(kvm_config_store_t *store,
                                              const kvm_config_backend_t *backend,
                                              const kvm_config_debounce_t *debounce,
                                              uint64_t now_ms);

// The live record. Never NULL for an initialized store.
const kvm_config_t *kvm_config_store_get(const kvm_config_store_t *store);

// --- mutations (all of them mark the record dirty only when the value actually changes) ---

// Set one scalar setting. False when the value is outside the field's valid range.
bool kvm_config_store_set_field(kvm_config_store_t *store, kvm_config_field_t field, uint32_t value,
                                uint64_t now_ms);

// Set the keyboard layout id (FR-741). False when the id is not well formed.
bool kvm_config_store_set_layout(kvm_config_store_t *store, const char *id, uint64_t now_ms);

// Replace the favorites / recents list of the HOST-layout picker (ADR-039 FR-741). The whole list
// is written in one step, so a half-applied list never reaches flash; false when an entry is not a
// well-formed id or the count does not fit the list, and then the record is left unchanged. The UI
// owns the ordering, the removal of a duplicate and the truncation; the store validates and stores.
bool kvm_config_store_set_favorites(kvm_config_store_t *store,
                                    const char ids[][KVM_CONFIG_LAYOUT_ID_CAP], uint8_t count,
                                    uint64_t now_ms);
bool kvm_config_store_set_recents(kvm_config_store_t *store,
                                  const char ids[][KVM_CONFIG_LAYOUT_ID_CAP], uint8_t count,
                                  uint64_t now_ms);

// One step of the picker's two lists (ADR-039): pin or unpin `id`, and put `id` at the head of the
// recents. The ordering, the dedup and the two limits are kvm_config_pin_toggle() /
// kvm_config_recent_push(); false leaves the record exactly as it was.
bool kvm_config_store_pin_toggle(kvm_config_store_t *store, const char *id, uint64_t now_ms);
bool kvm_config_store_note_layout_use(kvm_config_store_t *store, const char *id, uint64_t now_ms);

// Replace the whole record, e.g. from a Settings screen that edited a working copy. Out-of-range
// values are repaired to their defaults rather than rejected; `out_repair` may be NULL.
bool kvm_config_store_apply(kvm_config_store_t *store, const kvm_config_t *next, uint64_t now_ms,
                            kvm_config_repair_t *out_repair);

// Store or drop the Wi-Fi fast-reconnect hint (TECH-071). No secret is accepted here.
bool kvm_config_store_set_wifi_cache(kvm_config_store_t *store, const uint8_t bssid[6],
                                     uint8_t channel, uint64_t now_ms);
void kvm_config_store_clear_wifi_cache(kvm_config_store_t *store, uint64_t now_ms);

// --- pairing register (written immediately, FR-726) ---

int kvm_config_store_pair_add(kvm_config_store_t *store, const char *name,
                              const uint8_t token_sha[KVM_CONFIG_TOKEN_SHA_LEN],
                              uint64_t paired_at_unix, bool trusted, uint64_t now_ms);
bool kvm_config_store_pair_set_trusted(kvm_config_store_t *store, uint8_t index, bool trusted,
                                       uint64_t now_ms);
bool kvm_config_store_pair_remove(kvm_config_store_t *store, uint8_t index, uint64_t now_ms);
uint8_t kvm_config_store_pair_remove_all(kvm_config_store_t *store, uint64_t now_ms);

// Factory reset (PROC-010, FR-736, AT-FAULT-10): erase the backing store, return every setting to
// its default, clear the pairing register and the Wi-Fi cache, then write the defaults back.
// Returns true when the new record reached the backend.
bool kvm_config_store_factory_reset(kvm_config_store_t *store, uint64_t now_ms);

// --- write policy ---

// Call from the housekeeping loop. Writes at most one record and returns true when it did.
bool kvm_config_store_tick(kvm_config_store_t *store, uint64_t now_ms);

// Write a pending record now, ignoring the debounce (shutdown, OTA, mode change). Returns true
// when a write succeeded; true as well when there was nothing to write.
bool kvm_config_store_flush(kvm_config_store_t *store, uint64_t now_ms);

bool kvm_config_store_is_dirty(const kvm_config_store_t *store);

// Milliseconds until kvm_config_store_tick() would write, 0 when it would write now, and
// UINT32_MAX when nothing is pending. Lets the housekeeping task pick its sleep.
uint32_t kvm_config_store_ms_until_write(const kvm_config_store_t *store, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
