// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ESP-IDF adapter for the TASK-110 OTA state machine: the real esp_ota_* calls, the HID
// maintenance interlock, the health probes and the update task.
//
// Nothing here decides policy — the state machine in update_manager.c does. This file only turns
// ESP-IDF results into the three slot codes and keeps the layout assumptions honest with the
// static assertions below.
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h" // ESP_ERR_IMAGE_INVALID
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "hid_dispatch.h"
#include "sdkconfig.h"
#include "touch_input.h"
#include "update_manager_esp.h"
#include "update_transport.h"
#include "usb_hid_device.h"
#if CONFIG_KVM_UI_PLATFORM
#include "ui_platform.h"
#endif

static const char *TAG = "ota";

// ---- the layout contract of update_image.h, checked against the pinned ESP-IDF headers ---------
// If any of these fails, do not "fix" the number: the image format changed and the parser in
// update_image.c has to be revisited.
ESP_STATIC_ASSERT(sizeof(esp_image_header_t) == KVM_OTA_IMAGE_HEADER_LEN, "image header size");
ESP_STATIC_ASSERT(sizeof(esp_image_segment_header_t) == KVM_OTA_SEGMENT_HEADER_LEN, "segment header size");
ESP_STATIC_ASSERT(sizeof(esp_app_desc_t) == KVM_OTA_APP_DESC_LEN, "app desc size");
ESP_STATIC_ASSERT(KVM_OTA_APP_DESC_OFF == sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                  "app desc offset: esp_ota_get_partition_description() reads it here");
ESP_STATIC_ASSERT(offsetof(esp_image_header_t, magic) == 0, "magic offset");
ESP_STATIC_ASSERT(offsetof(esp_image_header_t, segment_count) == 1, "segment_count offset");
ESP_STATIC_ASSERT(offsetof(esp_image_header_t, chip_id) == 12, "chip_id offset");
ESP_STATIC_ASSERT(offsetof(esp_image_header_t, min_chip_rev_full) == 15, "min_chip_rev_full offset");
ESP_STATIC_ASSERT(offsetof(esp_image_header_t, max_chip_rev_full) == 17, "max_chip_rev_full offset");
ESP_STATIC_ASSERT(offsetof(esp_image_header_t, hash_appended) == 23, "hash_appended offset");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, secure_version) == 4, "secure_version offset");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, version) == 16, "version offset");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, project_name) == 48, "project_name offset");
ESP_STATIC_ASSERT(offsetof(esp_app_desc_t, idf_ver) == 112, "idf_ver offset");
ESP_STATIC_ASSERT(sizeof(((esp_app_desc_t *)0)->version) == KVM_OTA_STR_LEN, "version field width");
ESP_STATIC_ASSERT(ESP_IMAGE_HEADER_MAGIC == KVM_OTA_IMAGE_MAGIC, "image magic");
ESP_STATIC_ASSERT(ESP_APP_DESC_MAGIC_WORD == KVM_OTA_APP_DESC_MAGIC, "app desc magic");
ESP_STATIC_ASSERT(ESP_CHIP_ID_ESP32S3 == KVM_OTA_CHIP_ID_ESP32S3, "chip id");
ESP_STATIC_ASSERT((int)ESP_OTA_IMG_NEW == (int)KVM_OTA_IMG_STATE_NEW, "ota state NEW");
ESP_STATIC_ASSERT((int)ESP_OTA_IMG_PENDING_VERIFY == (int)KVM_OTA_IMG_STATE_PENDING_VERIFY, "ota state PENDING");
ESP_STATIC_ASSERT((int)ESP_OTA_IMG_VALID == (int)KVM_OTA_IMG_STATE_VALID, "ota state VALID");
ESP_STATIC_ASSERT((int)ESP_OTA_IMG_INVALID == (int)KVM_OTA_IMG_STATE_INVALID, "ota state INVALID");
ESP_STATIC_ASSERT((int)ESP_OTA_IMG_ABORTED == (int)KVM_OTA_IMG_STATE_ABORTED, "ota state ABORTED");
ESP_STATIC_ASSERT((uint32_t)ESP_OTA_IMG_UNDEFINED == (uint32_t)KVM_OTA_IMG_STATE_UNDEFINED, "ota state UNDEF");

