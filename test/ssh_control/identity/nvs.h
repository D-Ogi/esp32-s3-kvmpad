// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint32_t nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_INVALID_LENGTH, NVS_READWRITE, NVS_READONLY };
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *name, void *buf, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *name, const void *buf, size_t len);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
