# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# ui_platform (TASK-060A): LVGL 9.6 <-> esp_lcd integration and the only owner of the LVGL context.
# Exactly one backend source is compiled; the managed component for the other one is not resolved
# (see idf_component.yml rules), so the two binaries of the ADR-013 spike never mix the adapters.
set(srcs "src/ui_platform.c" "src/ui_lv_mem.c")
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