// The build must actually verify signatures on update (ADR-031 lists the sdkconfig lines). A
// bring-up build without them still compiles, loudly: an ESP_LOGE at every startup and
// `signature_checked_on_update = false` in the status, which the release checklist reads. It is a
// runtime error rather than a #warning because this project builds with warnings as errors, and a
// missing release setting must not block a bring-up build.
#if !defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) && !defined(CONFIG_SECURE_BOOT)
#define KVM_OTA_UNSIGNED_BUILD 1
#else
#define KVM_OTA_UNSIGNED_BUILD 0
#endif

#if defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#error "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK burns the eFuse secure-version field. AGENTS.md prohibition 6: eFuses are the operator's decision. The anti-rollback check lives in update_image.c instead."
#endif

#define UPDATE_TASK_STACK 8192 // TLS client handshake + RSA chain check run in this task
#define UPDATE_TASK_PRIO 4
#define POLL_ACTIVE_MS 5    // while an attempt is running: 4 KiB per poll is far above the link
#define POLL_IDLE_MS 100    // idle and during the health check

// ---- state ------------------------------------------------------------------------------------

static kvm_ota_t s_mgr;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_mem;
static StaticTask_t s_task_mem;
static StackType_t s_task_stack[UPDATE_TASK_STACK / sizeof(StackType_t)];
static bool s_started;

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static const esp_partition_t *s_running;

static void (*s_revoke)(void *ctx);
static void *s_revoke_ctx;

static kvm_ota_source_t s_source;
static char s_url[KVM_OTA_URL_MAX];

static inline void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static inline void unlock(void)
{
    xSemaphoreGive(s_lock);
}

// ---- heartbeat probes -------------------------------------------------------------------------
// A probe answers "is this subsystem still running", not "did it run once". Each one watches a
// counter that advances on its own and reports dead when the counter stops moving.

// The counter must move after the first sample (kvm_ota_heartbeat_alive, update_health.c): a frozen counter is dead.
#define heartbeat_alive kvm_ota_heartbeat_alive
typedef kvm_ota_heartbeat_t heartbeat_t;

static heartbeat_t s_hb_hid = {.stale_us = 1000000u};   // hid_dispatch ticks every 10 ms
static heartbeat_t s_hb_touch = {.stale_us = 3000000u}; // GT911 sanity read every 100 ms when idle
#if CONFIG_KVM_UI_PLATFORM
static heartbeat_t s_hb_lvgl = {.stale_us = 2000000u};  // LVGL refresh period is 16 ms
#endif

// USB: a state check, not a heartbeat. The TinyUSB pump only runs when there is HID work, and a
// powered-down HOST legitimately leaves the device DETACHED, so a stalled pump counter would be a
// false rollback. What must hold is that the stack is installed and not in its error state; the
// HID scheduler probe covers the task that feeds it.
static bool probe_usb(void *ctx)
{
    (void)ctx;
    usb_hid_status_t st;
    usb_hid_device_get_status(&st);
    return st.state != KVM_USB_UNINITIALIZED && st.state != KVM_USB_ERROR;
}

static bool probe_hid(void *ctx)
{
    (void)ctx;
    hid_dispatch_stats_t st;
    hid_dispatch_get_stats(&st);
    return heartbeat_alive(&s_hb_hid, st.pumps, (uint64_t)esp_timer_get_time());
}

static bool probe_touch(void *ctx)
{
    (void)ctx;
    touch_input_stats_t st;
    touch_input_get_stats(&st);
    return heartbeat_alive(&s_hb_touch, st.reads, (uint64_t)esp_timer_get_time());
}

#if CONFIG_KVM_UI_PLATFORM
static bool probe_lvgl(void *ctx)
{
    (void)ctx;
    if (!ui_platform_is_running()) {
        return false;
    }
    ui_platform_metrics_t m;
    ui_platform_metrics_get(&m);
    return heartbeat_alive(&s_hb_lvgl, m.refr_cycles, (uint64_t)esp_timer_get_time());
}
#endif

