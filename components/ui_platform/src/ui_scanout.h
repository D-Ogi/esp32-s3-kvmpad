// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "ui_platform.h"

typedef struct ui_scanout ui_scanout_t;

// The LVGL owner copies one complete double-partial framebuffer between refreshes.
// ESP_ERR_NOT_FOUND means the adapter has not submitted a board framebuffer yet.
esp_err_t ui_scanout_take(uint16_t div, ui_scanout_t **out);
void ui_scanout_send(ui_scanout_t *shot, ui_platform_row_cb_t cb, void *ctx);
void ui_scanout_free(ui_scanout_t *shot);
