// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "board_hal.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_ext.h"

static const char *TAG = "board_hal";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exp;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static uint8_t s_shadow;
static board_hal_stats_t s_stats;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

i2c_master_bus_handle_t board_hal_i2c_bus(void)
{
    return s_bus;
}

esp_err_t board_hal_i2c_lock(uint32_t timeout_ms)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        portENTER_CRITICAL(&s_stats_mux);
        s_stats.i2c_lock_timeouts++;
        portEXIT_CRITICAL(&s_stats_mux);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void board_hal_i2c_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void count_error(void)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.i2c_errors++;
    portEXIT_CRITICAL(&s_stats_mux);
}

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, out);
}

// ---- Expander register access (caller holds the lock) --------------------------------------
static esp_err_t exp_write_locked(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    esp_err_t err = i2c_master_transmit(s_exp, buf, sizeof(buf), BOARD_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        count_error();
    }
    return err;
}

static esp_err_t exp_read_locked(uint8_t reg, uint8_t *value)
{
    esp_err_t err = i2c_master_transmit_receive(s_exp, &reg, 1, value, 1, BOARD_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        count_error();
    }
    return err;
}

static esp_err_t exp_output_locked(uint8_t value)
{
    value = io_ext_enforce(value);
    esp_err_t err = exp_write_locked(BOARD_EXP_REG_OUTPUT, value);
    if (err == ESP_OK) {
        s_shadow = value;
        portENTER_CRITICAL(&s_stats_mux);
        s_stats.exio_shadow = value;
        portEXIT_CRITICAL(&s_stats_mux);
    }
    return err;
}

esp_err_t board_hal_exio_set(int exio, bool level)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    uint8_t next;
    if (!io_ext_apply(s_shadow, exio, level, &next)) {
        ESP_LOGE(TAG, "EXIO%d=%d refused (USB_SEL must stay LOW; valid EXIO 1..6)", exio, level);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(board_hal_i2c_lock(100), TAG, "bus lock");
    esp_err_t err = exp_output_locked(next);
    board_hal_i2c_unlock();
    return err;
}

esp_err_t board_hal_exio_verify(uint8_t *shadow, uint8_t *readback, bool *match)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    uint8_t rd = 0;
    ESP_RETURN_ON_ERROR(board_hal_i2c_lock(100), TAG, "bus lock");
    esp_err_t err = exp_read_locked(BOARD_EXP_REG_INPUT, &rd);
    uint8_t sh = s_shadow;
    board_hal_i2c_unlock();
    if (err != ESP_OK) {
        return err;
    }
    bool ok = io_ext_readback_matches(sh, rd);
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.exio_readback = rd;
    s_stats.exio_verified = ok;
    portEXIT_CRITICAL(&s_stats_mux);
    if (shadow) {
        *shadow = sh;
    }
    if (readback) {
        *readback = rd;
    }
    if (match) {
        *match = ok;
    }
    return ESP_OK;
}