// ---- platform hooks ---------------------------------------------------------------------------

static uint64_t pl_now(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time();
}

static void pl_maintenance(void *ctx, bool on)
{
    (void)ctx;
    // hid_dispatch_set_maintenance() revokes the remote lease, performs RELEASE_ALL with
    // KVM_RA_MAINTENANCE and rejects every intent while it is raised (TECH-053).
    esp_err_t err = hid_dispatch_set_maintenance(on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "maintenance %d: %s", (int)on, esp_err_to_name(err));
    }
}

static bool pl_hid_neutral(void *ctx)
{
    (void)ctx;
    kvm_hid_snapshot_t snap;
    uint32_t seq = 0u;
    hid_dispatch_get_snapshot(&snap, &seq);
    return snap.ledger_empty && !snap.chord_active;
}

static void pl_revoke(void *ctx)
{
    (void)ctx;
    if (s_revoke != NULL) {
        s_revoke(s_revoke_ctx);
    }
}

static void pl_progress(void *ctx, kvm_ota_state_t state, uint8_t percent)
{
    (void)ctx;
    // The log line is the "Updating" state for now; the UI subject is wired by the integrator.
    ESP_LOGI(TAG, "%s %u%%", kvm_ota_state_name(state), (unsigned)percent);
}

static size_t pl_slot_size(void *ctx)
{
    (void)ctx;
    const esp_partition_t *p = s_target != NULL ? s_target : esp_ota_get_next_update_partition(NULL);
    return p != NULL ? p->size : 0u;
}

static kvm_ota_slot_rc_t pl_slot_begin(void *ctx, size_t image_size)
{
    (void)ctx;
    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL) {
        ESP_LOGE(TAG, "no inactive OTA slot in the partition table");
        return KVM_OTA_SLOT_ERROR;
    }
    esp_err_t err = esp_ota_begin(s_target, image_size, &s_handle);
    if (err != ESP_OK) {
        // ESP_ERR_OTA_ROLLBACK_INVALID_STATE here means the running image is still PENDING_VERIFY;
        // the state machine refuses that earlier, so this is a real flash or argument failure.
        ESP_LOGE(TAG, "esp_ota_begin(%s, %u): %s", s_target->label, (unsigned)image_size,
                 esp_err_to_name(err));
        s_handle = 0;
        return KVM_OTA_SLOT_ERROR;
    }
    ESP_LOGI(TAG, "writing %u bytes to %s", (unsigned)image_size, s_target->label);
    return KVM_OTA_SLOT_OK;
}

static kvm_ota_slot_rc_t pl_slot_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write(%u): %s", (unsigned)len, esp_err_to_name(err));
        return KVM_OTA_SLOT_ERROR;
    }
    return KVM_OTA_SLOT_OK;
}

static kvm_ota_slot_rc_t pl_slot_end(void *ctx)
{
    (void)ctx;
    // esp_ota_end() runs esp_image_verify(), which checks the appended secure boot v2 signature
    // block against the running app's trusted key digest when signature verification on update is
    // configured (ADR-031). The handle is consumed whatever it returns.
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err == ESP_OK) {
        return KVM_OTA_SLOT_OK;
    }
    ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
    if (err == ESP_ERR_OTA_VALIDATE_FAILED || err == ESP_ERR_IMAGE_INVALID) {
        return KVM_OTA_SLOT_INVALID;
    }
    return KVM_OTA_SLOT_ERROR;
}

static void pl_slot_abort(void *ctx)
{
    (void)ctx;
    if (s_handle != 0) {
        esp_err_t err = esp_ota_abort(s_handle);
        s_handle = 0;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_ota_abort: %s", esp_err_to_name(err));
        }
    }
    // The boot partition was never switched, so the running slot is still the one that boots.
}

