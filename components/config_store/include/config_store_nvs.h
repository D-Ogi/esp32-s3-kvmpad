// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ESP-IDF binding of the settings store (TASK-090): the record lives in one NVS blob in this
// component's own namespace. The pure-C store in config_store.h holds the policy; this file only
// moves bytes and recovers an unusable NVS partition.
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
// `nvs_flash_init()` reporting ESP_ERR_NVS_NO_FREE_PAGES or ESP_ERR_NVS_NEW_VERSION_FOUND means the
// partition cannot be used as it stands: it is erased and initialized again, and the store comes up
// on the factory defaults (AT-FAULT-02/10). `debounce` may be NULL for the defaults.
//
// Returns ESP_OK when the store is usable — including every recovery path, because a device that
// lost its settings must still work on LOCAL USB. Only a failure to initialize NVS at all is
// reported as an error. Inspect `store->load_status`, `store->recovered` and
// `store->future_schema` for what actually happened.
esp_err_t config_store_nvs_init(kvm_config_store_t *store, const kvm_config_debounce_t *debounce,
                                uint64_t now_ms);

// The backend used by config_store_nvs_init(); exposed for a store the caller wants to own itself.
const kvm_config_backend_t *config_store_nvs_backend(void);

// Monotonic millisecond clock for the store's now_ms arguments.
uint64_t config_store_nvs_now_ms(void);

// Log the load outcome: the status, the keys of the settings that had to be reset and how many
// pairing entries were dropped. Never logs a setting value, a client name or a token digest
// (SEC-105, AT-SEC-08).
void config_store_nvs_log_status(const kvm_config_store_t *store);

#ifdef __cplusplus
}
#endif
