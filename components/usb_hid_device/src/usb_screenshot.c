// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "usb_hid_device.h"
#include "usb_monitor_port.h"
#include "screenshot_volume.h"
#include "esp_blockdev.h"
#include "tinyusb_msc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include <fcntl.h>
#include <unistd.h>
#endif

#if !CONFIG_TINYUSB_MSC_ASYNC_IO
#error "Screenshot MSC needs the pinned asynchronous I/O drain."
#endif

/* begin/end/present have one external owner, the card worker. The managed
 * MSC worker also holds this mutex whenever it can access the backing file. */
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static screenshot_volume_io_t volume;
static tinyusb_msc_storage_handle_t storage;
static bool media_failed;

static bool valid_blocks(uint64_t address, size_t bytes)
{
    return bytes && address < SCREENSHOT_VOLUME_BYTES && bytes <= SCREENSHOT_VOLUME_BYTES - address &&
           !(address % SCREENSHOT_SECTOR_BYTES) && !(bytes % SCREENSHOT_SECTOR_BYTES);
}

static esp_err_t file_read(esp_blockdev_handle_t dev, uint8_t *data, size_t cap, uint64_t address, size_t bytes)
{
    (void)dev;
    if (!data || cap < bytes || !valid_blocks(address, bytes)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok = volume.context && !media_failed && screenshot_volume_read(&volume,
        (uint32_t)(address / SCREENSHOT_SECTOR_BYTES), bytes / SCREENSHOT_SECTOR_BYTES, data);
    if (!ok) media_failed = true;
    xSemaphoreGive(mutex);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t file_write(esp_blockdev_handle_t dev, const uint8_t *data, uint64_t address, size_t bytes)
{
    (void)dev;
    if (!data || !valid_blocks(address, bytes)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok = volume.context && !media_failed && screenshot_volume_write(&volume,
        (uint32_t)(address / SCREENSHOT_SECTOR_BYTES), bytes / SCREENSHOT_SECTOR_BYTES, data) &&
        screenshot_volume_sync(&volume);
    if (!ok) media_failed = true;
    xSemaphoreGive(mutex);
    return ok ? ESP_OK : ESP_FAIL;
}

static const esp_blockdev_ops_t file_ops = {.read = file_read, .write = file_write};
static esp_blockdev_t file_device = {
    .ops = &file_ops,
    .geometry = {.disk_size = SCREENSHOT_VOLUME_BYTES, .read_size = SCREENSHOT_SECTOR_BYTES,
                 .write_size = SCREENSHOT_SECTOR_BYTES, .erase_size = 1},
};

/* Screenshot storage is the sole MSC personality. In particular USB events
 * for normal HID must never mount inside the image or race its destruction. */
void __wrap_msc_storage_mount_to_app(void) {}
void __wrap_msc_storage_mount_to_usb(void) {}

bool usb_hid_device_screenshot_present(bool (*card_present)(void))
{
    if (!storage || !card_present) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool present = volume.context && !media_failed && card_present();
    if (!present) media_failed = true;
    xSemaphoreGive(mutex);
    return present;
}

static esp_err_t release_storage(void)
{
    /* With ASYNC_IO this unmaps the LUN then drains all counted requests.
     * Even a managed-driver error cannot leave a caller-owned FILE reachable. */
    esp_err_t result = tinyusb_msc_delete_storage(storage);
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool flushed = volume.context && screenshot_volume_sync(&volume);
    volume = (screenshot_volume_io_t){0};
    bool valid = flushed && !media_failed;
    xSemaphoreGive(mutex);
    if (result != ESP_OK) return result;
    storage = NULL;
    result = tinyusb_msc_uninstall_driver();
    return result == ESP_OK && !valid ? ESP_FAIL : result;
}

esp_err_t usb_hid_device_screenshot_begin(FILE *image)
{
    if (storage) return ESP_ERR_INVALID_STATE;
    if (!image) return ESP_ERR_INVALID_ARG;
#ifdef ESP_PLATFORM
    /* An append-mode stream ignores the LBA on write. IDF FatFs implements
     * F_GETFL for file descriptors and retains the original access mode. */
    int flags = fcntl(fileno(image), F_GETFL, 0);
    if (flags < 0 || (flags & (O_ACCMODE | O_APPEND)) != O_RDWR) return ESP_ERR_INVALID_ARG;
#endif
    screenshot_volume_io_t candidate;
    if (!screenshot_volume_open_file(image, &candidate)) return ESP_ERR_INVALID_ARG;
    if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    const tinyusb_msc_driver_config_t driver = {.user_flags.auto_mount_off = 1};
    esp_err_t result = tinyusb_msc_install_driver(&driver);
    if (result != ESP_OK) return result;
    volume = candidate;
    media_failed = false;
    const tinyusb_msc_storage_config_t config = {
        .medium.blockdev = &file_device, .fat_fs.do_not_format = true, .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    result = tinyusb_msc_new_storage_blockdev(&config, &storage);
    if (result != ESP_OK) {
        volume = (screenshot_volume_io_t){0};
        tinyusb_msc_uninstall_driver();
        return result;
    }
    result = usb_monitor_port_screenshot(true);
    if (result != ESP_OK) release_storage();
    return result;
}

esp_err_t usb_hid_device_screenshot_end(void)
{
    if (!storage) return ESP_ERR_INVALID_STATE;
    esp_err_t bus = usb_monitor_port_screenshot(false);
    esp_err_t result = release_storage();
    return result == ESP_OK ? bus : result;
}
