// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// KVMpad bring-up image for M0-M2: board HAL, USB HID device (enumeration only), RGB LCD test
// card, GT911 touch diagnostics, UART console. No HID input is generated in this image.
#include <inttypes.h>
#include <stdio.h>

#include "app_config.h"
#include "board_hal.h"
#include "board_lcd.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "kvm_console.h"
#include "kvm_diag.h"
#include "kvm_notice.h"
#include "net_status.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "touch_input.h"
#include "hid_dispatch.h"
#include "remote_glue.h"
#include "update_manager_esp.h"
#if KVM_OTA_TEST_CERT
#include "update_transport.h"
#endif
#include "usb_hid_device.h"
#if CONFIG_KVM_UI_PLATFORM
#include "ime_glue.h"
#include "ui_glue.h"
#include "text_glue.h"
#include "ui_shell.h"
#include "ui_platform.h"
#if CONFIG_KVM_UI_BENCH
#include "ui_bench.h"
#endif
#endif

static const char *TAG = "kvmpad";
static uint32_t s_boot_count;

uint32_t kvm_boot_count(void)
{
    return s_boot_count;
}

static void nvs_boot_counter(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition reformatted (%s)", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init: %s", esp_err_to_name(err));
        return;
    }
    nvs_handle_t h;
    if (nvs_open("kvm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot_count", &s_boot_count);
        s_boot_count++;
        nvs_set_u32(h, "boot_count", s_boot_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void banner(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s", kvm_notice());
    ESP_LOGI(TAG, "app %s %s (IDF %s) chip rev v%d.%d flash %" PRIu32 " MiB psram %u KiB boot #%" PRIu32
                  " reset_reason %d",
             app->project_name, app->version, app->idf_ver, ci.revision / 100, ci.revision % 100, flash >> 20,
             (unsigned)(esp_psram_get_size() >> 10), s_boot_count, (int)esp_reset_reason());
}

void app_main(void)
{
    // 1. Expander safe state with EXIO5=LOW (USB) confirmed by read-back — before anything else.
    esp_err_t err = board_hal_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board_hal_init: %s - USB stack NOT started (EXIO5 unconfirmed)", esp_err_to_name(err));
    }

    // 2. USB device immediately after the mux (TECH-030); the HID core is installed first so the
    //    very first mount already goes through the scheduler (neutral start, TECH-035).
    if (err == ESP_OK) {
        esp_err_t herr = hid_dispatch_start();
        if (herr != ESP_OK) {
            ESP_LOGE(TAG, "hid_dispatch_start: %s", esp_err_to_name(herr));
        }
        esp_err_t terr = text_glue_start(); // ADR-024 FR-748: composed text -> one burst per item
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "text_glue_start: %s", esp_err_to_name(terr));
        }
        esp_err_t uerr = usb_hid_device_start();
        if (uerr != ESP_OK) {
            ESP_LOGE(TAG, "usb_hid_device_start: %s", esp_err_to_name(uerr));
        }
    }

    nvs_boot_counter();
    // Settings record (TASK-090): loaded once NVS is up; a missing or damaged record means defaults, never a halt.
    esp_err_t cerr = app_config_start();
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "app_config_start: %s", esp_err_to_name(cerr));
    }
    {
        // ADR-020 keep-awake from the saved record (default OFF); Settings re-applies it on every change.
        static app_config_settings_t ka;
        if (app_config_settings(&ka, 100)) {
            (void)hid_dispatch_set_keepalive(ka.keepawake_enabled, ka.keepawake_interval_s);
        }
    }
    banner();

    // 3. LCD: reset, panel, then either the LVGL platform (TASK-060A) or the M1 test card; backlight last.
    board_lcd_cfg_t lcd_cfg = BOARD_LCD_CFG_DEFAULT;
#if CONFIG_KVM_UI_PLATFORM
    lcd_cfg.num_fbs = ui_platform_required_fbs();
    lcd_cfg.bounce_lines = ui_platform_bounce_lines();
