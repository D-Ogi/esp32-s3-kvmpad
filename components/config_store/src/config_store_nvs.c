// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// NVS backend of the settings store (TASK-090, TECH-050). The whole record is one blob under
// CONFIG_STORE_NVS_KEY in CONFIG_STORE_NVS_NAMESPACE: NVS replaces a blob by writing the new value
// and only then moving the index entry, so a power cut during a write leaves the previous record
// readable, which is what the store's retry logic assumes (AT-FAULT-08).
//
// Nothing here parses the record and nothing here logs its content.

#include "config_store_nvs.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config_store";

static kvm_config_io_t backend_read(void *ctx, void *buf, size_t cap, size_t *out_len)
{
    (void)ctx;
    if (!buf || !out_len) {
        return KVM_CONFIG_IO_ERROR;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_STORE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return KVM_CONFIG_IO_ABSENT; // first boot: the namespace does not exist yet
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(read) failed: %s", esp_err_to_name(err));
        return KVM_CONFIG_IO_ERROR;
    }

    size_t len = cap;
    err = nvs_get_blob(handle, CONFIG_STORE_NVS_KEY, buf, &len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return KVM_CONFIG_IO_ABSENT;
    }
    if (err != ESP_OK) {
        // ESP_ERR_NVS_INVALID_LENGTH means the stored blob is larger than any record this
        // firmware can produce, i.e. corrupt: the store falls back to the defaults.
        ESP_LOGW(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return KVM_CONFIG_IO_ERROR;
    }
    *out_len = len;
    return KVM_CONFIG_IO_OK;
}

static kvm_config_io_t backend_write(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (!buf || len == 0u) {
        return KVM_CONFIG_IO_ERROR;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_STORE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(write) failed: %s", esp_err_to_name(err));
        return KVM_CONFIG_IO_ERROR;
    }

    err = nvs_set_blob(handle, CONFIG_STORE_NVS_KEY, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings write failed: %s", esp_err_to_name(err));
        return KVM_CONFIG_IO_ERROR;
    }
    return KVM_CONFIG_IO_OK;
}

static kvm_config_io_t backend_erase(void *ctx)
{
    (void)ctx;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_STORE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return KVM_CONFIG_IO_ABSENT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(erase) failed: %s", esp_err_to_name(err));
        return KVM_CONFIG_IO_ERROR;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return KVM_CONFIG_IO_ABSENT;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings erase failed: %s", esp_err_to_name(err));
        return KVM_CONFIG_IO_ERROR;
    }
    return KVM_CONFIG_IO_OK;
}

static const kvm_config_backend_ops_t s_ops = {
    .read = backend_read,
    .write = backend_write,
    .erase = backend_erase,
};

static const kvm_config_backend_t s_backend = {
    .ops = &s_ops,
    .ctx = NULL,
};

const kvm_config_backend_t *config_store_nvs_backend(void)
{
    return &s_backend;
}

uint64_t config_store_nvs_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

esp_err_t config_store_nvs_init(kvm_config_store_t *store, const kvm_config_debounce_t *debounce,
                                uint64_t now_ms)
{
    if (!store) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The partition is full of stale pages or was written by a newer NVS format. Neither can
        // be read; erasing it is the only way back and costs the settings, not the firmware.
        ESP_LOGW(TAG, "NVS unusable (%s), erasing the partition", esp_err_to_name(err));
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(erase_err));
            return erase_err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    (void)kvm_config_store_init(store, config_store_nvs_backend(), debounce, now_ms);
    config_store_nvs_log_status(store);
    return ESP_OK;
}

void config_store_nvs_log_status(const kvm_config_store_t *store)
{
    if (!store) {
        return;
    }
    ESP_LOGI(TAG, "settings load: %s", kvm_config_load_status_str(store->load_status));

    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        if ((store->load_repair.field_mask & ((uint32_t)1u << f)) != 0u) {
            ESP_LOGW(TAG, "setting reset to default: %s",
                     kvm_config_field_key((kvm_config_field_t)f));
        }
    }
    if (store->load_repair.layout_repaired) {
        ESP_LOGW(TAG, "setting reset to default: kbd.host_layout");
    }
    if (store->load_repair.wifi_repaired) {
        ESP_LOGW(TAG, "wifi fast-reconnect cache dropped");
    }
    if (store->load_repair.host_fav_repaired || store->load_repair.host_recent_repaired) {
        // Counts only: an id is not a secret, but the log stays free of record content.
        ESP_LOGW(TAG, "host layout list entries dropped: %u",
                 (unsigned)store->load_repair.host_ids_dropped);
    }
    if (store->load_repair.pairs_dropped != 0u) {
        ESP_LOGW(TAG, "paired clients dropped: %u", (unsigned)store->load_repair.pairs_dropped);
    }
    if (store->future_schema) {
        ESP_LOGW(TAG, "stored record is newer than this firmware; running on defaults");
    }
}
