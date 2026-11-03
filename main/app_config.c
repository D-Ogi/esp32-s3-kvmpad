// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Settings owner (TASK-090 integration). config_store itself is not thread-safe: every access goes through
// s_mtx. The writer task sleeps until the debounce says a write is due (or a mutation wakes it), so a
// slider dragged for ten seconds still costs at most two NVS writes. The task runs below the HID, touch,
// LVGL and network priorities; its stack stays in internal RAM because it writes flash.
#include "app_config.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "config_store_nvs.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_config";

#define TASK_STACK 4096 // board #1: 668 B left of 3072 after one NVS write; page GC goes deeper
#define TASK_PRIO 2
#define IDLE_WAKE_MS 5000 // re-check even without a notification (clock-based debounce)

// The record (~0.7 KiB with the pairing register) lives in PSRAM: it is touched a few times a minute at most.
EXT_RAM_BSS_ATTR static kvm_config_store_t s_store;
static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_task;
static bool s_ready;
static volatile uint32_t s_seq; // bumped under the lock by every write (app_config_seq)

static void lock(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
}

static void unlock_and_wake(bool wake)
{
    if (wake) {
        s_seq++; // every path that writes wakes the writer: one place to mark the change for the UI
    }
    xSemaphoreGive(s_mtx);
    if (wake && s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        lock();
        const uint64_t now = config_store_nvs_now_ms();
        if (kvm_config_store_tick(&s_store, now)) {
            ESP_LOGI(TAG, "settings written (%" PRIu32 " writes)", s_store.stats.writes);
        }
        uint32_t wait_ms = kvm_config_store_ms_until_write(&s_store, config_store_nvs_now_ms());
        xSemaphoreGive(s_mtx);
        if (wait_ms > IDLE_WAKE_MS) {
            wait_ms = IDLE_WAKE_MS;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms == 0 ? 1 : wait_ms));
    }
}

