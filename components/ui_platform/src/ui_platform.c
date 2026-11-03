// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_platform (TASK-060A): backend-independent part — start sequence, lock wrappers, render
// metrics taken from LVGL display events (they run in the LVGL task) and the screen snapshot.
#include "ui_platform.h"

#include <string.h>

#include "board_lcd.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ui_backend.h"

static const char *TAG = "ui_platform";

static lv_display_t *s_disp;
static bool s_running;
static TaskHandle_t s_lvgl_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint64_t t_reset_us;
    uint64_t t_refr_start_us;
    uint64_t t_render_start_us;
    uint64_t t_flush_start_us;
    uint32_t refr_cycles, renders, flushes;
    uint32_t render_timed, flush_timed; // samples with a start stamp inside the window
    uint64_t render_sum_us, flush_sum_us;
    uint32_t render_max_us, flush_max_us, cycle_max_us;
} metrics_t;
static metrics_t s_m;

static void on_display_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (s_lvgl_task == NULL) {
        s_lvgl_task = xTaskGetCurrentTaskHandle();
    }
    portENTER_CRITICAL(&s_mux);
    switch (code) {
    case LV_EVENT_REFR_START:
        s_m.t_refr_start_us = now;
        break;
    case LV_EVENT_REFR_READY: {
        s_m.refr_cycles++;
        if (s_m.t_refr_start_us != 0) { // 0 = the window was reset during this cycle: no sample
            uint32_t d = (uint32_t)(now - s_m.t_refr_start_us);
            if (d > s_m.cycle_max_us) {
                s_m.cycle_max_us = d;
            }
        }
        break;
    }
    case LV_EVENT_RENDER_START:
        s_m.t_render_start_us = now;
        break;
    case LV_EVENT_RENDER_READY: {
        s_m.renders++;
        if (s_m.t_render_start_us != 0) {
            uint32_t d = (uint32_t)(now - s_m.t_render_start_us);
            s_m.render_timed++;
            s_m.render_sum_us += d;
            if (d > s_m.render_max_us) {
                s_m.render_max_us = d;
            }
        }
        break;
    }
    case LV_EVENT_FLUSH_START:
        s_m.t_flush_start_us = now;
        break;
    case LV_EVENT_FLUSH_FINISH: {
        s_m.flushes++;
        if (s_m.t_flush_start_us != 0) {
            uint32_t d = (uint32_t)(now - s_m.t_flush_start_us);
            s_m.flush_timed++;
            s_m.flush_sum_us += d;
            if (d > s_m.flush_max_us) {
                s_m.flush_max_us = d;
            }
        }
        break;
    }
    default:
        break;
    }
    portEXIT_CRITICAL(&s_mux);
}

uint8_t ui_platform_required_fbs(void)
{
    return ui_backend_required_fbs();
}

uint16_t ui_platform_bounce_lines(void)
{
    return 10; // TECH-020 variant A, measured in M1 (board_lcd default)
}

esp_err_t ui_platform_start(void)
{
    ESP_RETURN_ON_FALSE(!s_running, ESP_ERR_INVALID_STATE, TAG, "already running");
    esp_lcd_panel_handle_t panel = board_lcd_panel();
    ESP_RETURN_ON_FALSE(panel != NULL, ESP_ERR_INVALID_STATE, TAG, "board_lcd_init first");
    const uint8_t need = ui_backend_required_fbs();
    ESP_RETURN_ON_FALSE(board_lcd_fb(need - 1) != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "board_lcd has fewer than %u frame buffers", need);

    ESP_RETURN_ON_ERROR(ui_backend_start(panel, BOARD_LCD_H_RES, BOARD_LCD_V_RES, &s_disp), TAG, "backend");

    const lv_event_code_t codes[] = {LV_EVENT_REFR_START,   LV_EVENT_REFR_READY,  LV_EVENT_RENDER_START,
                                     LV_EVENT_RENDER_READY, LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH};
    if (ui_backend_lock(-1)) {
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
            lv_display_add_event_cb(s_disp, on_display_event, codes[i], NULL);
        }
        ui_backend_unlock();
    }
    ui_platform_metrics_reset();
    s_running = true;
    ESP_LOGI(TAG, "LVGL %d.%d.%d on %s (%s), %dx%d, %u panel fbs, internal free %u, psram free %u", LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, ui_backend_name(), ui_backend_mode_name(), BOARD_LCD_H_RES,
             BOARD_LCD_V_RES, need, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

bool ui_platform_is_running(void)
{
    return s_running;
}

bool ui_platform_lock(int32_t timeout_ms)
{
    return s_running && ui_backend_lock(timeout_ms);
}

void ui_platform_unlock(void)
{
    ui_backend_unlock();
}

lv_display_t *ui_platform_display(void)
{
    return s_disp;
}

const char *ui_platform_backend_name(void)
{
    return ui_backend_name();
}

const char *ui_platform_mode_name(void)
{
    return ui_backend_mode_name();
}

void ui_platform_metrics_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_m, 0, sizeof(s_m));
    s_m.t_reset_us = (uint64_t)esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
    ui_backend_fps_reset();
}

