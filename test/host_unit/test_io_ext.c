// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-010 / ADR-026: expander shadow logic — the EXIO5=LOW (USB) invariant can never be violated.
#include "board_pins.h"
#include "io_ext.h"
#include "kvm_test.h"

static void safe_default_is_usb_and_resets_held(void)
{
    uint8_t b = io_ext_safe_default();
    CHECK(!io_ext_level(b, BOARD_EXIO_USB_SEL));
    CHECK(!io_ext_level(b, BOARD_EXIO_TP_RST));
    CHECK(!io_ext_level(b, BOARD_EXIO_LCD_RST));
    CHECK(!io_ext_level(b, BOARD_EXIO_BL_EN));
    CHECK(!io_ext_level(b, BOARD_EXIO_LCD_VDD_EN));
    CHECK(io_ext_level(b, BOARD_EXIO_SD_CS));
    CHECK_EQ(b & (uint8_t)~IO_EXT_USED_MASK, 0);
    CHECK(b != 0xFF); // never the vendor demo's implicit all-HIGH byte (CAN selected)
}

static void usb_sel_high_is_refused(void)
{
    uint8_t out = 0xEE;
    CHECK(!io_ext_apply(io_ext_safe_default(), BOARD_EXIO_USB_SEL, true, &out));
    CHECK_EQ(out, 0xEE); // untouched
    CHECK(io_ext_apply(io_ext_safe_default(), BOARD_EXIO_USB_SEL, false, &out));
    CHECK(!io_ext_level(out, BOARD_EXIO_USB_SEL));
}

static void enforce_clears_usb_sel_and_unused_bits(void)
{
    CHECK_EQ(io_ext_enforce(0xFF), IO_EXT_USED_MASK & (uint8_t)~(1u << BOARD_EXIO_USB_SEL));
    for (int v = 0; v < 256; v++) {
        uint8_t e = io_ext_enforce((uint8_t)v);
        CHECK(!io_ext_level(e, BOARD_EXIO_USB_SEL));
        CHECK_EQ(e & 0x81, 0);
    }
}

static void apply_sets_and_clears_every_exio(void)
{
    for (int exio = IO_EXT_EXIO_MIN; exio <= IO_EXT_EXIO_MAX; exio++) {
        if (exio == BOARD_EXIO_USB_SEL) {
            continue;
        }
        uint8_t on = 0;
        uint8_t off = 0;
        CHECK(io_ext_apply(0, exio, true, &on));
        CHECK(io_ext_level(on, exio));
        CHECK(io_ext_apply(on, exio, false, &off));
        CHECK(!io_ext_level(off, exio));
    }
}

static void out_of_range_exio_is_refused(void)
{
    uint8_t out = 0;
    CHECK(!io_ext_apply(0, 0, true, &out));
    CHECK(!io_ext_apply(0, 7, true, &out));
    CHECK(!io_ext_apply(0, -1, true, &out));
    CHECK(!io_ext_apply(0, 1, true, NULL));
}

static void readback_ignores_unused_bits(void)
{
    CHECK(io_ext_readback_matches(0x10, 0x10 | 0x81));
    CHECK(!io_ext_readback_matches(0x10, 0x30)); // USB_SEL reads HIGH
    CHECK(!io_ext_readback_matches(0x10, 0x00)); // SD_CS reads LOW
}

static void pwm_is_inverted_and_capped(void)
{
    CHECK_EQ(io_ext_pwm_from_percent(100), 0);
    CHECK_EQ(io_ext_pwm_from_percent(0), BOARD_EXP_PWM_MAX);
    CHECK_EQ(io_ext_pwm_from_percent(200), 0);
    CHECK(io_ext_pwm_from_percent(50) > 100 && io_ext_pwm_from_percent(50) < 150);
    for (int p = 1; p <= 100; p++) {
        CHECK(io_ext_pwm_from_percent((uint8_t)p) <= io_ext_pwm_from_percent((uint8_t)(p - 1)));
    }
}

int main(void)
{
    printf("test_io_ext\n");
    RUN(safe_default_is_usb_and_resets_held);
    RUN(usb_sel_high_is_refused);
    RUN(enforce_clears_usb_sel_and_unused_bits);
    RUN(apply_sets_and_clears_every_exio);
    RUN(out_of_range_exio_is_refused);
    RUN(readback_ignores_unused_bits);
    RUN(pwm_is_inverted_and_capped);
    TEST_MAIN_END("test_io_ext");
}