esp_err_t board_hal_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");
    ESP_RETURN_ON_ERROR(add_dev(BOARD_EXP_ADDR, &s_exp), TAG, "expander dev");

    // The expander is a separate MCU that is NOT reset with the ESP32-S3: after a warm reset it
    // still drives the previous levels and was seen to report them for a short time after the
    // first writes (read-back 0x58 right after writing 0x10). The safe byte is therefore written
    // before and after the direction register and re-applied a bounded number of times until the
    // read-back confirms it; USB is never started without that confirmation.
    uint8_t sh = 0;
    uint8_t rd = 0;
    bool match = false;
    int attempt = 0;
    for (; attempt < BOARD_EXP_VERIFY_ATTEMPTS && !match; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(BOARD_EXP_VERIFY_DELAY_MS));
        }
        ESP_RETURN_ON_ERROR(board_hal_i2c_lock(100), TAG, "bus lock");
        esp_err_t err = exp_output_locked(io_ext_safe_default());
        if (err == ESP_OK) {
            err = exp_write_locked(BOARD_EXP_REG_MODE, 0xFF);
        }
        if (err == ESP_OK) {
            err = exp_output_locked(io_ext_safe_default());
        }
        board_hal_i2c_unlock();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "expander 0x%02X safe-state write (attempt %d): %s", BOARD_EXP_ADDR, attempt + 1,
                     esp_err_to_name(err));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        if (board_hal_exio_verify(&sh, &rd, &match) != ESP_OK) {
            match = false;
        }
    }
    ESP_LOGI(TAG, "expander safe state: shadow=0x%02X readback=0x%02X EXIO5(USB_SEL)=%d -> %s (attempt %d/%d)", sh,
             rd, io_ext_level(rd, BOARD_EXIO_USB_SEL), match ? "verified" : "MISMATCH", attempt,
             BOARD_EXP_VERIFY_ATTEMPTS);
    if (!match || io_ext_level(rd, BOARD_EXIO_USB_SEL)) {
        ESP_LOGE(TAG, "EXIO5 is not confirmed LOW (USB); refusing to continue");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t board_hal_backlight(bool on)
{
    return board_hal_exio_set(BOARD_EXIO_BL_EN, on);
}

esp_err_t board_hal_backlight_level(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(board_hal_i2c_lock(100), TAG, "bus lock");
    esp_err_t err = exp_write_locked(BOARD_EXP_REG_PWM, io_ext_pwm_from_percent(percent));
    board_hal_i2c_unlock();
    return err;
}

esp_err_t board_hal_lcd_power_reset(void)
{
    // Panel supply first, then the reset pulse (LCD_RST 0, 10 ms, 1, 100 ms).
    ESP_RETURN_ON_ERROR(board_hal_exio_set(BOARD_EXIO_LCD_RST, false), TAG, "lcd rst low");
    ESP_RETURN_ON_ERROR(board_hal_exio_set(BOARD_EXIO_LCD_VDD_EN, true), TAG, "lcd vdd on");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(board_hal_exio_set(BOARD_EXIO_LCD_RST, true), TAG, "lcd rst high");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t board_hal_touch_reset(uint8_t addr)
{
    ESP_RETURN_ON_FALSE(addr == BOARD_TOUCH_ADDR_PRIMARY || addr == BOARD_TOUCH_ADDR_BACKUP,
                        ESP_ERR_INVALID_ARG, TAG, "GT911 address 0x%02X", addr);
    // Hold the bus for the whole sequence: nobody may talk to GT911 while it is in reset.
    ESP_RETURN_ON_ERROR(board_hal_i2c_lock(500), TAG, "bus lock");
    gpio_intr_disable(BOARD_TOUCH_INT_GPIO);
    const gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    uint8_t next = 0;
    if (err == ESP_OK) {
        gpio_set_level(BOARD_TOUCH_INT_GPIO, addr == BOARD_TOUCH_ADDR_BACKUP ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        io_ext_apply(s_shadow, BOARD_EXIO_TP_RST, false, &next);
        err = exp_output_locked(next);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        io_ext_apply(s_shadow, BOARD_EXIO_TP_RST, true, &next);
        err = exp_output_locked(next);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    // Hand INT back to the GT911 as an input. gpio_reset_pin() also releases the output
    // reservation taken for the address strap, so the driver's later input configuration of the
    // same pin is not reported as a conflict.
    gpio_reset_pin(BOARD_TOUCH_INT_GPIO);
    const gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);
    board_hal_i2c_unlock();
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.touch_resets++;
    portEXIT_CRITICAL(&s_stats_mux);
    return err;
}

esp_err_t board_hal_i2c_probe(uint8_t addr)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(board_hal_i2c_lock(100), TAG, "bus lock");
    esp_err_t err = i2c_master_probe(s_bus, addr, BOARD_I2C_TIMEOUT_MS);
    board_hal_i2c_unlock();
    return err;
}

esp_err_t board_hal_i2c_scan(uint8_t *found, size_t max, size_t *count)
{
    ESP_RETURN_ON_FALSE(count != NULL, ESP_ERR_INVALID_ARG, TAG, "count");
    *count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (board_hal_i2c_probe(addr) == ESP_OK) {
            if (found != NULL && *count < max) {
                found[*count] = addr;
            }
            (*count)++;
        }
    }
    return ESP_OK;
}

esp_err_t board_hal_i2c_recover(void)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(board_hal_i2c_lock(500), TAG, "bus lock");
    esp_err_t err = i2c_master_bus_reset(s_bus);
    board_hal_i2c_unlock();
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.i2c_bus_resets++;
    portEXIT_CRITICAL(&s_stats_mux);
    return err;
}

static esp_err_t raw(uint8_t addr, uint8_t *data, size_t len, bool rd)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL && data != NULL && len > 0, ESP_ERR_INVALID_ARG, TAG, "raw args");
    // The debug utility may not bypass the USB mux invariant.
    if (!rd && addr == BOARD_EXP_ADDR && len >= 2 && data[0] == BOARD_EXP_REG_OUTPUT &&
        io_ext_level(data[1], BOARD_EXIO_USB_SEL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!rd && addr == BOARD_EXP_ADDR && len >= 2 && data[0] == BOARD_EXP_REG_MODE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    i2c_master_dev_handle_t dev;
    ESP_RETURN_ON_ERROR(add_dev(addr, &dev), TAG, "add dev");
    esp_err_t err = board_hal_i2c_lock(100);
    if (err == ESP_OK) {
        err = rd ? i2c_master_receive(dev, data, len, BOARD_I2C_TIMEOUT_MS)
                 : i2c_master_transmit(dev, data, len, BOARD_I2C_TIMEOUT_MS);
        board_hal_i2c_unlock();
    }
    i2c_master_bus_rm_device(dev);
    return err;
}

esp_err_t board_hal_i2c_raw_write(uint8_t addr, const uint8_t *data, size_t len)
{
    return raw(addr, (uint8_t *)data, len, false);
}

esp_err_t board_hal_i2c_raw_read(uint8_t addr, uint8_t *data, size_t len)
{
    return raw(addr, data, len, true);
}

void board_hal_get_stats(board_hal_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_stats_mux);
    memcpy(out, &s_stats, sizeof(*out));
    portEXIT_CRITICAL(&s_stats_mux);
}
