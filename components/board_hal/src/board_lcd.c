// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "board_lcd.h"

#include <string.h>

#include "board_hal.h"
#include "board_pins.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/periph_ctrl.h" // PERIPH_RCC_ATOMIC for the LCD_CAM reset on the restart path
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/lcd_ll.h"
#include "soc/lcd_cam_struct.h"

static const char *TAG = "board_lcd";

#define H_TOTAL (BOARD_LCD_H_RES + BOARD_LCD_HSYNC_PULSE + BOARD_LCD_HSYNC_BP + BOARD_LCD_HSYNC_FP)
#define V_TOTAL (BOARD_LCD_V_RES + BOARD_LCD_VSYNC_PULSE + BOARD_LCD_VSYNC_BP + BOARD_LCD_VSYNC_FP)
#define NOMINAL_FRAME_US ((uint32_t)(((uint64_t)H_TOTAL * V_TOTAL * 1000000ULL) / BOARD_LCD_PCLK_HZ))

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[3];
static board_lcd_cfg_t s_cfg;

// Written from the LCD ISR (IRAM-safe: CONFIG_LCD_RGB_ISR_IRAM_SAFE), read with the spinlock.
static DRAM_ATTR portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR volatile int64_t s_last_vsync_us;
static DRAM_ATTR board_lcd_stats_t s_stats;

uint32_t board_lcd_nominal_mhz(void)
{
    return (uint32_t)(((uint64_t)BOARD_LCD_PCLK_HZ * 1000ULL) / ((uint64_t)H_TOTAL * V_TOTAL));
}

// ---- Callback chaining ---------------------------------------------------------------------------
// The driver keeps one callback set per panel and every registration replaces all of it, so the LVGL adapter's
// registration (TASK-060A) would silently switch off the telemetry below. The link option in CMakeLists.txt routes
// every esp_lcd_rgb_panel_register_event_callbacks() call for our panel through __wrap_…: the driver always holds
// the board_lcd trampolines, which count and then forward to the callbacks the other registrant asked for.
esp_err_t __real_esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel,
                                                            const esp_lcd_rgb_panel_event_callbacks_t *callbacks,
                                                            void *user_ctx);

static DRAM_ATTR esp_lcd_rgb_panel_event_callbacks_t s_chain; // written under s_mux, read under s_mux in the ISR
static DRAM_ATTR void *s_chain_ctx;

static IRAM_ATTR bool on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
                               void *user_ctx)
{
    (void)user_ctx;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_mux);
    esp_lcd_rgb_panel_vsync_cb_t next = s_chain.on_vsync;
    void *next_ctx = s_chain_ctx;
    if (s_last_vsync_us != 0) {
        uint32_t dt = (uint32_t)(now - s_last_vsync_us);
        if (dt < s_stats.vsync_min_us) {
            s_stats.vsync_min_us = dt;
        }
        if (dt > s_stats.vsync_max_us) {
            s_stats.vsync_max_us = dt;
        }
        s_stats.vsync_sum_us += dt;
        if (dt > (NOMINAL_FRAME_US + NOMINAL_FRAME_US / 2)) {
            s_stats.vsync_late++;
        }
    }
    s_last_vsync_us = now;
    s_stats.vsync_count++;
    portEXIT_CRITICAL_ISR(&s_mux);
    return next != NULL ? next(panel, edata, next_ctx) : false;
}

static IRAM_ATTR bool on_frame_done(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
                                    void *user_ctx)
{
    (void)user_ctx;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_mux);
    esp_lcd_rgb_panel_frame_buf_complete_cb_t next = s_chain.on_frame_buf_complete;
    void *next_ctx = s_chain_ctx;
    s_stats.bounce_frames++;
    if (s_last_vsync_us != 0) {
        uint32_t ph = (uint32_t)(now - s_last_vsync_us);
        s_stats.phase_last_us = ph;
        if (ph < s_stats.phase_min_us) {
            s_stats.phase_min_us = ph;
        }
        if (ph > s_stats.phase_max_us) {
            s_stats.phase_max_us = ph;
        }
    }
    portEXIT_CRITICAL_ISR(&s_mux);
    return next != NULL ? next(panel, edata, next_ctx) : false;
}

static IRAM_ATTR bool on_trans_done(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
                                    void *user_ctx)
{
    (void)user_ctx;
    portENTER_CRITICAL_ISR(&s_mux);
    esp_lcd_rgb_panel_draw_buf_complete_cb_t next = s_chain.on_color_trans_done;
    void *next_ctx = s_chain_ctx;
    portEXIT_CRITICAL_ISR(&s_mux);
    return next != NULL ? next(panel, edata, next_ctx) : false;
}

