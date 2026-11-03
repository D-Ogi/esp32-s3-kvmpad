// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Firmware side of TASK-110: the ESP-IDF adapter around the pure state machine in
// update_manager.h, the locally triggered HTTPS image source and the console hook.
//
// There is deliberately no entry point here that a remote session can reach (TECH-045, AT-SEC-07):
// an update is started from the device console or from the local Settings UI, never from the
// CLIENT protocol. See docs/ADR/ADR-031-ota-source-and-signing.md.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "update_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the platform hooks, read the running image, register the health probes (USB stack, HID
 * scheduler, LVGL owner, touch reader) and start the update task.
 *
 * Call AFTER hid_dispatch_start(), usb_hid_device_start() and (when built) ui_platform_start(), and
 * before the console. When the running image is in ESP_OTA_IMG_PENDING_VERIFY this call blocks
 * input (maintenance) and the task runs the health check; otherwise it returns with the device idle.
 */
esp_err_t update_manager_start(void);

/**
 * Register the hook that closes the remote session before an update. Called by the integrator once
 * remote_protocol/secure_remote is linked in; without it the manager still revokes the HID lease
 * through maintenance, which is what actually takes control away.
 */
void update_manager_set_session_revoke(void (*revoke)(void *ctx), void *ctx);

/**
 * Start a locally triggered update from `url` (https:// only). Returns ESP_ERR_INVALID_STATE with
 * the refusal reason in `*out_reason` when the manager will not start.
 */
esp_err_t update_manager_begin_https(const char *url, kvm_ota_reason_t *out_reason);

/** Operator cancel; true when an attempt was actually cancelled. */
bool update_manager_cancel(void);

/** Confirm the running image by hand (`ota mark-valid`) while the health check is pending. */
esp_err_t update_manager_mark_valid(void);

/** Snapshot for the console, the status service and the "Updating" screen. */
void update_manager_get_status(kvm_ota_status_t *out);

/** Partition labels and the OTA state of the running slot, for the status line. */
typedef struct {
    const char *running_label;
    const char *next_label;
    kvm_ota_img_state_t running_state;
    size_t next_size;
    bool signature_checked_on_update; // CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT / SECURE_BOOT
    bool rollback_enabled;            // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
} update_manager_slots_t;

void update_manager_get_slots(update_manager_slots_t *out);

/**
 * The rollback that led to this boot, if any (docs/OTA.md row 4): the image that failed records its reason and the
 * mask of the required probes that never reported alive in RTC memory that survives the software reset (no flash
 * write on the failure path). This boot takes the record over, so `ota status` shows `last reason` and the probe
 * names (comma-separated into `names`, may be NULL). False when this boot did not follow a recorded rollback.
 */
bool update_manager_last_rollback(kvm_ota_reason_t *reason, uint32_t *missing_mask, char *names, size_t cap);

/**
 * Console command `ota`, to be registered by the integrator in main/kvm_console.c:
 *
 *   {.command = "ota",
 *    .help = "ota [status | url <https-url> | cancel | mark-valid]: signed update (TASK-110)",
 *    .func = update_manager_console_cmd},
 *
 * Subcommands: `status` (default), `url <https-url>`, `cancel`, `mark-valid`. It never flashes
 * anything by itself and prints no URL credentials.
 */
int update_manager_console_cmd(int argc, char **argv);

#ifdef __cplusplus
}
#endif
