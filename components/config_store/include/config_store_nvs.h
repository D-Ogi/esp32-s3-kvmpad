// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ESP-IDF binding of the settings store (TASK-090): the record lives in one NVS blob in this
// component's own namespace. The pure-C store in config_store.h holds the policy; this file only
// transfers the record between RAM and NVS.
#pragma once

#include "config_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// NVS namespace and key holding the settings record. The namespace is private to this component;
// nothing else may write it.
#define CONFIG_STORE_NVS_NAMESPACE "kvmpad_cfg"
#define CONFIG_STORE_NVS_KEY "settings"

// Bring up NVS and load the record into `store`.
//
// Returns an NVS mount error to the caller without formatting the partition.
// After a successful mount, inspect `store->load_status` for the record outcome.
// Pass NULL for `debounce` to use the default timings.
esp_err_t config_store_nvs_init(kvm_config_store_t *store, const kvm_config_debounce_t *debounce,
                                uint64_t now_ms);

// The backend used by config_store_nvs_init(); exposed for a store the caller wants to own itself.
const kvm_config_backend_t *config_store_nvs_backend(void);

// Versioned side record for optional AccessPad timing. Schema-4 firmware ignores this key.
esp_err_t config_store_nvs_access_read(uint64_t *out);
esp_err_t config_store_nvs_access_write(uint64_t word);
esp_err_t config_store_nvs_palette_read(uint8_t *value);
esp_err_t config_store_nvs_palette_write(uint8_t value);

// Monotonic millisecond clock for the store's now_ms arguments.
uint64_t config_store_nvs_now_ms(void);

// Log the load outcome: the status, the keys of the settings that had to be reset and how many
// pairing entries were dropped. Never logs a setting value, a client name or a token digest
// (SEC-105, AT-SEC-08).
void config_store_nvs_log_status(const kvm_config_store_t *store);

#ifdef __cplusplus
}
#endif
