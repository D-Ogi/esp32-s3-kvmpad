// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "pack_card_bus.h"
#include "pack_card.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#ifdef KVM_CARD_WRITE_LABEL
bool card_ram_roundtrip(void);
#endif

// The installed firmware has already configured the independent expander.
// Observe the two required pins without resetting the display or changing its outputs.
static esp_err_t verify_card_pins(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    const i2c_master_bus_config_t cfg = {.i2c_port = BOARD_I2C_PORT,
                                         .sda_io_num = BOARD_I2C_SDA_GPIO,
                                         .scl_io_num = BOARD_I2C_SCL_GPIO,
                                         .clk_source = I2C_CLK_SRC_DEFAULT,
                                         .glitch_ignore_cnt = 7,
                                         .flags.enable_internal_pullup = true};
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BOARD_EXP_ADDR, .scl_speed_hz = BOARD_I2C_FREQ_HZ};
    esp_err_t e = i2c_new_master_bus(&cfg, &bus);
    if (e == ESP_OK)
        e = i2c_master_bus_add_device(bus, &device, &dev);
    uint8_t reg = BOARD_EXP_REG_INPUT, pins = 0;
    if (e == ESP_OK)
        e = i2c_master_transmit_receive(dev, &reg, 1, &pins, 1, BOARD_I2C_TIMEOUT_MS);
    if (e == ESP_OK && (!(pins & (1u << BOARD_EXIO_SD_CS)) || (pins & (1u << BOARD_EXIO_USB_SEL))))
        e = ESP_ERR_INVALID_STATE;
    if (dev)
        i2c_master_bus_rm_device(dev);
    if (bus)
        i2c_del_master_bus(bus);
    printf("CARD_PINS status=%s sd_dat3=%u usb_sel=%u\n", esp_err_to_name(e),
           (unsigned)((pins >> BOARD_EXIO_SD_CS) & 1u), (unsigned)((pins >> BOARD_EXIO_USB_SEL) & 1u));
    return e;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t e = pack_card_mount_on_bus(verify_card_pins);
    if (e != ESP_OK) {
        printf("CARD_RAM mount=%s\n", esp_err_to_name(e));
        return;
    }
    // Read only. Avoid enumerating user files. Drive zero is the sole mounted FAT volume.
    char label[64] = {0};
    FRESULT name = f_getlabel("0:", label, NULL);
    FATFS *fs = NULL;
    DWORD clusters = 0;
    FRESULT info = f_getfree("0:", &clusters, &fs);
    bool exfat = info == FR_OK && fs && fs->fs_type == FS_EXFAT;
    bool ready = pack_card_present();
#ifdef KVM_CARD_WRITE_LABEL
    bool authorized_volume = ready && exfat && name == FR_OK && strcmp(label, KVM_CARD_WRITE_LABEL) == 0;
    bool written = authorized_volume && card_ram_roundtrip();
    printf("CARD_WRITE label_match=%d roundtrip=%d\n", (int)authorized_volume, (int)written);
#endif
    pack_card_close();
    printf("CARD_RAM mount=OK status=%d exfat=%d label=%s label_result=%d info_result=%d unmounted=1\n", (int)ready,
           (int)exfat, label, (int)name, (int)info);
}
