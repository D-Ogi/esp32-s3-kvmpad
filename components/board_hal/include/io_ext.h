// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// I/O expander output shadow: pure logic (no I/O), unit-tested on the host. Enforces the board
// invariants on every byte that may reach the expander output register.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IO_EXT_EXIO_MIN   1
#define IO_EXT_EXIO_MAX   6

/** Bits the board uses (EXIO1..EXIO6); EXIO0 and EXIO7 are unconnected and kept at 0. */
#define IO_EXT_USED_MASK  0x7E

/**
 * Safe power-on output byte: touch and LCD held in reset, LCD supply and backlight off, SD
 * deselected, USB_SEL LOW (USB). Never the vendor demo's implicit 0xFF (that selects CAN).
 */
uint8_t io_ext_safe_default(void);

/** Enforce the invariants on a byte: USB_SEL LOW and unused bits 0. */
uint8_t io_ext_enforce(uint8_t shadow);

/**
 * New shadow byte with EXIO `exio` set to `level`.
 * @return false (shadow unchanged) for an out-of-range EXIO or for USB_SEL=HIGH (CAN is disabled
 *         in v1, spec 02 §1.1).
 */
bool io_ext_apply(uint8_t shadow, int exio, bool level, uint8_t *out);

/** Level of EXIO `exio` in `byte` (shadow or read-back). */
bool io_ext_level(uint8_t byte, int exio);

/** True when the read-back pin levels match the shadow on every used bit. */
bool io_ext_readback_matches(uint8_t shadow, uint8_t readback);

/** Backlight brightness 0..100 % to the expander's inverted PWM register value. */
uint8_t io_ext_pwm_from_percent(uint8_t percent);

#ifdef __cplusplus
}
#endif
