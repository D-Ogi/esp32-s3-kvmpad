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

set(KVM_LCD_IRQ_CORE1 "AUTO" CACHE STRING "RGB LCD interrupt affinity: AUTO, ON (CPU1), or OFF (caller)")
set_property(CACHE KVM_LCD_IRQ_CORE1 PROPERTY STRINGS AUTO ON OFF)
if(NOT KVM_LCD_IRQ_CORE1 MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "KVM_LCD_IRQ_CORE1 must be AUTO, ON or OFF")
endif()
set(lcd_product_profile OFF)
if(CONFIG_IDF_TARGET_ESP32S3 AND CONFIG_FREERTOS_NUMBER_OF_CORES EQUAL 2
   AND CONFIG_KVM_UI_PLATFORM AND CONFIG_KVM_UI_BACKEND_ADAPTER AND CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL
   AND CONFIG_KVM_UI_DRAW_BUF_LINES EQUAL 12 AND NOT KVM_LCD_LONGER_REFILL)
    set(lcd_product_profile ON)
endif()
if(KVM_LCD_IRQ_CORE1 STREQUAL "ON" AND NOT lcd_product_profile)
    message(FATAL_ERROR "The CPU1 RGB IRQ profile requires a dual-core ESP32-S3 and the standard 10/12 partial display")
endif()
if(lcd_product_profile AND KVM_MEDIA_FFMPEG AND NOT KVM_MEDIA_SYNTHETIC_DIAG AND KVM_LCD_IRQ_CORE1 STREQUAL "OFF")
    message(FATAL_ERROR "7B media firmware requires both RGB interrupts on CPU1. Configure with -DKVM_LCD_IRQ_CORE1=AUTO. Use an engineering build for the CPU0 comparison.")
endif()
if(lcd_product_profile AND NOT KVM_LCD_IRQ_CORE1 STREQUAL "OFF")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_LCD_IRQ_CORE1=1)
endif()