#endif
    esp_err_t lerr = err == ESP_OK ? board_lcd_init(&lcd_cfg) : ESP_ERR_INVALID_STATE;
    if (lerr == ESP_OK) {
        diag_start(); // touch-sample consumer for `touch capture`; it draws only on an explicit `lcd touch`
#if CONFIG_KVM_UI_PLATFORM
        esp_err_t uerr = ui_platform_start();
        if (uerr != ESP_OK) {
            ESP_LOGE(TAG, "ui_platform_start: %s", esp_err_to_name(uerr));
        } else {
            esp_err_t gerr = ui_glue_start(); // TASK-060B: domain snapshot -> mailbox -> LVGL Subjects
            if (gerr != ESP_OK) {
                ESP_LOGE(TAG, "ui_glue_start: %s", esp_err_to_name(gerr));
            }
        }
        if (uerr == ESP_OK) {
            // TASK-070A: the product shell (router + MOUSE view); pad gestures and its buttons reach the HOST.
            esp_err_t serr = ui_shell_show();
            if (serr != ESP_OK) {
                ESP_LOGE(TAG, "ui_shell_show: %s", esp_err_to_name(serr));
            }
            // TASK-072: the pinyin decoder and the hanzi font load in the background; the ZH layer enables itself.
            esp_err_t ierr = ime_glue_start();
            if (ierr != ESP_OK) {
                ESP_LOGE(TAG, "ime_glue_start: %s", esp_err_to_name(ierr));
            }
        }
#else
        diag_show(DIAG_SCREEN_CARD);
#endif
        board_hal_backlight(true);
    } else {
        ESP_LOGE(TAG, "LCD init: %s", esp_err_to_name(lerr));
    }

    // 4. Touch: the single GT911 reader.
    if (err == ESP_OK) {
#if KVM_OTA_TEST_NO_TOUCH
        // Development-only (main/CMakeLists.txt): an image whose health check must fail (docs/OTA.md row 4).
        ESP_LOGE(TAG, "OTA TEST IMAGE: touch reader not started - the health check will roll this image back");
#else
        esp_err_t terr = touch_input_start();
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "touch_input_start: %s", esp_err_to_name(terr));
        }
#endif
#if KVM_OTA_TEST_CERT
        // Development-only (main/CMakeLists.txt): the LAN test server's self-signed certificate is pinned.
        extern const char ota_test_cert_pem_start[] asm("_binary_ota_test_cert_pem_start");
        update_transport_https_set_cert(ota_test_cert_pem_start);
        ESP_LOGW(TAG, "OTA TEST BUILD: a development server certificate is pinned for `ota url`");
#endif
        // 4b. Update manager (TASK-110, ADR-031): after every health probe's subject has started. A fresh OTA image
        //     (PENDING_VERIFY) stays in maintenance until the health check confirms it or rolls it back.
        esp_err_t oerr = update_manager_start();
        if (oerr != ESP_OK) {
            ESP_LOGE(TAG, "update_manager_start: %s", esp_err_to_name(oerr));
        }
    }

    // 5. Network status (TASK-080, ADR-015): last, so local HID, touch and UI never wait for it. The radio stays
    //    off while no network is configured.
    const net_status_cfg_t ncfg = {.cache = app_config_net_cache_hooks(), // ADR-017 BSSID/channel record
                                   .power_save_off = true};             // TECH-070
    esp_err_t nerr = net_status_start(&ncfg);
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "net_status_start: %s", esp_err_to_name(nerr));
    }

    // 6. Remote transport (TASK-100, ADR-032): WSS on the LAN, only after USB, HID, touch and the UI. It admits
    //    nothing while Settings -> Remote is off (the default), and an OTA closes every session first.
    esp_err_t rerr = remote_glue_start();
    if (rerr == ESP_OK) {
        update_manager_set_session_revoke(remote_glue_drop_all_for_maintenance, NULL);
    } else {
        ESP_LOGE(TAG, "remote_glue_start: %s", esp_err_to_name(rerr));
    }

    uint8_t sh = 0;
    uint8_t rd = 0;
    bool match = false;
    if (err == ESP_OK && board_hal_exio_verify(&sh, &rd, &match) == ESP_OK) {
        ESP_LOGI(TAG, "EXIO after init: shadow=0x%02X readback=0x%02X match=%d USB_SEL=%d", sh, rd, match,
                 (rd >> 5) & 1);
    }
    ESP_LOGI(TAG, "BOOT_OK boot #%" PRIu32, s_boot_count);

    // 7. Console (UART0 via the CH343 bridge).
    kvm_console_start();
}
