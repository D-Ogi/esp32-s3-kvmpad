# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# hid_core (TASK-050 core): hid_core.c, kbd_fsm.c and hid_keymap.c are pure C (host unit tests in
# test/host_unit); hid_dispatch.c is the firmware owner that runs the core in the TinyUSB task.
idf_component_register(
    SRCS "src/hid_core.c" "src/kbd_fsm.c" "src/hid_keymap.c" "src/hid_dispatch.c"
    INCLUDE_DIRS "include"
    REQUIRES usb_hid_device
    PRIV_REQUIRES esp_timer log freertos)
