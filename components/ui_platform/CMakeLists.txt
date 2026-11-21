# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# ui_platform (TASK-060A): LVGL 9.6 <-> esp_lcd integration and the only owner of the LVGL context.
# Exactly one backend source is compiled; the managed component for the other one is not resolved
# (see idf_component.yml rules), so the two binaries of the ADR-013 spike never mix the adapters.
set(srcs "src/ui_platform.c" "src/ui_lv_mem.c")
if(CONFIG_KVM_USB_MONITOR_EVAL AND CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL AND CONFIG_LV_USE_SNAPSHOT)
    list(APPEND srcs "src/ui_scanout.c")
    set(capture_scanout ON)
endif()
set(priv_req board_hal esp_timer esp_mm log freertos heap)
if(CONFIG_KVM_UI_BACKEND_PORT)
    list(APPEND srcs "src/ui_backend_port.c")
    list(APPEND priv_req espressif__esp_lvgl_port)
else()
    list(APPEND srcs "src/ui_backend_adapter.c")
    list(APPEND priv_req espressif__esp_lvgl_adapter)
endif()
if(CONFIG_KVM_UI_BENCH)
    list(APPEND srcs "src/ui_bench.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl esp_lcd
    PRIV_REQUIRES ${priv_req})

if(capture_scanout)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_CAPTURE_SCANOUT=1)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_lcd_panel_draw_bitmap")
endif()

if(KVM_LCD_LONGER_REFILL)
    if(NOT CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL OR NOT CONFIG_KVM_UI_DRAW_BUF_LINES EQUAL 12)
        message(FATAL_ERROR "KVM_LCD_LONGER_REFILL requires the 12-line DOUBLE_PARTIAL baseline")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_LCD_LONGER_REFILL=1)
endif()
