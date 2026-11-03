# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# usb_hid_device: the only component that includes tusb.h / tinyusb.h (TASK-050 USB part).
# usb_hid_reports.c is pure C (host unit tests).
idf_component_register(
    SRCS "src/usb_hid_reports.c" "src/usb_hid_device.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_hw_support esp_timer log)
