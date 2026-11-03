# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# board_hal: I2C manager + I/O expander (TASK-010, ADR-026), RGB LCD (TASK-020). No LVGL, no TinyUSB.
idf_component_register(
    SRCS "src/io_ext.c" "src/board_hal.c" "src/board_lcd.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_lcd
    PRIV_REQUIRES esp_timer esp_mm log esp_hal_lcd esp_hw_support esp_system)

# board_lcd keeps its VSYNC / frame-complete telemetry when another component (the LVGL adapter) registers panel
# callbacks: the registration is routed through board_lcd.c, which chains the callbacks (see __wrap_… there).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_lcd_rgb_panel_register_event_callbacks")
