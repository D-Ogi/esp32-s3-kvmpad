# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# touch_input: the single GT911 reader (TASK-030). touch_sample.c is pure C (host unit tests).
idf_component_register(
    SRCS "src/touch_sample.c" "src/touch_input.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd freertos
    PRIV_REQUIRES board_hal esp_driver_gpio esp_timer log)