static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t panel, void *bounce_buf, int pos_px, int len_bytes,
                                      void *user_ctx)
{
    (void)user_ctx;
    portENTER_CRITICAL_ISR(&s_mux);
    esp_lcd_rgb_panel_bounce_buf_fill_cb_t next = s_chain.on_bounce_empty;
    void *next_ctx = s_chain_ctx;
    portEXIT_CRITICAL_ISR(&s_mux);
    return next != NULL ? next(panel, bounce_buf, pos_px, len_bytes, next_ctx) : false;
}

// Hands the driver the trampolines. on_color_trans_done and on_bounce_empty are installed only when the chained set
// has them: a non-NULL on_bounce_empty replaces the driver's own frame-buffer copy into the bounce buffers.
static esp_err_t board_lcd_install_trampolines(esp_lcd_panel_handle_t panel)
{
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync,
        .on_frame_buf_complete = on_frame_done,
        .on_color_trans_done = s_chain.on_color_trans_done != NULL ? on_trans_done : NULL,
        .on_bounce_empty = s_chain.on_bounce_empty != NULL ? on_bounce_empty : NULL,
    };
    return __real_esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL);
}

esp_err_t __wrap_esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel,
                                                            const esp_lcd_rgb_panel_event_callbacks_t *callbacks,
                                                            void *user_ctx)
{
    if (panel == NULL || panel != s_panel || callbacks == NULL) {
        return __real_esp_lcd_rgb_panel_register_event_callbacks(panel, callbacks, user_ctx);
    }
#if CONFIG_LCD_RGB_ISR_IRAM_SAFE
    // The driver checks only what it is given (the trampolines); apply its rules to the chained set here.
    const void *fns[] = {(const void *)callbacks->on_vsync, (const void *)callbacks->on_color_trans_done,
                         (const void *)callbacks->on_frame_buf_complete, (const void *)callbacks->on_bounce_empty};
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        ESP_RETURN_ON_FALSE(fns[i] == NULL || esp_ptr_in_iram(fns[i]), ESP_ERR_INVALID_ARG, TAG,
                            "chained callback %u not in IRAM", (unsigned)i);
    }
    ESP_RETURN_ON_FALSE(user_ctx == NULL || esp_ptr_internal(user_ctx), ESP_ERR_INVALID_ARG, TAG,
                        "chained user context not in internal RAM");
#endif
    portENTER_CRITICAL(&s_mux);
    s_chain = *callbacks;
    s_chain_ctx = user_ctx;
    portEXIT_CRITICAL(&s_mux);
    return board_lcd_install_trampolines(panel);
}

void board_lcd_reset_stats(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.cfg = s_cfg;
    s_stats.vsync_min_us = UINT32_MAX;
    s_stats.phase_min_us = UINT32_MAX;
    s_last_vsync_us = 0;
    portEXIT_CRITICAL(&s_mux);
}

void board_lcd_get_stats(board_lcd_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(out, &s_stats, sizeof(*out));
    portEXIT_CRITICAL(&s_mux);
}

// esp_restart() shutdown handler: stop the RGB scan-out and reset LCD_CAM before esp_restart_noos. That function
// resets GDMA but not LCD_CAM; with the panel still scanning, a requested restart on board #1 often ended through
// the RTC watchdog backstop (rst:0x10, about 2 s late) instead of a CPU reset. Register access only: no lock, no
// heap, safe in the restart path. Panic and watchdog resets skip shutdown handlers and are not covered.
static void board_lcd_shutdown(void)
{
    lcd_ll_stop(&LCD_CAM);
    // The lcd_ll_reset_register() macro demands the reference-counted RCC section, which would also change the
    // module's reference count; the parentheses call the inline function itself, under the plain RCC lock that
    // guards the shared SYSTEM reset register.
    PERIPH_RCC_ATOMIC() {
        (lcd_ll_reset_register)(0);
    }
}