static kvm_ota_slot_rc_t pl_slot_read(void *ctx, size_t offset, void *buf, size_t len)
{
    (void)ctx;
    if (s_target == NULL) {
        return KVM_OTA_SLOT_ERROR;
    }
    esp_err_t err = esp_partition_read(s_target, offset, buf, len);
    return err == ESP_OK ? KVM_OTA_SLOT_OK : KVM_OTA_SLOT_ERROR;
}

static kvm_ota_slot_rc_t pl_slot_set_boot(void *ctx)
{
    (void)ctx;
    if (s_target == NULL) {
        return KVM_OTA_SLOT_ERROR;
    }
    esp_err_t err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s): %s", s_target->label, esp_err_to_name(err));
        return KVM_OTA_SLOT_ERROR;
    }
    ESP_LOGW(TAG, "boot partition switched to %s", s_target->label);
    return KVM_OTA_SLOT_OK;
}

/** Fill the info record from the running partition's own bytes, through the same parser. */
static void pl_running_image(void *ctx, kvm_ota_image_info_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (s_running == NULL) {
        return;
    }
    uint8_t prefix[KVM_OTA_IMAGE_PROBE_BYTES];
    if (esp_partition_read(s_running, 0u, prefix, sizeof prefix) != ESP_OK) {
        ESP_LOGE(TAG, "cannot read the running image header");
        return;
    }
    kvm_ota_image_parse(prefix, sizeof prefix, out);
}

static uint16_t pl_chip_rev(void *ctx)
{
    (void)ctx;
    return (uint16_t)efuse_hal_chip_revision(); // major * 100 + minor, as in the image header
}

static kvm_ota_slot_rc_t pl_mark_valid(void *ctx)
{
    (void)ctx;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
        return KVM_OTA_SLOT_ERROR;
    }
    ESP_LOGI(TAG, "image confirmed");
    return KVM_OTA_SLOT_OK;
}

// ---- rollback record (docs/OTA.md row 4) ------------------------------------------------------
// RTC slow memory that is not initialized at boot survives the software reset of the rollback, so the image that
// boots next can say why it is running again - without a flash write on the failure path. A power cut loses it,
// which only loses the explanation, never the rollback itself.
#define ROLLBACK_MAGIC 0x4B524231u // "KRB1"

typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t missing;
    uint32_t check;
} rollback_record_t;

static RTC_NOINIT_ATTR rollback_record_t s_rb_rec;
static bool s_rb_valid;
static kvm_ota_reason_t s_rb_reason;
static uint32_t s_rb_missing;

static uint32_t rb_check(const rollback_record_t *r)
{
    return ~(r->magic ^ (r->reason * 0x9E3779B1u) ^ (r->missing * 0x85EBCA77u));
}

static void rb_take_over(void)
{
    rollback_record_t r = s_rb_rec;
    memset(&s_rb_rec, 0, sizeof s_rb_rec);
    const esp_reset_reason_t why = esp_reset_reason();
    // only after a software restart (or the RTC watchdog backstop of one); never trust stale RTC bytes after power-on
    if (r.magic != ROLLBACK_MAGIC || r.check != rb_check(&r) || r.reason >= 64u ||
        (why != ESP_RST_SW && why != ESP_RST_WDT && why != ESP_RST_PANIC)) {
        return;
    }
    s_rb_valid = true;
    s_rb_reason = (kvm_ota_reason_t)r.reason;
    s_rb_missing = r.missing;
}

static void pl_rollback(void *ctx)
{
    (void)ctx;
    rollback_record_t r = {.magic = ROLLBACK_MAGIC,
                           .reason = (uint32_t)s_mgr.reason,
                           .missing = kvm_ota_health_missing(&s_mgr.health)};
    r.check = rb_check(&r);
    s_rb_rec = r;
    ESP_LOGE(TAG, "health check failed (missing 0x%" PRIx32 ") - rolling back to the previous slot", r.missing);
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot(); // does not return on success
    ESP_LOGE(TAG, "rollback refused: %s - restarting anyway", esp_err_to_name(err));
    esp_restart();
}

