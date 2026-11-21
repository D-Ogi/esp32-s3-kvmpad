# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
idf_component_register(SRCS "peer.c" "bridge.c" "crop_wire.c"
    INCLUDE_DIRS "include"
    REQUIRES live_view
    PRIV_REQUIRES nvs_flash remote_protocol tcp_transport esp_timer esp_hw_support esp_event esp_netif
                  mbedtls secure_remote pack_runtime lwip esp_psram)
