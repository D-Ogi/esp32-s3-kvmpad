# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# config_store (TASK-090): settings record, validation, migration and write debounce.
# config_record.c and config_store.c are pure C and are built by the host suite in test/config_store;
# config_store_nvs.c is the only file that touches ESP-IDF. No USB, no LVGL, no crypto here —
# the pairing register takes a SHA-256 digest from secure_remote, never a token.
idf_component_register(
    SRCS "src/config_record.c" "src/config_store.c" "src/config_store_nvs.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_timer log)
