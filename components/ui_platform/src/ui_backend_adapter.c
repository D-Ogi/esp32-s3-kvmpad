// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_platform backend A (ADR-013 candidate): espressif/esp_lvgl_adapter 0.7.4. The adapter owns the
// LVGL task, the tick and the recursive lock; the RGB panel frame buffers come from board_lcd.
#include "ui_backend.h"

#include "esp_check.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "sdkconfig.h"

static const char *TAG = "ui_adapter";
static lv_display_t *s_disp;

#if CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_DIRECT
#define TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT
#define TEAR_NAME "adapter DOUBLE_DIRECT (2 fbs, direct)"
#elif CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL
#define TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL
#define TEAR_NAME "adapter DOUBLE_PARTIAL (2 fbs, partial)"
#elif CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_FULL
#define TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL
#define TEAR_NAME "adapter DOUBLE_FULL (2 fbs, full)"
#elif CONFIG_KVM_UI_ADAPTER_TEAR_NONE
#define TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE
#define TEAR_NAME "adapter NONE (1 fb, partial)"
#else
#define TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL
#define TEAR_NAME "adapter TRIPLE_PARTIAL (3 fbs, partial)"
#endif

uint8_t ui_backend_required_fbs(void)
{
    return esp_lv_adapter_get_required_frame_buffer_count(TEAR_MODE, ESP_LV_ADAPTER_ROTATE_0);
}

const char *ui_backend_name(void)
{
    return "esp_lvgl_adapter 0.7.4";
}

const char *ui_backend_mode_name(void)
{
    return TEAR_NAME;
}

esp_err_t ui_backend_start(esp_lcd_panel_handle_t panel, uint16_t h_res, uint16_t v_res, lv_display_t **out_disp)
{
    ESP_RETURN_ON_FALSE(panel != NULL && out_disp != NULL, ESP_ERR_INVALID_ARG, TAG, "args");
    ESP_RETURN_ON_FALSE(s_disp == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_stack_size = CONFIG_KVM_UI_TASK_STACK;
    cfg.task_priority = CONFIG_KVM_UI_TASK_PRIORITY;
    cfg.task_core_id = CONFIG_KVM_UI_TASK_CORE;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&cfg), TAG, "adapter init");

    esp_lv_adapter_display_config_t dcfg =
        ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(panel, NULL, h_res, v_res, ESP_LV_ADAPTER_ROTATE_0);
    dcfg.tear_avoid_mode = TEAR_MODE;
    dcfg.profile.buffer_height = CONFIG_KVM_UI_DRAW_BUF_LINES;
    s_disp = esp_lv_adapter_register_display(&dcfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "register display");

    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "adapter start");
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    esp_lv_adapter_fps_stats_enable(s_disp, true);
#endif
    ESP_LOGI(TAG, "%s: %s, draw buffer %d lines, task prio %d core %d", ui_backend_name(), TEAR_NAME,
             CONFIG_KVM_UI_DRAW_BUF_LINES, CONFIG_KVM_UI_TASK_PRIORITY, CONFIG_KVM_UI_TASK_CORE);
    *out_disp = s_disp;
    return ESP_OK;
}

bool ui_backend_lock(int32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void ui_backend_unlock(void)
{
    esp_lv_adapter_unlock();
}

uint32_t ui_backend_fps(void)
{
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    uint32_t fps = 0;
    if (s_disp != NULL && esp_lv_adapter_get_fps(s_disp, &fps) == ESP_OK) {
        return fps;
    }
#endif
    return 0;
}

void ui_backend_fps_reset(void)
{
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    if (s_disp != NULL) {
        esp_lv_adapter_fps_stats_reset(s_disp);
    }
#endif
}