esp_err_t board_lcd_init(const board_lcd_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && cfg->num_fbs >= 1 && cfg->num_fbs <= 3, ESP_ERR_INVALID_ARG, TAG,
                        "num_fbs must be 1..3");
    ESP_RETURN_ON_FALSE(s_panel == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    ESP_RETURN_ON_ERROR(board_hal_lcd_power_reset(), TAG, "LCD reset");

    esp_lcd_rgb_panel_config_t pc = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BOARD_LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_LCD_HSYNC_PULSE,
            .hsync_back_porch = BOARD_LCD_HSYNC_BP,
            .hsync_front_porch = BOARD_LCD_HSYNC_FP,
            .vsync_pulse_width = BOARD_LCD_VSYNC_PULSE,
            .vsync_back_porch = BOARD_LCD_VSYNC_BP,
            .vsync_front_porch = BOARD_LCD_VSYNC_FP,
            .flags.pclk_active_neg = BOARD_LCD_PCLK_ACTIVE_NEG,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = cfg->num_fbs,
        .bounce_buffer_size_px = (size_t)BOARD_LCD_H_RES * cfg->bounce_lines,
        .dma_burst_size = 64,
        .hsync_gpio_num = BOARD_LCD_HSYNC_GPIO,
        .vsync_gpio_num = BOARD_LCD_VSYNC_GPIO,
        .de_gpio_num = BOARD_LCD_DE_GPIO,
        .pclk_gpio_num = BOARD_LCD_PCLK_GPIO,
        .disp_gpio_num = BOARD_LCD_DISP_GPIO,
        .data_gpio_nums = BOARD_LCD_DATA_GPIOS,
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&pc, &s_panel), TAG, "new rgb panel");

    portENTER_CRITICAL(&s_mux);
    memset(&s_chain, 0, sizeof(s_chain)); // nothing chained until another registrant (the LVGL adapter) appears
    s_chain_ctx = NULL;
    portEXIT_CRITICAL(&s_mux);
    esp_err_t err = board_lcd_install_trampolines(s_panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(s_panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(s_panel);
    }
    if (err == ESP_OK) {
        void *fb0 = NULL;
        void *fb1 = NULL;
        void *fb2 = NULL;
        switch (cfg->num_fbs) {
        case 3:
            err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 3, &fb0, &fb1, &fb2);
            break;
        case 2:
            err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1);
            break;
        default:
            err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb0);
            break;
        }
        s_fb[0] = fb0;
        s_fb[1] = fb1;
        s_fb[2] = fb2;
    }
    if (err != ESP_OK) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        return err;
    }
    s_cfg = *cfg;
    board_lcd_reset_stats();
    err = esp_register_shutdown_handler(board_lcd_shutdown);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // INVALID_STATE: already registered by an earlier init
        ESP_LOGW(TAG, "shutdown handler: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "RGB panel %dx%d RGB565 pclk=%d Hz fbs=%u bounce=%u lines nominal=%lu.%03lu Hz fb0=%p",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_LCD_PCLK_HZ, cfg->num_fbs, cfg->bounce_lines,
             (unsigned long)(board_lcd_nominal_mhz() / 1000), (unsigned long)(board_lcd_nominal_mhz() % 1000),
             s_fb[0]);
    return ESP_OK;
}

esp_err_t board_lcd_deinit(void)
{
    if (s_panel == NULL) {
        return ESP_OK;
    }
    (void)esp_unregister_shutdown_handler(board_lcd_shutdown); // the bus clock goes off with the panel
    esp_err_t err = esp_lcd_panel_del(s_panel);
    s_panel = NULL;
    portENTER_CRITICAL(&s_mux);
    memset(&s_chain, 0, sizeof(s_chain));
    s_chain_ctx = NULL;
    portEXIT_CRITICAL(&s_mux);
    s_fb[0] = NULL;
    s_fb[1] = NULL;
    s_fb[2] = NULL;
    return err;
}

esp_lcd_panel_handle_t board_lcd_panel(void)
{
    return s_panel;
}

uint16_t *board_lcd_fb(int index)
{
    if (index < 0 || index >= s_cfg.num_fbs) {
        return NULL;
    }
    return s_fb[index];
}

esp_err_t board_lcd_present(int index)
{
    uint16_t *fb = board_lcd_fb(index);
    ESP_RETURN_ON_FALSE(s_panel != NULL && fb != NULL, ESP_ERR_INVALID_STATE, TAG, "no panel/fb");
    if (s_cfg.bounce_lines == 0) {
        // DMA reads PSRAM directly: write the CPU cache back. With bounce buffers the driver copies
        // through the same cache, so no write-back is needed.
        ESP_RETURN_ON_ERROR(esp_cache_msync(fb, (size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2,
                                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
                            TAG, "cache sync");
    }
    // The driver's own frame buffer is not copied; with 2 fbs this selects the one to scan out.
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, fb);
}
