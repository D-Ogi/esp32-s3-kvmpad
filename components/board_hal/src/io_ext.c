// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "io_ext.h"

#include "board_pins.h"

#define BIT_OF(exio) ((uint8_t)(1u << (exio)))

uint8_t io_ext_safe_default(void)
{
    // TP_RST=0, BL_EN=0, LCD_RST=0, LCD_VDD_EN=0, SD_CS=1 (deselected), USB_SEL=0 (USB).
    return BIT_OF(BOARD_EXIO_SD_CS);
}

uint8_t io_ext_enforce(uint8_t shadow)
{
    shadow &= IO_EXT_USED_MASK;
    shadow &= (uint8_t)~BIT_OF(BOARD_EXIO_USB_SEL);
    return shadow;
}

bool io_ext_apply(uint8_t shadow, int exio, bool level, uint8_t *out)
{
    if (out == 0 || exio < IO_EXT_EXIO_MIN || exio > IO_EXT_EXIO_MAX) {
        return false;
    }
    if (exio == BOARD_EXIO_USB_SEL && level) {
        return false;
    }
    uint8_t next = level ? (uint8_t)(shadow | BIT_OF(exio)) : (uint8_t)(shadow & ~BIT_OF(exio));
    *out = io_ext_enforce(next);
    return true;
}

bool io_ext_level(uint8_t byte, int exio)
{
    if (exio < 0 || exio > 7) {
        return false;
    }
    return (byte & BIT_OF(exio)) != 0;
}

bool io_ext_readback_matches(uint8_t shadow, uint8_t readback)
{
    return ((shadow ^ readback) & IO_EXT_USED_MASK) == 0;
}

uint8_t io_ext_pwm_from_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    // Inverted duty: 100 % brightness -> 0; 0 % -> the vendor cap.
    return (uint8_t)(((uint32_t)(100u - percent) * BOARD_EXP_PWM_MAX + 50u) / 100u);
}
