// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Private contract between ui_platform.c and the one compiled backend (adapter or port).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t ui_backend_required_fbs(void);
const char *ui_backend_name(void);
const char *ui_backend_mode_name(void);

/** Init LVGL + backend, register `panel` (h x v RGB565) and start the LVGL task. */
esp_err_t ui_backend_start(esp_lcd_panel_handle_t panel, uint16_t h_res, uint16_t v_res, lv_display_t **out_disp);

bool ui_backend_lock(int32_t timeout_ms);
void ui_backend_unlock(void);

/** Backend's own FPS counter if it has one (0 = none). */
uint32_t ui_backend_fps(void);
void ui_backend_fps_reset(void);

#ifdef __cplusplus
}
#endif