static void pl_reboot(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "restarting into the new image");
    esp_restart();
}

static const kvm_ota_platform_t s_platform = {
    .ctx = NULL,
    .now_us = pl_now,
    .maintenance = pl_maintenance,
    .hid_neutral = pl_hid_neutral,
    .revoke_sessions = pl_revoke,
    .progress = pl_progress,
    .slot_size = pl_slot_size,
    .slot_begin = pl_slot_begin,
    .slot_write = pl_slot_write,
    .slot_end = pl_slot_end,
    .slot_abort = pl_slot_abort,
    .slot_read = pl_slot_read,
    .slot_set_boot = pl_slot_set_boot,
    .running_image = pl_running_image,
    .chip_rev_full = pl_chip_rev,
    .mark_valid = pl_mark_valid,
    .rollback_and_reboot = pl_rollback,
    .reboot = pl_reboot,
};

// ---- task -------------------------------------------------------------------------------------

static bool attempt_active(kvm_ota_state_t s)
{
    switch (s) {
    case KVM_OTA_PREPARE:
    case KVM_OTA_RECEIVE:
    case KVM_OTA_VERIFY:
    case KVM_OTA_SET_BOOT:
    case KVM_OTA_REBOOT:
        return true;
    default:
        return false;
    }
}

static void update_task(void *arg)
{
    (void)arg;
    for (;;) {
        lock();
        kvm_ota_poll(&s_mgr, (uint64_t)esp_timer_get_time());
        kvm_ota_state_t st = s_mgr.state;
        unlock();
        vTaskDelay(pdMS_TO_TICKS(attempt_active(st) ? POLL_ACTIVE_MS : POLL_IDLE_MS));
    }
}

// ---- public API -------------------------------------------------------------------------------

void update_manager_set_session_revoke(void (*revoke)(void *ctx), void *ctx)
{
    s_revoke = revoke;
    s_revoke_ctx = ctx;
}

esp_err_t update_manager_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_running = esp_ota_get_running_partition();
    if (s_running == NULL) {
        ESP_LOGE(TAG, "cannot identify the running partition");
        return ESP_ERR_NOT_FOUND;
    }
    s_target = esp_ota_get_next_update_partition(NULL);

    kvm_ota_config_t cfg = KVM_OTA_CONFIG_DEFAULT;
    kvm_ota_init(&s_mgr, &s_platform, &cfg);

    (void)kvm_ota_add_probe(&s_mgr, "usb", probe_usb, NULL, true);
    (void)kvm_ota_add_probe(&s_mgr, "hid", probe_hid, NULL, true);
#if CONFIG_KVM_UI_PLATFORM
    (void)kvm_ota_add_probe(&s_mgr, "lvgl", probe_lvgl, NULL, true);
#endif
    (void)kvm_ota_add_probe(&s_mgr, "touch", probe_touch, NULL, true);

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(s_running, &state);
    if (err != ESP_OK) {
        // No otadata entry for this slot (first flash over the wire, or rollback support off).
        ESP_LOGI(TAG, "esp_ota_get_state_partition: %s - treating the image as confirmed",
                 esp_err_to_name(err));
        state = ESP_OTA_IMG_UNDEFINED;
    }
    ESP_LOGI(TAG, "running %s, ota state 0x%" PRIx32 ", next %s", s_running->label, (uint32_t)state,
             s_target != NULL ? s_target->label : "(none)");
#if KVM_OTA_UNSIGNED_BUILD
    ESP_LOGE(TAG, "image signatures are NOT verified in this build - bring-up only, never a release "
                  "(see docs/ADR/ADR-031-ota-source-and-signing.md)");
#endif
#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    ESP_LOGE(TAG, "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is off: no PENDING_VERIFY state, so a bad "
                  "image cannot roll back by itself");
