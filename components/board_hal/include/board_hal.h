// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Board HAL (TASK-010): the single owner of the shared I2C bus and of the I/O expander
// (CH32V003 with the Waveshare expander firmware, ESP32-S3-Touch-LCD-7B, ADR-026).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t i2c_errors;        // failed expander transfers
    uint32_t i2c_lock_timeouts; // board_hal_i2c_lock() that timed out
    uint32_t i2c_bus_resets;    // board_hal_i2c_recover() calls
    uint32_t touch_resets;      // GT911 reset sequences
    uint8_t exio_shadow;        // last byte written to the expander output register
    uint8_t exio_readback;      // last byte read from the expander input register
    bool exio_verified;         // read-back matched the shadow at the last check
} board_hal_stats_t;

/**
 * Bring up the I2C bus and the expander in the safe state (spec 02 §1.1, ADR-026):
 * all EXIO as outputs, USB_SEL LOW (USB) — verified by read-back — touch and LCD held in
 * reset, LCD supply and backlight off. Must be the first hardware call in app_main, before the
 * USB stack.
 */
esp_err_t board_hal_init(void);

/** The shared bus handle (for panel IO creation). Every transfer must hold the bus lock. */
i2c_master_bus_handle_t board_hal_i2c_bus(void);

/** Take the bus lock (short holds only, TECH-013). ESP_ERR_TIMEOUT when not acquired. */
esp_err_t board_hal_i2c_lock(uint32_t timeout_ms);
void board_hal_i2c_unlock(void);

/** Set one EXIO output. USB_SEL=HIGH is refused with ESP_ERR_NOT_SUPPORTED. */
esp_err_t board_hal_exio_set(int exio, bool level);

/** Read the expander pin levels and compare them with the shadow. */
esp_err_t board_hal_exio_verify(uint8_t *shadow, uint8_t *readback, bool *match);

esp_err_t board_hal_backlight(bool on);

/** Backlight brightness 0..100 % through the expander PWM register (inverted duty). */
esp_err_t board_hal_backlight_level(uint8_t percent);

/** Panel supply on (LCD_VDD_EN), then the reset pulse: LCD_RST 0, 20 ms, 1, 100 ms. */
esp_err_t board_hal_lcd_power_reset(void);

/**
 * GT911 reset with the address strap on INT (GPIO4): INT driven to the level selecting
 * `addr` (0x5D: LOW, 0x14: HIGH), TP_RST 0 for 100 ms, TP_RST 1, hold 200 ms, then INT is
 * released as an input (no interrupt type — the touch layer configures it).
 */
esp_err_t board_hal_touch_reset(uint8_t addr);

/** Probe the 7-bit address range 0x08..0x77; returns found addresses (ascending). */
esp_err_t board_hal_i2c_scan(uint8_t *found, size_t max, size_t *count);

/** Probe one address under the bus lock. */
esp_err_t board_hal_i2c_probe(uint8_t addr);

/** Bus recovery after a stuck transfer (9 SCL clocks + STOP) under the bus lock. */
esp_err_t board_hal_i2c_recover(void);

/** Bring-up debug utility (TASK-010): one raw byte to / from a 7-bit address, under the bus lock.
 *  Refuses an expander output byte with USB_SEL=1 and any direction-register write. */
esp_err_t board_hal_i2c_raw_write(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t board_hal_i2c_raw_read(uint8_t addr, uint8_t *data, size_t len);

void board_hal_get_stats(board_hal_stats_t *out);

#ifdef __cplusplus
}
#endif
