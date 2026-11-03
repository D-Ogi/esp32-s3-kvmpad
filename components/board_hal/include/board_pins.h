// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pinmap of the Waveshare ESP32-S3-Touch-LCD-7B (1024x600, GT911, CH32V003 I/O expander) — the
// target board per ADR-026, which replaces the 800x480 board named in spec 02 §1.1. Sources: the
// Waveshare 7B documentation and schematic, the Waveshare 7B ESP-IDF demo (commit c652c90), and
// measurements on board #1 (hardware/boards.md). Nothing else in the firmware hardcodes a GPIO number.
#pragma once

// ---- Shared I2C bus (I/O expander + GT911 + external header) --------------------------------
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA_GPIO      8
#define BOARD_I2C_SCL_GPIO      9
#define BOARD_I2C_FREQ_HZ       400000
#define BOARD_I2C_TIMEOUT_MS    20      // every transfer is bounded (TECH-012)

// ---- GT911 ----------------------------------------------------------------------------------
#define BOARD_TOUCH_INT_GPIO    4       // interrupt input; the ISR only notifies (TECH-012)
#define BOARD_TOUCH_ADDR_PRIMARY 0x5D   // INT held LOW across the reset release
#define BOARD_TOUCH_ADDR_BACKUP  0x14   // INT held HIGH across the reset release
#define BOARD_TOUCH_H_RES       1024
#define BOARD_TOUCH_V_RES       600
// Explicit 0° transform (TECH-011), checked with the five control points on board #1 — see
// hardware/boards.md; never "try permutations" at runtime.
#define BOARD_TOUCH_SWAP_XY     0
#define BOARD_TOUCH_MIRROR_X    0
#define BOARD_TOUCH_MIRROR_Y    0

// ---- I/O expander: CH32V003 running the Waveshare expander firmware at 0x24 -------------------
// Register protocol (Waveshare 7B demo, io_extension.c): write {reg, value}; read: write {reg},
// then read one byte.
#define BOARD_EXP_ADDR          0x24
#define BOARD_EXP_REG_MODE      0x02    // per-pin direction, 1 = output
#define BOARD_EXP_REG_OUTPUT    0x03    // output levels, bit n = EXIOn
#define BOARD_EXP_REG_INPUT     0x04    // pin levels, bit n = EXIOn
#define BOARD_EXP_REG_PWM       0x05    // backlight dimming duty, inverted: 0 = full brightness
#define BOARD_EXP_REG_ADC       0x06    // battery voltage (16-bit)
#define BOARD_EXP_VERIFY_ATTEMPTS 5   // bounded safe-state re-apply + read-back at init (ADR-026)
#define BOARD_EXP_VERIFY_DELAY_MS 10
#define BOARD_EXP_PWM_MAX       247     // Waveshare caps the duty at 97 % (97 * 255 / 100)

// EXIO roles (schematic nets). EXIO0 and EXIO7 are unconnected and kept LOW.
#define BOARD_EXIO_TP_RST       1       // CTP_RST, GT911 reset, active LOW
#define BOARD_EXIO_BL_EN        2       // DISP, backlight enable
#define BOARD_EXIO_LCD_RST      3       // LCD_RST, active LOW
#define BOARD_EXIO_SD_CS        4       // microSD chip select, active LOW (card unused in v1)
#define BOARD_EXIO_USB_SEL      5       // USB_SEL: LOW = USB, HIGH = CAN (CAN is disabled in v1)
#define BOARD_EXIO_LCD_VDD_EN   6       // LCD_VDD_EN, panel supply / VCOM enable

// ---- Native USB OTG (owned by the USB stack, never re-muxed at runtime) ---------------------
#define BOARD_USB_DM_GPIO       19
#define BOARD_USB_DP_GPIO       20

// ---- RGB parallel LCD (16-bit RGB565), timing from the Waveshare 7B demo ---------------------
#define BOARD_LCD_H_RES         1024
#define BOARD_LCD_V_RES         600
#define BOARD_LCD_PCLK_HZ       (30 * 1000 * 1000)   // vendor value; do not raise blindly (25 MHz flickered: ADR-038 rejected)
#define BOARD_LCD_HSYNC_PULSE   162
#define BOARD_LCD_HSYNC_BP      152
#define BOARD_LCD_HSYNC_FP      48
#define BOARD_LCD_VSYNC_PULSE   45
#define BOARD_LCD_VSYNC_BP      13
#define BOARD_LCD_VSYNC_FP      3
#define BOARD_LCD_PCLK_ACTIVE_NEG 1

#define BOARD_LCD_PCLK_GPIO     7
#define BOARD_LCD_VSYNC_GPIO    3
#define BOARD_LCD_HSYNC_GPIO    46
#define BOARD_LCD_DE_GPIO       5
#define BOARD_LCD_DISP_GPIO     (-1)  // display enable goes through the expander (EXIO2)

// TECH-001: data_gpio_nums[] from LSB (D0 = B3) to MSB (D15 = R7) following RGB565
// (bits 0..4 blue, 5..10 green, 11..15 red). Identical to the Waveshare 7B demo; physical
// verification by the red/green/blue swatch test is recorded in hardware/boards.md.
#define BOARD_LCD_DATA_GPIOS { \
    14, 38, 18, 17, 10,        /* D0..D4  = B3 B4 B5 B6 B7     */ \
    39, 0, 45, 48, 47, 21,     /* D5..D10 = G2 G3 G4 G5 G6 G7  */ \
    1, 2, 42, 41, 40           /* D11..D15 = R3 R4 R5 R6 R7    */ \
}
