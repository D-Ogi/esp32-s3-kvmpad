// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "msc_session_platform.h"
#include "screenshot_volume.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* Deterministic storage-manager scheduler. TinyUSB core, MSC BOT, HID,
 * bus selection, the esp_blockdev file ops and the backing file are real. */
static struct fixture_lun { esp_blockdev_handle_t dev; } lun;
static bool manager, delayed;
static unsigned delayed_lba, drained;
static uint8_t delayed_data[512];

esp_err_t tinyusb_msc_install_driver(const tinyusb_msc_driver_config_t *cfg)
{
    assert(!manager && cfg->user_flags.auto_mount_off);
    manager = true;
    return ESP_OK;
}
esp_err_t tinyusb_msc_new_storage_blockdev(const tinyusb_msc_storage_config_t *cfg, tinyusb_msc_storage_handle_t *out)
{
    assert(manager && !lun.dev && cfg->fat_fs.do_not_format && cfg->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB);
    lun.dev = cfg->medium.blockdev;
    assert(lun.dev->geometry.disk_size == SCREENSHOT_VOLUME_BYTES && lun.dev->geometry.write_size == 512);
    *out = &lun;
    return ESP_OK;
}
esp_err_t tinyusb_msc_delete_storage(tinyusb_msc_storage_handle_t instance)
{
    assert(instance == &lun && lun.dev);
    if (delayed) {
        assert(lun.dev->ops->write(lun.dev, delayed_data, delayed_lba * 512, sizeof(delayed_data)) == ESP_OK);
        delayed = false;
        drained++;
    }
    lun.dev = NULL;
    return ESP_OK;
}
esp_err_t tinyusb_msc_uninstall_driver(void)
{
    assert(manager && !lun.dev);
    manager = false;
    return ESP_OK;
}
bool tud_msc_test_unit_ready_cb(uint8_t n) { return !n && lun.dev; }
void tud_msc_capacity_cb(uint8_t n, uint32_t *count, uint16_t *size)
{
    assert(!n && lun.dev);
    *size = 512;
    *count = lun.dev->geometry.disk_size / *size;
}
int32_t tud_msc_read10_cb(uint8_t n, uint32_t lba, uint32_t offset, void *b, uint32_t size)
{
    assert(!n && lun.dev);
    return lun.dev->ops->read(lun.dev, b, size, (uint64_t)lba * 512 + offset, size) == ESP_OK ? (int32_t)size : -1;
}
int32_t tud_msc_write10_cb(uint8_t n, uint32_t lba, uint32_t offset, uint8_t *b, uint32_t size)
{
    assert(!n && lun.dev);
    if (lba == 63001) {
        assert(!offset && size == sizeof(delayed_data));
        delayed = true;
        delayed_lba = lba;
        memcpy(delayed_data, b, size);
        return (int32_t)size;
    }
    return lun.dev->ops->write(lun.dev, b, (uint64_t)lba * 512 + offset, size) == ESP_OK ? (int32_t)size : -1;
}
int32_t tud_msc_scsi_cb(uint8_t n, const uint8_t cmd[16], void *buf, uint16_t size)
{
    (void)n; (void)cmd; (void)buf; (void)size; return -1;
}
static void storage_command(uint8_t cmd, uint32_t lba, unsigned bytes, bool in)
{
    msc_cbw_t cbw = {.signature = MSC_CBW_SIGNATURE, .tag = 17, .cmd_len = 10, .dir = in ? 0x80 : 0,
                     .total_bytes = bytes, .command = {cmd, 0, lba>>24, lba>>16, lba>>8, lba, 0, 0, 1}};
    assert(endpoint[1].pending && endpoint[1].len == sizeof(cbw));
    memcpy(endpoint[1].data, &cbw, sizeof(cbw));
    complete(1);
}
static void storage_status(void)
{
    msc_csw_t csw;
    assert(endpoint[0x81].pending && endpoint[0x81].len == sizeof(csw));
    memcpy(&csw, endpoint[0x81].data, sizeof(csw));
    assert(csw.signature == MSC_CSW_SIGNATURE && csw.tag == 17 && !csw.status && !csw.data_residue);
    complete(0x81);
}
static bool card_inserted(void) { return true; }
static bool card_removed(void) { return false; }

