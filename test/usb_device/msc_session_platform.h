// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "session_platform.h"
#include "esp_blockdev.h"
typedef struct fixture_lun *tinyusb_msc_storage_handle_t;
typedef struct {
    struct { unsigned auto_mount_off; } user_flags;
} tinyusb_msc_driver_config_t;
enum { TINYUSB_MSC_STORAGE_MOUNT_USB = 1 };
typedef struct {
    struct { esp_blockdev_handle_t blockdev; } medium;
    struct { bool do_not_format; } fat_fs;
    int mount_point;
} tinyusb_msc_storage_config_t;
esp_err_t tinyusb_msc_install_driver(const tinyusb_msc_driver_config_t *);
esp_err_t tinyusb_msc_uninstall_driver(void);
esp_err_t tinyusb_msc_new_storage_blockdev(const tinyusb_msc_storage_config_t *, tinyusb_msc_storage_handle_t *);
esp_err_t tinyusb_msc_delete_storage(tinyusb_msc_storage_handle_t);