#endif

    kvm_ota_boot(&s_mgr, (kvm_ota_img_state_t)state, (uint64_t)esp_timer_get_time());
    rb_take_over();
    if (s_rb_valid && state != ESP_OTA_IMG_PENDING_VERIFY) {
        // this image runs again because the update after it was rolled back: report why
        s_mgr.reason = s_rb_reason;
        s_mgr.rollbacks++;
        ESP_LOGW(TAG, "booted after a rollback: %s, missing probes 0x%" PRIx32, kvm_ota_reason_name(s_rb_reason),
                 s_rb_missing);
    }

    if (xTaskCreateStatic(update_task, "ota", UPDATE_TASK_STACK / sizeof(StackType_t), NULL,
                          UPDATE_TASK_PRIO, s_task_stack, &s_task_mem) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

bool update_manager_last_rollback(kvm_ota_reason_t *reason, uint32_t *missing_mask, char *names, size_t cap)
{
    if (!s_rb_valid) {
        return false;
    }
    if (reason != NULL) {
        *reason = s_rb_reason;
    }
    if (missing_mask != NULL) {
        *missing_mask = s_rb_missing;
    }
    if (names != NULL && cap > 0u) {
        // every image registers the probes in the same order (usb, hid, lvgl, touch), so the bits name the same ones
        names[0] = '\0';
        size_t used = 0;
        for (uint8_t i = 0; i < KVM_OTA_PROBE_MAX && used + 1u < cap; i++) {
            if ((s_rb_missing & (1u << i)) == 0u) {
                continue;
            }
            const char *n = kvm_ota_health_probe_name(&s_mgr.health, i);
            int w = snprintf(names + used, cap - used, "%s%s", used == 0u ? "" : ",", n[0] != '\0' ? n : "?");
            if (w < 0) {
                break;
            }
            used += (size_t)w < cap - used ? (size_t)w : cap - used - 1u;
        }
    }
    return true;
}

esp_err_t update_manager_begin_https(const char *url, kvm_ota_reason_t *out_reason)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (url == NULL || strncmp(url, "https://", 8) != 0) {
        // TECH-041 / ADR-031: no plain http, not even "temporarily".
        if (out_reason != NULL) {
            *out_reason = KVM_OTA_R_NO_SOURCE;
        }
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(url);
    if (n >= sizeof s_url) {
        if (out_reason != NULL) {
            *out_reason = KVM_OTA_R_NO_SOURCE;
        }
        return ESP_ERR_INVALID_SIZE;
    }

    lock();
    memcpy(s_url, url, n + 1u);
    s_source = update_transport_https();
    kvm_ota_reason_t r = kvm_ota_start(&s_mgr, &s_source, s_url, (uint64_t)esp_timer_get_time());
    unlock();

    if (out_reason != NULL) {
        *out_reason = r;
    }
    return r == KVM_OTA_R_NONE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool update_manager_cancel(void)
{
    if (!s_started) {
        return false;
    }
    lock();
    bool ok = kvm_ota_cancel(&s_mgr);
    unlock();
    return ok;
}

esp_err_t update_manager_mark_valid(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    // The machine performs the mark-valid itself (pl_mark_valid) and releases input through the
    // same path as an automatic confirmation, so there is one way out of the health check.
    bool ok = kvm_ota_confirm_now(&s_mgr);
    bool already = s_mgr.confirmed;
    unlock();
    if (ok) {
        return ESP_OK;
    }
    return already ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

void update_manager_get_status(kvm_ota_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!s_started) {
        memset(out, 0, sizeof *out);
        return;
    }
    lock();
    kvm_ota_status(&s_mgr, out);
    unlock();
}

void update_manager_get_slots(update_manager_slots_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->running_label = s_running != NULL ? s_running->label : "(unknown)";
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    out->next_label = next != NULL ? next->label : "(none)";
    out->next_size = next != NULL ? next->size : 0u;
    out->running_state = KVM_OTA_IMG_STATE_UNDEFINED;
    if (s_running != NULL) {
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(s_running, &st) == ESP_OK) {
            out->running_state = (kvm_ota_img_state_t)st;
        }
    }
#if defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) || defined(CONFIG_SECURE_BOOT)
    out->signature_checked_on_update = true;
#endif
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    out->rollback_enabled = true;
#endif
}
