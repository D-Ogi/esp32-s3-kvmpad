// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103 };
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);

enum { BOARD_EXIO_SD_CS, BOARD_SD_CLK, BOARD_SD_CMD, BOARD_SD_D0 };
enum { SDMMC_SLOT_FLAG_INTERNAL_PULLUP = 1 };
typedef struct { int slot; } sdmmc_host_t;
typedef struct { int width, clk, cmd, d0, flags; } sdmmc_slot_config_t;
typedef struct { bool format_if_mount_failed; int max_files; } esp_vfs_fat_sdmmc_mount_config_t;
typedef struct { struct { unsigned sector_size; } csd; } sdmmc_card_t;
#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){0})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})

esp_err_t board_hal_exio_set(int pin, bool level);
esp_err_t esp_vfs_fat_sdmmc_mount(const char *path, const sdmmc_host_t *host, const void *slot,
                                  const esp_vfs_fat_sdmmc_mount_config_t *config, sdmmc_card_t **card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card);
uint8_t ff_diskio_get_pdrv_card(const sdmmc_card_t *card);
esp_err_t sdmmc_read_sectors(sdmmc_card_t *card, void *data, size_t start, size_t count);
