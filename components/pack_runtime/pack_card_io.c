// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_card.h"
#include "pack_card_bus.h"
#include "board_hal.h"
#include "board_pins.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include <stdatomic.h>
#include <string.h>

static sdmmc_card_t *card;
static pack_card_volume_t mounted_volume;
static bool volume_lost;
static _Atomic(TaskHandle_t) owner;
static esp_err_t select_native(void) { return board_hal_exio_set(BOARD_EXIO_SD_CS, true); }
static bool identify_volume(void);

esp_err_t pack_card_mount(void) { return pack_card_mount_on_bus(select_native); }

esp_err_t pack_card_mount_on_bus(esp_err_t (*prepare)(void))
{
    TaskHandle_t idle = NULL;
    if (!atomic_compare_exchange_strong(&owner, &idle, xTaskGetCurrentTaskHandle()))
        return ESP_ERR_INVALID_STATE;
    esp_err_t e = prepare();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t cfg = {.format_if_mount_failed = false, .max_files = 12};
    if (e == ESP_OK)
        e = esp_vfs_fat_sdmmc_mount(PACK_MOUNT, &host, &slot, &cfg, &card);
    if (e == ESP_OK && !identify_volume()) {
        esp_vfs_fat_sdcard_unmount(PACK_MOUNT, card);
        e = ESP_FAIL;
    }
    if (e != ESP_OK) {
        card = NULL;
        atomic_store(&owner, NULL);
    } else {
        ++mounted_volume.mount_generation;
        volume_lost = false;
    }
    return e;
}

void pack_card_close(void)
{
    if (atomic_load(&owner) != xTaskGetCurrentTaskHandle())
        return;
    if (card)
        esp_vfs_fat_sdcard_unmount(PACK_MOUNT, card);
    card = NULL;
    atomic_store(&owner, NULL);
}

// Read the physical boot sector afresh. A cached FATFS window can still contain
// the serial from a card that was removed while the worker was decoding.
static bool volume_serial(const pack_card_volume_t *v, uint32_t *serial)
{
    static uint32_t boot_words[512 / sizeof(uint32_t)];
    if (card->csd.sector_size != sizeof(boot_words) ||
        sdmmc_read_sectors(card, boot_words, v->sector, 1) != ESP_OK)
        return false;
    const uint8_t *boot = (const uint8_t *)boot_words;
    if (boot[510] != 0x55 || boot[511] != 0xaa)
        return false;
    memcpy(serial, boot + v->serial_offset, sizeof(*serial));
    return true;
}

static bool identify_volume(void)
{
    unsigned drive = ff_diskio_get_pdrv_card(card);
    if (drive >= FF_VOLUMES || drive > 9)
        return false;
    char path[] = {(char)('0' + drive), ':', 0};
    FF_DIR root;
    if (f_opendir(&root, path) != FR_OK) return false;
    FATFS *fs = root.obj.fs;
    if (f_closedir(&root) != FR_OK) return false;
    if (!fs || fs->pdrv != drive)
        return false;
    mounted_volume.sector = fs->volbase;
    switch (fs->fs_type) {
    case FS_EXFAT: mounted_volume.serial_offset = 100; break;
    case FS_FAT32: mounted_volume.serial_offset = 67; break;
    case FS_FAT16:
    case FS_FAT12: mounted_volume.serial_offset = 39; break;
    default: return false;
    }
    return volume_serial(&mounted_volume, &mounted_volume.serial);
}

bool pack_card_present(void)
{
    if (atomic_load(&owner) != xTaskGetCurrentTaskHandle() || !card || volume_lost)
        return false;
    uint32_t serial;
    if (!volume_serial(&mounted_volume, &serial) || serial != mounted_volume.serial)
        volume_lost = true;
    return !volume_lost;
}

bool pack_card_read_volume(pack_card_volume_t *volume, uint64_t *free_bytes)
{
    if (!pack_card_present()) return false;
    if (free_bytes) {
        unsigned drive = ff_diskio_get_pdrv_card(card);
        if (drive >= FF_VOLUMES || drive > 9) return false;
        char path[] = {(char)('0' + drive), ':', 0};
        DWORD free_clusters = 0;
        FATFS *fs = NULL;
        if (f_getfree(path, &free_clusters, &fs) != FR_OK || !fs || fs->pdrv != drive || !pack_card_present())
            return false;
        *free_bytes = (uint64_t)free_clusters * fs->csize * card->csd.sector_size;
    }
    *volume = mounted_volume;
    return true;
}

bool pack_card_same_volume(const pack_card_volume_t *volume)
{
    return pack_card_present() && volume->mount_generation == mounted_volume.mount_generation &&
           volume->serial == mounted_volume.serial && volume->sector == mounted_volume.sector &&
           volume->serial_offset == mounted_volume.serial_offset;
}
