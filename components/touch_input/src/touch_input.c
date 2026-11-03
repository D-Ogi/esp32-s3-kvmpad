// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// The esp_lcd_touch_gt911 component (pinned 1.2.1) is used for creation, the configuration read
// and INT registration. Sampling uses our own bounded read of 0x814E/0x814F through the same
// panel IO handle: the 1.2.1 read path keeps the previous points when the buffer-ready bit is
// clear, which would hand stale coordinates to the gesture engine.
#include "touch_input.h"

#include <string.h>

#include "board_hal.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_LCD_TOUCH_MAX_POINTS != 5
#error "TECH-014: CONFIG_ESP_LCD_TOUCH_MAX_POINTS must be 5"
#endif

static const char *TAG = "touch_input";

#define QUEUE_LEN            16
#define POLL_ACTIVE_MS       20    // sanity read while fingers are down
#define POLL_IDLE_MS         100   // sanity read when idle (a lost edge never strands a touch)
#define GAP_CANCEL_US        120000 // TECH-064
#define LOCK_TIMEOUT_MS      10
#define ERRORS_BEFORE_RECOVERY 3
#define BACKOFF_MIN_MS       10
#define BACKOFF_MAX_MS       1000

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static StaticQueue_t s_queue_buf;
static uint8_t s_queue_storage[QUEUE_LEN * sizeof(kvm_touch_sample_t)];
static touch_input_info_t s_info;
static kvm_touch_transform_t s_xf;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static touch_input_stats_t s_stats;
static kvm_touch_sample_t s_last;
static bool s_have_last;
static volatile uint32_t s_inject;
static volatile uint32_t s_irq_count; // written only by the INT ISR

#define STAT_INC(field)                 \
    do {                                \
        portENTER_CRITICAL(&s_mux);     \
        s_stats.field++;                \
        portEXIT_CRITICAL(&s_mux);      \
    } while (0)

