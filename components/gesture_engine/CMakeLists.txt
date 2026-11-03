# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# gesture_engine (TASK-040): pure C, also compiled by the host unit tests (test/host_unit/gesture_*.c).
idf_component_register(
    SRCS "src/gesture_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_core touch_input)