static void screenshot_session(const char *path)
{
    uint8_t initial[64], desc[255], sector[512], buffer[4096];
    assert(descriptor(TUSB_DESC_DEVICE, 0, initial) == 18 && initial[10] == 0x0c && initial[11] == 0x40);
    FILE *image = fopen(path, "wb+");
    assert(image && usb_hid_device_screenshot_begin(image) == ESP_ERR_INVALID_ARG);
    assert(screenshot_volume_create_file(image, 0x12345678, buffer, sizeof(buffer)));
    assert(usb_hid_device_screenshot_begin(image) == ESP_OK);
    assert(usb_hid_device_screenshot_begin(image) == ESP_ERR_INVALID_STATE);
    assert(usb_hid_device_set_monitor(true) == ESP_ERR_INVALID_STATE);
    testing_storage = true;
    wait_for_reconnect(); reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, desc) == 18 && desc[8] == 0x3a && desc[9] == 0x30 &&
           desc[10] == 0x0a && desc[11] == 0x40);
    assert(descriptor(TUSB_DESC_CONFIGURATION, 0, desc) == 82);
    assert(desc[4] == 3 && desc[14] == TUSB_CLASS_MSC && desc[15] == 6 && desc[16] == 0x50);
    assert(desc[20] == 1 && desc[27] == 0x81 && desc[37] == TUSB_CLASS_HID && desc[39] == 1 &&
           desc[62] == TUSB_CLASS_HID && desc[64] == 2);
    configure(1);
    drain();
    assert(usb_hid_device_interface_count() == 2 && usb().state == KVM_USB_MOUNTED);
    check_first_led(1, true);
    assert(usb_hid_device_screenshot_present(card_inserted));
    setup(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_BOOT, 2, 0);
    status_stage(0x80);
    kvm_hid_intent_t move = {.type = KVM_HID_MOUSE_MOVE, .data.mouse_move = {.dx = 7, .dy = -2}};
    assert(hid_dispatch_local(&move) == ESP_OK);
    service();
    assert(endpoint[0x83].len == 3 && endpoint[0x83].data[1] == 7 && endpoint[0x83].data[2] == (uint8_t)-2);
    complete(0x83);

    setup(0xa1, MSC_REQ_GET_MAX_LUN, 0, 0, 1);
    assert(endpoint[0x80].pending && endpoint[0x80].len == 1 && !endpoint[0x80].data[0]);
    complete(0x80); status_stage(0);
    storage_command(SCSI_CMD_READ_CAPACITY_10, 0, 8, true);
    assert(endpoint[0x81].len == 8 && !memcmp(endpoint[0x81].data, (uint8_t[]){0,0,255,255,0,0,2,0}, 8));
    complete(0x81); storage_status();
    storage_command(SCSI_CMD_READ_10, 0, 512, true);
    assert(endpoint[0x81].len == 512 && !memcmp(endpoint[0x81].data + 54, "FAT16   ", 8));
    complete(0x81); storage_status();
    for (unsigned i = 0; i < 512; ++i) sector[i] = (uint8_t)(5 + i * 13);
    for (unsigned lba = 63000; lba <= 63001; ++lba) {
        storage_command(SCSI_CMD_WRITE_10, lba, 512, false);
        assert(endpoint[1].pending && endpoint[1].len == 512);
        memcpy(endpoint[1].data, sector, 512);
        complete(1); storage_status();
    }
    storage_command(SCSI_CMD_READ_10, 63000, 512, true);
    assert(endpoint[0x81].len == 512 && !memcmp(endpoint[0x81].data, sector, 512));
    complete(0x81); storage_status();
    esp_blockdev_handle_t stale = lun.dev;
    assert(stale->ops->write(stale, sector, UINT64_MAX - 511, 512) == ESP_ERR_INVALID_ARG);
    assert(usb_hid_device_screenshot_present(card_inserted) && delayed);
    kvm_hid_intent_t key = {.type = KVM_HID_KEY_DOWN, .data.key.usage_id = 4};
    assert(hid_dispatch_local(&key) == ESP_OK);
    service();
    assert(endpoint[0x82].len == 8 && endpoint[0x82].data[2] == 4 && snapshot().keys_down == 1);
    assert(usb_hid_device_screenshot_end() == ESP_OK && drained == 1 && !manager);
    assert(snapshot().ledger_empty);
    assert(!fseek(image, 63001 * 512, SEEK_SET) && fread(buffer, 1, 512, image) == 512 && !memcmp(sector, buffer, 512));
    assert(!fclose(image));
    assert(stale->ops->read(stale, buffer, 512, 0, 512) != ESP_OK);
    testing_storage = false;
    wait_for_reconnect(); reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, desc) == 18 && !memcmp(initial, desc, 18));
    configure(1); drain();
    image = fopen(path, "rb+");
    assert(image && usb_hid_device_screenshot_begin(image) == ESP_OK);
    assert(!usb_hid_device_screenshot_present(card_removed));
    assert(!usb_hid_device_screenshot_present(card_inserted));
    assert(usb_hid_device_screenshot_end() != ESP_OK && !manager && !fclose(image));
    wait_for_reconnect(); reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, desc) == 18 && !memcmp(initial, desc, 18));
    configure(1); drain();
    image = fopen(path, "rb+");
    assert(image && usb_hid_device_screenshot_begin(image) == ESP_OK);
    /* Simulate a lost tail in the isolated backing file. This is an actual
     * short fread, independent of the explicit card-presence callback. */
#ifdef _WIN32
    assert(_chsize_s(_fileno(image), 512) == 0);
#else
    assert(ftruncate(fileno(image), 512) == 0);
#endif
    assert(lun.dev->ops->read(lun.dev, buffer, 512, SCREENSHOT_VOLUME_BYTES - 512, 512) != ESP_OK);
    assert(!usb_hid_device_screenshot_present(card_inserted));
    assert(usb_hid_device_screenshot_end() != ESP_OK && !manager && !fclose(image));
    wait_for_reconnect(); reset_bus(); configure(1); drain();
    puts("PASS screenshot MSC+boot HID, GET_MAX_LUN, READ CAPACITY, FAT16 READ10/WRITE10, exclusive file, "
         "boot mouse/keyboard, queued write drained before close, stale I/O rejected, short file I/O, removal/cancel and HID restored");
}