static void on_int(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    // ISR: notify only (TECH-012).
    BaseType_t woken = pdFALSE;
    s_irq_count++;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static esp_err_t rx_locked(uint16_t reg, uint8_t *buf, size_t len)
{
    esp_err_t err = board_hal_i2c_lock(LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_io_rx_param(s_io, reg, buf, len);
    board_hal_i2c_unlock();
    return err;
}

static esp_err_t tx_locked(uint16_t reg, uint8_t value)
{
    esp_err_t err = board_hal_i2c_lock(LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_io_tx_param(s_io, reg, &value, 1);
    board_hal_i2c_unlock();
    return err;
}

static void publish(kvm_touch_sample_t *s, uint32_t *seq)
{
    s->sample_seq = ++(*seq);
    if (xQueueSend(s_queue, s, 0) != pdTRUE) {
        kvm_touch_sample_t drop;
        xQueueReceive(s_queue, &drop, 0); // drop the oldest; the newest state always arrives
        xQueueSend(s_queue, s, 0);
        STAT_INC(queue_overflows);
    }
    portENTER_CRITICAL(&s_mux);
    s_last = *s;
    s_have_last = true;
    s_stats.samples++;
    if (!s->valid) {
        s_stats.invalid_samples++;
    }
    if (s->point_count > s_stats.max_points_seen) {
        s_stats.max_points_seen = s->point_count;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void rearm_int(void)
{
    gpio_set_intr_type(BOARD_TOUCH_INT_GPIO, GPIO_INTR_NEGEDGE);
    gpio_intr_enable(BOARD_TOUCH_INT_GPIO);
}

static void recover(uint32_t *backoff_ms)
{
    ESP_LOGW(TAG, "recovery: bus reset + GT911 re-reset (backoff %lu ms)", (unsigned long)*backoff_ms);
    vTaskDelay(pdMS_TO_TICKS(*backoff_ms));
    board_hal_i2c_recover();
    board_hal_touch_reset(s_info.i2c_addr);
    rearm_int();
    STAT_INC(recoveries);
    *backoff_ms = *backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : *backoff_ms * 2;
}

static void touch_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;
    bool active = false;          // last published sample had fingers down
    bool error_streak = false;    // a cancel was already published for the current error streak
    uint32_t consecutive_errors = 0;
    uint32_t backoff_ms = BACKOFF_MIN_MS;
    int64_t last_frame_us = esp_timer_get_time();
    uint8_t buf[KVM_TOUCH_MAX_POINTS * GT911_POINT_BYTES];
    kvm_touch_sample_t sample;

    // Initial empty frame: consumers start from a known released state.
    memset(&sample, 0, sizeof(sample));
    sample.valid = true;
    sample.timestamp_us = (uint64_t)esp_timer_get_time();
    publish(&sample, &seq);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(active ? POLL_ACTIVE_MS : POLL_IDLE_MS));
        int64_t t0 = esp_timer_get_time();
        STAT_INC(reads);

        uint8_t status = 0;
        esp_err_t err;
        if (s_inject > 0) {
            s_inject--;
            STAT_INC(injected_faults);
            err = ESP_FAIL;
        } else {
            err = rx_locked(GT911_REG_STATUS, &status, 1);
        }
        uint8_t n = status & GT911_STATUS_COUNT;
        if (err == ESP_OK && (status & GT911_STATUS_READY) && n > 0 && n <= KVM_TOUCH_MAX_POINTS) {
            err = rx_locked(GT911_REG_POINTS, buf, (size_t)n * GT911_POINT_BYTES);
        }
        if (err == ESP_OK && (status & GT911_STATUS_READY)) {
            err = tx_locked(GT911_REG_STATUS, 0); // hand the buffer back to GT911
        }
        int64_t t1 = esp_timer_get_time();
        uint32_t read_us = (uint32_t)(t1 - t0);
        portENTER_CRITICAL(&s_mux);
        if (read_us > s_stats.max_read_us) {
            s_stats.max_read_us = read_us;
        }
        portEXIT_CRITICAL(&s_mux);

        if (err != ESP_OK) {
            STAT_INC(i2c_errors);
            consecutive_errors++;
            if (!error_streak) {
                // Error: publish a cancel at once, no leftover DOWN (TECH-012).
                sample.timestamp_us = (uint64_t)t1;
                kvm_touch_make_invalid(&sample);
                publish(&sample, &seq);
                error_streak = true;
                active = false;
            }
            if (consecutive_errors >= ERRORS_BEFORE_RECOVERY) {
                recover(&backoff_ms);
                consecutive_errors = 0;
            }
            continue;
        }
        consecutive_errors = 0;
        backoff_ms = BACKOFF_MIN_MS;

        sample.timestamp_us = (uint64_t)t1;
        kvm_touch_decode_t d = kvm_touch_decode_gt911(status, buf, sizeof(buf), &s_xf, &sample);
        if (d == KVM_TOUCH_DECODE_NOT_READY) {
            if (active && (t1 - last_frame_us) > GAP_CANCEL_US) {
                kvm_touch_make_invalid(&sample);
                publish(&sample, &seq);
                STAT_INC(gap_cancels);
                active = false;
            }
            continue;
        }
        if (d == KVM_TOUCH_DECODE_INVALID) {
            STAT_INC(corrupt_frames);
        }
        if (d == KVM_TOUCH_DECODE_OK && error_streak) {
            error_streak = false;
        }
        last_frame_us = t1;
        publish(&sample, &seq);
        active = sample.valid && sample.point_count > 0;
    }
}

static esp_err_t read_info(void)
{
    uint8_t b[11] = {0};
    ESP_RETURN_ON_ERROR(rx_locked(GT911_REG_PRODUCT_ID, b, sizeof(b)), TAG, "product info");
    memcpy(s_info.product_id, b, 4);
    s_info.product_id[4] = '\0';
    s_info.fw_version = (uint16_t)(b[4] | (b[5] << 8));
    s_info.info_x_res = (uint16_t)(b[6] | (b[7] << 8));
    s_info.info_y_res = (uint16_t)(b[8] | (b[9] << 8));
    s_info.vendor_id = b[10];

    uint8_t c[7] = {0};
    ESP_RETURN_ON_ERROR(rx_locked(GT911_REG_CONFIG, c, sizeof(c)), TAG, "config");
    s_info.config_version = c[0];
    s_info.cfg_x_max = (uint16_t)(c[1] | (c[2] << 8));
    s_info.cfg_y_max = (uint16_t)(c[3] | (c[4] << 8));
    s_info.cfg_touch_number = c[5] & 0x0F;
    s_info.cfg_int_mode = c[6] & 0x03;
    return ESP_OK;
}

esp_err_t touch_input_start(void)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_queue = xQueueCreateStatic(QUEUE_LEN, sizeof(kvm_touch_sample_t), s_queue_storage, &s_queue_buf);

    // Address strap for 0x5D, then probe what actually answers (spec 02 §1.2 item 3).
    ESP_RETURN_ON_ERROR(board_hal_touch_reset(BOARD_TOUCH_ADDR_PRIMARY), TAG, "GT911 reset");
    uint8_t addr = 0;
    if (board_hal_i2c_probe(BOARD_TOUCH_ADDR_PRIMARY) == ESP_OK) {
        addr = BOARD_TOUCH_ADDR_PRIMARY;
    } else if (board_hal_i2c_probe(BOARD_TOUCH_ADDR_BACKUP) == ESP_OK) {
        addr = BOARD_TOUCH_ADDR_BACKUP;
    }
    ESP_RETURN_ON_FALSE(addr != 0, ESP_ERR_NOT_FOUND, TAG, "GT911 answers neither 0x5D nor 0x14");
    s_info.i2c_addr = addr;

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    io_cfg.transaction_timeout_ms = BOARD_I2C_TIMEOUT_MS;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(board_hal_i2c_bus(), &io_cfg, &s_io), TAG, "panel io");

    // rst = NC: the component skips its own strap (done above through the expander) and configures
    // INT as a NEGEDGE input with our ISR.
    esp_lcd_touch_io_gt911_config_t gt_cfg = {.dev_addr = addr};
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_TOUCH_H_RES,
        .y_max = BOARD_TOUCH_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = BOARD_TOUCH_SWAP_XY, .mirror_x = BOARD_TOUCH_MIRROR_X, .mirror_y = BOARD_TOUCH_MIRROR_Y},
        .interrupt_callback = on_int,
        .driver_data = &gt_cfg,
    };
    ESP_RETURN_ON_ERROR(board_hal_i2c_lock(500), TAG, "bus lock");
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(s_io, &tp_cfg, &s_tp);
    board_hal_i2c_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "GT911 init");

    ESP_RETURN_ON_ERROR(read_info(), TAG, "GT911 info");
    ESP_RETURN_ON_FALSE(s_info.cfg_x_max > 0 && s_info.cfg_y_max > 0, ESP_ERR_INVALID_RESPONSE, TAG,
                        "GT911 reports a zero resolution");
    s_xf = (kvm_touch_transform_t){
        .raw_x_max = s_info.cfg_x_max,
        .raw_y_max = s_info.cfg_y_max,
        .h_res = BOARD_TOUCH_H_RES,
        .v_res = BOARD_TOUCH_V_RES,
        .swap_xy = BOARD_TOUCH_SWAP_XY,
        .mirror_x = BOARD_TOUCH_MIRROR_X,
        .mirror_y = BOARD_TOUCH_MIRROR_Y,
    };
    ESP_LOGI(TAG,
             "GT911 addr=0x%02X product=\"%s\" fw=0x%04X info_res=%ux%u vendor=0x%02X cfg_ver=%u cfg_res=%ux%u "
             "touch_number=%u int_mode=%u",
             addr, s_info.product_id, s_info.fw_version, s_info.info_x_res, s_info.info_y_res, s_info.vendor_id,
             s_info.config_version, s_info.cfg_x_max, s_info.cfg_y_max, s_info.cfg_touch_number,
             s_info.cfg_int_mode);
    if (s_info.cfg_x_max != BOARD_TOUCH_H_RES || s_info.cfg_y_max != BOARD_TOUCH_V_RES) {
        ESP_LOGW(TAG, "GT911 range %ux%u differs from the panel; points are scaled", s_info.cfg_x_max,
                 s_info.cfg_y_max);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(touch_task, "touch_sampler", 4096, NULL, 12, &s_task, tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

bool touch_input_receive(kvm_touch_sample_t *out, TickType_t wait)
{
    if (s_queue == NULL) {
        // Not started yet: still honour the wait, so a consumer looping with portMAX_DELAY blocks
        // instead of spinning (a spinning consumer starved app_main before touch_input_start).
        vTaskDelay(wait == portMAX_DELAY ? pdMS_TO_TICKS(100) : (wait > 0 ? wait : 1));
        return false;
    }
    return xQueueReceive(s_queue, out, wait) == pdTRUE;
}

bool touch_input_get_last(kvm_touch_sample_t *out)
{
    portENTER_CRITICAL(&s_mux);
    bool have = s_have_last;
    if (have) {
        *out = s_last;
    }
    portEXIT_CRITICAL(&s_mux);
    return have;
}

void touch_input_get_info(touch_input_info_t *out)
{
    *out = s_info;
}

void touch_input_get_stats(touch_input_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
    out->irq_count = s_irq_count;
}

void touch_input_inject_faults(uint32_t count)
{
    s_inject = count;
}
