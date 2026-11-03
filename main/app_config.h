// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Firmware owner of the settings record (TASK-090 config_store): one store, one mutex, one low-priority writer
// task that runs the debounce. Also provides the ADR-017 cache hooks for net_status (TASK-080, ADR-015).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kvm_config.h"
#include "kvm_pairing_platform.h"
#include "net_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load the record (after nvs_flash_init) and start the writer task. Safe to call once. */
esp_err_t app_config_start(void);

/** BSSID/channel cache hooks backed by the settings record; all members NULL before app_config_start(). */
net_cache_hooks_t app_config_net_cache_hooks(void);

/** FR-734 / ADR-023: true when Close window (Alt+F4) must be confirmed (the shortcuts view reads it per tap). */
bool app_config_confirm_alt_f4(void);

/** Write a pending record now (reboot, OTA, mode change). True when nothing is pending afterwards. */
bool app_config_flush(void);

/** Console: load status, write counters and the non-secret settings. */
void app_config_print(void);

/** Remote policies read together (TECH-040 FR-506, ADR-023 FR-732/733). Safe defaults before start. */
typedef struct {
    bool remote_enabled;
    bool pair_require_accept;
    kvm_control_approval_t control_approval;
} app_config_remote_t;

void app_config_remote(app_config_remote_t *out);

/** FR-506 Settings -> Remote, from the local console or UI only (never from a CLIENT, SEC-107). */
bool app_config_set_remote_enabled(bool on);

/** ADR-024 FR-745: the enumerated HOST layout id (e.g. "us"), NUL-terminated; "us" before start. */
void app_config_host_layout(char *out, size_t cap);

/**
 * FR-741: store the HOST layout id Settings picked ("us", "de(neo)"). False when the store is not up or the id is
 * not well formed - the record is the judge of that, not the screen.
 */
bool app_config_set_layout(const char *id);

/**
 * ADR-039 sections 3, 4 and 9: pin or unpin `id`, and move `id` to the head of the recents. The two lists are
 * the record's, and this is where their algebra lives - the order, the removal of a duplicate, the 8 and 5
 * limits - so the screen only ever asks. `app_config_pin_layout` returns false when the id is malformed, when
 * it is not pinned and the eight slots are taken, or when unpinning it would empty the list.
 */
bool app_config_pin_layout(const char *id);
bool app_config_note_layout_use(const char *id);

/**
 * The paired-client register (FR-726) as the pairing core wants it: add / count / get / remove / remove_all,
 * written immediately by config_store. Only the token digest is stored (SEC-105).
 */
kvm_pair_store_t app_config_pair_store(void);

/** FR-733 `trusted`: is register entry `index` marked trusted? */
bool app_config_client_trusted(int index);

/** What the local Settings screen shows (TASK-070C). No token material: names and flags only. */
typedef struct {
    bool remote_enabled;
    bool pair_require_accept;
    uint8_t control_approval; // kvm_control_approval_t
    bool confirm_alt_f4;
    bool keepawake_enabled;
    uint16_t keepawake_interval_s;
    uint8_t unicode_mode; // kvm_unicode_mode_t
    bool lang_ko;         // ADR-037: the composed languages the language key offers
    bool lang_zh;
    bool lang_ja;
    char host_layout[KVM_CONFIG_LAYOUT_ID_CAP];
    char host_fav[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP]; // ADR-039: pinned, in the user's order
    uint8_t host_fav_count;
    char host_recent[KVM_CONFIG_HOST_RECENT_MAX][KVM_CONFIG_LAYOUT_ID_CAP]; // newest first
    uint8_t host_recent_count;
    uint8_t client_count;
    struct {
        char name[KVM_CONFIG_PAIR_NAME_CAP];
        bool trusted;
    } client[KVM_CONFIG_MAX_PAIRED];
} app_config_settings_t;

/** Changes whenever any setting or the paired register changed (lock-free; 0 before the record is loaded). */
uint32_t app_config_seq(void);

/**
 * Copy the settings. Waits at most `timeout_ms` for the record lock (the writer may be flushing to flash), so the
 * LVGL task can call it with a few milliseconds and simply retry on the next tick. False = not read.
 */
bool app_config_settings(app_config_settings_t *out, uint32_t timeout_ms);

/** Validated write of one scalar field (range and type from the config_store field table). */
bool app_config_set_field(kvm_config_field_t field, uint32_t value);

/** FR-733 `trusted` flag of register entry `index`. */
bool app_config_set_client_trusted(uint8_t index, bool trusted);

#ifdef __cplusplus
}
#endif
