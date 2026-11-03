# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# update_manager (TASK-110): signed OTA, maintenance mode, health check and rollback.
#
# update_image.c, update_health.c and update_manager.c are pure C and are built by the host suite
# in test/update_manager; update_platform_esp.c, update_transport_https.c and update_console.c are
# the only files that touch ESP-IDF. The component emits no USB HID and holds no network entry
# point: an update is started locally (console or Settings), never from the CLIENT protocol
# (TECH-045, AT-SEC-07, ADR-031).
idf_component_register(
    SRCS "src/update_image.c" "src/update_health.c" "src/update_manager.c"
         "src/update_platform_esp.c" "src/update_transport_https.c" "src/update_console.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_core
    PRIV_REQUIRES app_update bootloader_support esp_app_format esp_partition esp_http_client
                  esp-tls mbedtls hal esp_timer console log usb_hid_device touch_input ui_platform)
