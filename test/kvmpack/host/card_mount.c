// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "card_mount_platform.h"
#include "pack_card.h"
#include "ff.h"
#include "diskio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FATFS fs;
static sdmmc_card_t controller = {.csd.sector_size = 512};
static bool mounted, fail_next_read;
static int owner_task, other_task;
static TaskHandle_t task = &owner_task;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task; }
esp_err_t board_hal_exio_set(int pin, bool level)
{
    assert(pin == BOARD_EXIO_SD_CS && level);
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdmmc_mount(const char *path, const sdmmc_host_t *host, const void *slot,
                                  const esp_vfs_fat_sdmmc_mount_config_t *config, sdmmc_card_t **card)
{
    assert(!mounted && !strcmp(path, PACK_MOUNT) && host && slot && !config->format_if_mount_failed);
    assert(f_mount(&fs, "0:", 1) == FR_OK && fs.fs_type == FS_EXFAT && fs.volbase != 0);
    mounted = true;
    *card = &controller;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card)
{
    assert(mounted && !strcmp(path, PACK_MOUNT) && card == &controller);
    assert(f_mount(NULL, "0:", 0) == FR_OK);
    mounted = false;
    return ESP_OK;
}

uint8_t ff_diskio_get_pdrv_card(const sdmmc_card_t *card)
{
    assert(mounted && card == &controller);
    return 0;
}

esp_err_t sdmmc_read_sectors(sdmmc_card_t *card, void *data, size_t start, size_t count)
{
    assert(mounted && card == &controller && start == fs.volbase && count == 1);
    if (fail_next_read) {
        fail_next_read = false;
        return ESP_FAIL;
    }
    return disk_read(0, data, start, count) == RES_OK ? ESP_OK : ESP_FAIL;
}

static void format_card(void)
{
    BYTE work[FF_MAX_SS];
    MKFS_PARM options = {.fmt = FM_EXFAT};
    assert(f_mkfs("0:", &options, work, sizeof(work)) == FR_OK);
}

void check_card_mount(void *medium, size_t bytes, DWORD *clock)
{
    void *first_card = malloc(bytes), *second_card = malloc(bytes);
    assert(first_card && second_card);
    format_card();
    memcpy(first_card, medium, bytes);
    ++*clock;  // FatFs derives the exFAT serial from volume size and format time.
    format_card();
    memcpy(second_card, medium, bytes);

    pack_card_volume_t first, second, remounted;
    uint64_t free_bytes = 0;
    assert(pack_card_mount() == ESP_OK && pack_card_read_volume(&second, NULL));
    pack_card_close();
    memcpy(medium, first_card, bytes);
    assert(pack_card_mount() == ESP_OK && pack_card_read_volume(&first, &free_bytes));
    assert(free_bytes > 0 && free_bytes < bytes && first.sector == second.sector);
    assert(first.serial != second.serial && pack_card_same_volume(&first));

    task = &other_task;
    assert(!pack_card_present() && pack_card_mount() == ESP_ERR_INVALID_STATE);
    pack_card_close();
    task = &owner_task;
    assert(pack_card_same_volume(&first));

    memcpy(medium, second_card, bytes);
    assert(!pack_card_present());
    memcpy(medium, first_card, bytes);
    assert(!pack_card_same_volume(&first) && !pack_card_read_volume(&remounted, NULL));
    pack_card_close();
    assert(pack_card_mount() == ESP_OK && pack_card_read_volume(&remounted, NULL));
    assert(remounted.serial == first.serial && !pack_card_same_volume(&first));

    fail_next_read = true;
    assert(!pack_card_present() && !pack_card_present());
    pack_card_close();
    fail_next_read = true;
    assert(pack_card_mount() != ESP_OK && !mounted);
    assert(pack_card_mount() == ESP_OK && pack_card_present());
    pack_card_close();
    free(first_card);
    free(second_card);
    puts("PASS partitioned exFAT hot swap, remount, owner isolation and read failure recovery");
}
