# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# ui_touch_bridge (TASK-060E): touch_router.c is pure C (host tests in test/ui_touch); ui_touch_bridge.c is
# the firmware task with the LVGL pointer indev and the gesture FSM. No GT911 access and no USB stack.
idf_component_register(
    SRCS "src/touch_router.c" "src/ui_touch_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES touch_input gesture_engine
    PRIV_REQUIRES ui_platform lvgl__lvgl esp_timer freertos log)
