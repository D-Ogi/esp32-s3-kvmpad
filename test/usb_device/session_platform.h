// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "../live_view/hid_test_platform.h"
#include "tusb.h"

#define CONFIG_KVM_PTP_WIDTH_TENTH_MM 0
#define CONFIG_KVM_PTP_HEIGHT_TENTH_MM 0
#define ESP_LOGE ESP_LOGI
typedef void *TaskHandle_t;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { static int task; return &task; }
static inline esp_err_t esp_efuse_mac_get_default(uint8_t *mac) { memset(mac, 0, 6); return ESP_OK; }
typedef struct { uint8_t app_elf_sha256[32]; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void) { static esp_app_desc_t desc; return &desc; }
uint32_t esp_random(void);

enum { TINYUSB_EVENT_ATTACHED, TINYUSB_EVENT_DETACHED, TINYUSB_EVENT_SUSPENDED, TINYUSB_EVENT_RESUMED };
typedef struct { unsigned id; struct { bool remote_wakeup; } suspended; } tinyusb_event_t;
typedef struct {
    void (*event)(tinyusb_event_t *, void *);
    struct {
        const tusb_desc_device_t *device;
        const char **string;
        unsigned string_count;
        const uint8_t *full_speed_config;
    } descriptor;
} tinyusb_config_t;
#define TINYUSB_DEFAULT_CONFIG(cb, arg) { .event = (cb) }
esp_err_t tinyusb_driver_install(const tinyusb_config_t *cfg);
#if CONFIG_KVM_USB_MONITOR_EVAL
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t us);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
#endif

static inline bool wait_for_dispatch(SemaphoreHandle_t s, unsigned wait)
{
    if (!*s && wait) tud_task_ext(0, false);
    return xSemaphoreTake(s, wait);
}
#define xSemaphoreTake wait_for_dispatch
