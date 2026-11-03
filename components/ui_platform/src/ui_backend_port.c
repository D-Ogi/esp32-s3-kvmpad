// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_platform backend B (ADR-013 controlled fallback): espressif/esp_lvgl_port 2.9.0. The port owns
// the LVGL task, the tick timer and the recursive lock; with KVM_UI_PORT_AVOID_TEARING the two RGB
// panel frame buffers are the LVGL buffers (direct mode), otherwise a partial DMA buffer is used.
#include "ui_backend.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "sdkconfig.h"

static const char *TAG = "ui_port";
static lv_display_t *s_disp;

uint8_t ui_backend_required_fbs(void)
{
    return CONFIG_KVM_UI_PORT_AVOID_TEARING ? 2 : 1;
}

const char *ui_backend_name(void)
{
    return "esp_lvgl_port 2.9.0";
}

const char *ui_backend_mode_name(void)
{
    return CONFIG_KVM_UI_PORT_AVOID_TEARING ? "port avoid_tearing (2 fbs, direct)" : "port partial (1 fb, DMA buffer)";
}

esp_err_t ui_backend_start(esp_lcd_panel_handle_t panel, uint16_t h_res, uint16_t v_res, lv_display_t **out_disp)
{
    ESP_RETURN_ON_FALSE(panel != NULL && out_disp != NULL, ESP_ERR_INVALID_ARG, TAG, "args");
    ESP_RETURN_ON_FALSE(s_disp == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.task_priority = CONFIG_KVM_UI_TASK_PRIORITY;
    cfg.task_stack = CONFIG_KVM_UI_TASK_STACK;
    cfg.task_affinity = CONFIG_KVM_UI_TASK_CORE;
    cfg.task_max_sleep_ms = 15;
    cfg.timer_period_ms = 2;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&cfg), TAG, "port init");

    const bool avoid = CONFIG_KVM_UI_PORT_AVOID_TEARING;
    lvgl_port_display_cfg_t dcfg = {
        .io_handle = NULL,
        .panel_handle = panel,
        .control_handle = NULL,
        .buffer_size = avoid ? (uint32_t)h_res * v_res : (uint32_t)h_res * CONFIG_KVM_UI_DRAW_BUF_LINES,
        .double_buffer = true,
        .trans_size = 0,
        .hres = h_res,
        .vres = v_res,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .rounder_cb = NULL,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = avoid ? 0 : 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 0,
            .full_refresh = 0,
            .direct_mode = avoid ? 1 : 0,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb = {
        .flags = {
            .bb_mode = 1, // board_lcd runs the panel with bounce buffers (TECH-020 variant A)
            .avoid_tearing = avoid ? 1 : 0,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&dcfg, &rgb);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "add display");
    ESP_LOGI(TAG, "%s: %s, task prio %d core %d", ui_backend_name(), ui_backend_mode_name(),
             CONFIG_KVM_UI_TASK_PRIORITY, CONFIG_KVM_UI_TASK_CORE);
    *out_disp = s_disp;
    return ESP_OK;
}

bool ui_backend_lock(int32_t timeout_ms)
{
    // In the port 0 means "wait forever"; keep our contract (< 0 forever, 0 = one try).
    return lvgl_port_lock(timeout_ms < 0 ? 0 : (timeout_ms == 0 ? 1 : (uint32_t)timeout_ms));
}

void ui_backend_unlock(void)
{
    lvgl_port_unlock();
}

uint32_t ui_backend_fps(void)
{
    return 0; // the port has no counter of its own; ui_platform measures from LVGL events
}

void ui_backend_fps_reset(void)
{
}