esp_err_t app_config_start(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    esp_err_t err = config_store_nvs_init(&s_store, NULL, config_store_nvs_now_ms());
    if (err != ESP_OK) {
        return err;
    }
    s_seq = 1;
    s_ready = true;
    if (xTaskCreatePinnedToCore(writer_task, "cfg_store", TASK_STACK, NULL, TASK_PRIO, &s_task, 0) != pdPASS) {
        // The record is still usable in RAM; it just is not persisted until the next boot.
        ESP_LOGE(TAG, "writer task not started");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---- ADR-017 cache hooks (called from the net_status task) ---------------------------------------------

static bool cache_load(net_cache_t *out)
{
    lock();
    const kvm_wifi_cache_t *w = &kvm_config_store_get(&s_store)->wifi;
    const bool ok = w->valid != 0 && w->channel >= 1 && w->channel <= 14;
    if (ok) {
        memcpy(out->bssid, w->bssid, sizeof(out->bssid));
        out->channel = w->channel;
    }
    out->valid = ok;
    unlock_and_wake(false);
    return ok;
}

static void cache_store(const net_cache_t *c)
{
    lock();
    const bool changed = kvm_config_store_set_wifi_cache(&s_store, c->bssid, c->channel, config_store_nvs_now_ms());
    unlock_and_wake(changed);
}

static void cache_invalidate(void)
{
    lock();
    kvm_config_store_clear_wifi_cache(&s_store, config_store_nvs_now_ms());
    unlock_and_wake(true);
}

net_cache_hooks_t app_config_net_cache_hooks(void)
{
    if (!s_ready) {
        return (net_cache_hooks_t){0};
    }
    return (net_cache_hooks_t){.load = cache_load, .store = cache_store, .invalidate = cache_invalidate};
}

bool app_config_confirm_alt_f4(void)
{
    if (!s_ready) {
        return false; // the record default (0) until the store is up
    }
    lock();
    const bool on = kvm_config_store_get(&s_store)->confirm_alt_f4 != 0;
    xSemaphoreGive(s_mtx);
    return on;
}

bool app_config_flush(void)
{
    if (!s_ready) {
        return true;
    }
    lock();
    const bool ok = kvm_config_store_flush(&s_store, config_store_nvs_now_ms());
    xSemaphoreGive(s_mtx);
    return ok;
}

void app_config_remote(app_config_remote_t *out)
{
    // Fail closed before the record is loaded: remote off, every approval asked.
    out->remote_enabled = false;
    out->pair_require_accept = true;
    out->control_approval = KVM_CONTROL_APPROVAL_ALWAYS;
    if (!s_ready) {
        return;
    }
    lock();
    const kvm_config_t *c = kvm_config_store_get(&s_store);
    out->remote_enabled = c->remote_enabled != 0;
    out->pair_require_accept = c->pair_require_accept != 0;
    out->control_approval = (kvm_control_approval_t)c->control_approval;
    xSemaphoreGive(s_mtx);
}

static int pair_add(void *ctx, const kvm_pair_record_t *rec)
{
    (void)ctx;
    if (!s_ready) {
        return -1;
    }
    lock();
    const int idx = kvm_config_store_pair_add(&s_store, rec->name, rec->token_sha256, rec->paired_at_unix,
                                              /*trusted=*/false, config_store_nvs_now_ms());
    unlock_and_wake(true);
    return idx;
}

static uint8_t pair_count(void *ctx)
{
    (void)ctx;
    if (!s_ready) {
        return 0;
    }
    lock();
    const uint8_t n = kvm_config_store_get(&s_store)->pair_count;
    xSemaphoreGive(s_mtx);
    return n;
}

static bool pair_get(void *ctx, uint8_t index, kvm_pair_record_t *out)
{
    (void)ctx;
    if (!s_ready) {
        return false;
    }
    lock();
    const kvm_config_t *c = kvm_config_store_get(&s_store);
    const bool ok = index < c->pair_count;
    if (ok) {
        const kvm_config_pair_t *p = &c->pair[index];
        memset(out, 0, sizeof(*out));
        _Static_assert(sizeof(out->name) <= sizeof(p->name), "pair name capacity");
        memcpy(out->name, p->name, sizeof(out->name) - 1u);
        _Static_assert(sizeof(out->token_sha256) == sizeof(p->token_sha), "token digest length");
        memcpy(out->token_sha256, p->token_sha, sizeof(out->token_sha256));
        out->paired_at_unix = p->paired_at_unix;
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

static bool pair_remove(void *ctx, uint8_t index)
{
    (void)ctx;
    if (!s_ready) {
        return false;
    }
    lock();
    const bool ok = kvm_config_store_pair_remove(&s_store, index, config_store_nvs_now_ms());
    unlock_and_wake(true);
    return ok;
}

static uint8_t pair_remove_all(void *ctx)
{
    (void)ctx;
    if (!s_ready) {
        return 0;
    }
    lock();
    const uint8_t n = kvm_config_store_pair_remove_all(&s_store, config_store_nvs_now_ms());
    unlock_and_wake(true);
    return n;
}

void app_config_host_layout(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    const char *src = "us";
    char buf[KVM_CONFIG_LAYOUT_ID_CAP];
    if (s_ready) {
        lock();
        memcpy(buf, kvm_config_store_get(&s_store)->host_layout, sizeof(buf));
        xSemaphoreGive(s_mtx);
        buf[sizeof(buf) - 1u] = '\0';
        src = buf;
    }
    const size_t n = strnlen(src, cap - 1u);
    memcpy(out, src, n);
    out[n] = '\0';
}

bool app_config_set_layout(const char *id)
{
    if (!s_ready || id == NULL) {
        return false;
    }
    lock();
    // The record validates the id itself (kvm_config_layout_id_valid): a malformed one is refused here and the
    // screen keeps showing what the record holds (FR-741).
    const bool ok = kvm_config_store_set_layout(&s_store, id, config_store_nvs_now_ms());
    unlock_and_wake(ok);
    return ok;
}

// ADR-039 section 9: the ordering, the dedup and the two limits live in config_store, where the record is
// and where the host tests reach them; this layer only holds the lock and wakes the writer.
bool app_config_pin_layout(const char *id)
{
    if (!s_ready || id == NULL) {
        return false;
    }
    lock();
    const bool ok = kvm_config_store_pin_toggle(&s_store, id, config_store_nvs_now_ms());
    unlock_and_wake(ok);
    return ok;
}

bool app_config_note_layout_use(const char *id)
{
    if (!s_ready || id == NULL) {
        return false;
    }
    lock();
    const bool ok = kvm_config_store_note_layout_use(&s_store, id, config_store_nvs_now_ms());
    unlock_and_wake(ok);
    return ok;
}

bool app_config_set_remote_enabled(bool on)
{
    if (!s_ready) {
        return false;
    }
    lock();
    const bool ok = kvm_config_store_set_field(&s_store, KVM_CFG_F_REMOTE_ENABLED, on ? 1u : 0u,
                                               config_store_nvs_now_ms());
    unlock_and_wake(true);
    return ok;
}

kvm_pair_store_t app_config_pair_store(void)
{
    const kvm_pair_store_t st = {.add = pair_add,
                                 .count = pair_count,
                                 .get = pair_get,
                                 .remove = pair_remove,
                                 .remove_all = pair_remove_all,
                                 .ctx = NULL};
    return st;
}

uint32_t app_config_seq(void)
{
    return s_ready ? s_seq : 0u;
}

bool app_config_settings(app_config_settings_t *out, uint32_t timeout_ms)
{
    if (!s_ready || out == NULL || xSemaphoreTake(s_mtx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    const kvm_config_t *c = kvm_config_store_get(&s_store);
    memset(out, 0, sizeof(*out));
    out->remote_enabled = c->remote_enabled != 0;
    out->pair_require_accept = c->pair_require_accept != 0;
    out->control_approval = c->control_approval;
    out->confirm_alt_f4 = c->confirm_alt_f4 != 0;
    out->keepawake_enabled = c->keepawake_enabled != 0;
    out->keepawake_interval_s = c->keepawake_interval_s;
    out->unicode_mode = c->unicode_mode;
    out->lang_ko = c->lang_ko != 0;
    out->lang_zh = c->lang_zh != 0;
    out->lang_ja = c->lang_ja != 0;
    memcpy(out->host_layout, c->host_layout, sizeof(out->host_layout));
    out->host_layout[sizeof(out->host_layout) - 1u] = '\0';
    memcpy(out->host_fav, c->host_fav, sizeof(out->host_fav)); // ADR-039: both lists as the record holds them
    out->host_fav_count =
        c->host_fav_count <= KVM_CONFIG_HOST_FAV_MAX ? c->host_fav_count : KVM_CONFIG_HOST_FAV_MAX;
    memcpy(out->host_recent, c->host_recent, sizeof(out->host_recent));
    out->host_recent_count =
        c->host_recent_count <= KVM_CONFIG_HOST_RECENT_MAX ? c->host_recent_count : KVM_CONFIG_HOST_RECENT_MAX;
    out->client_count = c->pair_count <= KVM_CONFIG_MAX_PAIRED ? c->pair_count : KVM_CONFIG_MAX_PAIRED;
    for (uint8_t i = 0; i < out->client_count; i++) {
        memcpy(out->client[i].name, c->pair[i].name, sizeof(out->client[i].name));
        out->client[i].name[sizeof(out->client[i].name) - 1u] = '\0';
        out->client[i].trusted = c->pair[i].trusted != 0;
    }
    xSemaphoreGive(s_mtx);
    return true;
}

bool app_config_set_field(kvm_config_field_t field, uint32_t value)
{
    if (!s_ready) {
        return false;
    }
    lock();
    const bool ok = kvm_config_store_set_field(&s_store, field, value, config_store_nvs_now_ms());
    unlock_and_wake(ok);
    return ok;
}

bool app_config_set_client_trusted(uint8_t index, bool trusted)
{
    if (!s_ready) {
        return false;
    }
    lock();
    const bool ok = kvm_config_store_pair_set_trusted(&s_store, index, trusted, config_store_nvs_now_ms());
    unlock_and_wake(ok);
    return ok;
}

bool app_config_client_trusted(int index)
{
    if (!s_ready || index < 0) {
        return false;
    }
    lock();
    const kvm_config_t *c = kvm_config_store_get(&s_store);
    const bool t = index < (int)c->pair_count && c->pair[index].trusted != 0;
    xSemaphoreGive(s_mtx);
    return t;
}

void app_config_print(void)
{
    if (!s_ready) {
        printf("CONFIG not started\n");
        return;
    }
    lock();
    const kvm_config_store_t *s = &s_store;
    const kvm_config_t *c = kvm_config_store_get(s);
    const uint64_t now = config_store_nvs_now_ms();
    const uint32_t until = kvm_config_store_ms_until_write(s, now);
    char when[12] = "none";
    if (until != UINT32_MAX) {
        snprintf(when, sizeof(when), "%" PRIu32, until);
    }
    printf("CONFIG load=%s recovered=%s future_schema=%s hold=%s dirty=%s write_in_ms=%s\n",
           kvm_config_load_status_str(s->load_status), s->recovered ? "yes" : "no", s->future_schema ? "yes" : "no",
           s->hold_writes ? "yes" : "no", s->dirty ? "yes" : "no", when);
    printf("CONFIG_COUNTERS changes=%" PRIu32 " coalesced=%" PRIu32 " writes=%" PRIu32 " write_failures=%" PRIu32
           " immediate=%" PRIu32 " erases=%" PRIu32 " holds=%" PRIu32 "\n",
           s->stats.changes, s->stats.coalesced, s->stats.writes, s->stats.write_failures, s->stats.immediate_writes,
           s->stats.erases, s->stats.holds);
    printf("CONFIG_FIELDS");
    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        printf(" %s=%" PRIu32, kvm_config_field_key((kvm_config_field_t)f), kvm_config_get(c, (kvm_config_field_t)f));
    }
    printf("\n");
    // The BSSID is not printed: it identifies the user's network. Pairing entries hold digests only; not printed.
    printf("CONFIG_EXTRA host_layout=%s wifi_cache=%s channel=%u paired=%u\n", c->host_layout,
           c->wifi.valid ? "valid" : "none", (unsigned)c->wifi.channel, (unsigned)c->pair_count);
    xSemaphoreGive(s_mtx);
}