void ui_platform_metrics_get(ui_platform_metrics_t *out)
{
    if (out == NULL) {
        return;
    }
    metrics_t m;
    portENTER_CRITICAL(&s_mux);
    m = s_m;
    portEXIT_CRITICAL(&s_mux);
    memset(out, 0, sizeof(*out));
    out->refr_cycles = m.refr_cycles;
    out->renders = m.renders;
    out->flushes = m.flushes;
    out->window_us = (uint64_t)esp_timer_get_time() - m.t_reset_us;
    out->render_avg_us = m.render_timed ? (uint32_t)(m.render_sum_us / m.render_timed) : 0;
    out->render_max_us = m.render_max_us;
    out->flush_avg_us = m.flush_timed ? (uint32_t)(m.flush_sum_us / m.flush_timed) : 0;
    out->flush_max_us = m.flush_max_us;
    out->cycle_max_us = m.cycle_max_us;
    out->adapter_fps = ui_backend_fps();
    out->lvgl_task_stack_hwm = s_lvgl_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_lvgl_task) * sizeof(StackType_t) : 0;
}

uint32_t ui_platform_fps_x100(void)
{
    ui_platform_metrics_t m;
    ui_platform_metrics_get(&m);
    if (m.window_us == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)m.renders * 100000000ULL) / m.window_us);
}

#if LV_USE_SNAPSHOT
// The snapshot renders the whole screen. It runs in the LVGL task, whose stack is sized for rendering
// (CONFIG_KVM_UI_TASK_STACK), never in the caller's task: a full shell render overflowed the 6 KiB console stack.
typedef struct {
    uint16_t *small;
    uint16_t w, h, div;
    esp_err_t ret;
    SemaphoreHandle_t done;
    bool finished;  // set by the LVGL task, under s_mux
    bool abandoned; // set by a caller that timed out, under s_mux: the LVGL task frees the request
} snap_req_t;

static void snap_free(snap_req_t *r)
{
    vSemaphoreDelete(r->done);
    heap_caps_free(r->small);
    heap_caps_free(r);
}

static void snap_in_lvgl_task(void *arg)
{
    snap_req_t *r = arg;
    lv_draw_buf_t *shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (shot == NULL) {
        r->ret = ESP_ERR_NO_MEM;
    } else {
        const uint32_t stride = shot->header.stride;
        const uint8_t *base = shot->data;
        for (uint16_t y = 0; y < r->h; y++) {
            const uint16_t *src = (const uint16_t *)(base + (size_t)y * r->div * stride);
            uint16_t *dst = r->small + (size_t)y * r->w;
            for (uint16_t x = 0; x < r->w; x++) {
                dst[x] = src[(size_t)x * r->div];
            }
        }
        lv_draw_buf_destroy(shot);
        r->ret = ESP_OK;
    }
    portENTER_CRITICAL(&s_mux);
    r->finished = true;
    const bool abandoned = r->abandoned;
    portEXIT_CRITICAL(&s_mux);
    if (abandoned) {
        snap_free(r);
    } else {
        xSemaphoreGive(r->done);
    }
}
#endif

esp_err_t ui_platform_snapshot(uint16_t div, ui_platform_row_cb_t cb, void *ctx, uint16_t *out_w, uint16_t *out_h)
{
    ESP_RETURN_ON_FALSE(s_running && cb != NULL && div >= 1 && div <= 16, ESP_ERR_INVALID_ARG, TAG, "args");
#if LV_USE_SNAPSHOT
    snap_req_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(r != NULL, ESP_ERR_NO_MEM, TAG, "snapshot request");
    r->div = div;
    r->w = (uint16_t)(BOARD_LCD_H_RES / div);
    r->h = (uint16_t)(BOARD_LCD_V_RES / div);
    r->ret = ESP_FAIL;
    r->small = heap_caps_malloc((size_t)r->w * r->h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->done = xSemaphoreCreateBinary();
    if (r->small == NULL || r->done == NULL) {
        if (r->done != NULL) {
            vSemaphoreDelete(r->done);
        }
        heap_caps_free(r->small);
        heap_caps_free(r);
        return ESP_ERR_NO_MEM;
    }

    if (!ui_backend_lock(2000)) {
        snap_free(r);
        return ESP_ERR_TIMEOUT;
    }
    const lv_result_t queued = lv_async_call(snap_in_lvgl_task, r);
    ui_backend_unlock();
    if (queued != LV_RESULT_OK) {
        snap_free(r);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(r->done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        const bool finished = r->finished;
        if (!finished) {
            r->abandoned = true; // the LVGL task frees it when the render ends
        }
        portEXIT_CRITICAL(&s_mux);
        if (!finished) {
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(r->done, 0); // finished between the timeout and the check
    }
    const esp_err_t ret = r->ret;
    if (ret == ESP_OK) {
        for (uint16_t y = 0; y < r->h; y++) {
            cb(r->small + (size_t)y * r->w, r->w, y, ctx);
        }
        if (out_w) {
            *out_w = r->w;
        }
        if (out_h) {
            *out_h = r->h;
        }
    }
    snap_free(r);
    return ret;
#else
    (void)ctx;
    (void)out_w;
    (void)out_h;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
