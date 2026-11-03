# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# net_status (TASK-080, ADR-015): Wi-Fi STA link owner and read-only network status. net_fsm.c is pure C
# (host tests in test/net_status); net_wifi.c is the ESP-IDF glue. No TLS, no pairing, no HID.
idf_component_register(
    SRCS "src/net_fsm.c" "src/net_wifi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer esp_hw_support freertos log)
