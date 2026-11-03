// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Threading:
// - the transport hooks below run in the WSS server task with the transport lock held: they must not block for
//   long and must not call kvm_srt_server_* (the lock is not recursive). They only post to hid_dispatch or
//   record UI state under s_mux.
// - one small glue task polls the authoritative snapshots (HID owner/epoch/USB, Wi-Fi, settings) every 100 ms
//   and pushes changes into the transport; it also reads the pairing view for the UI.
// - remote_glue_action() runs in the UI or console task and calls the thread-safe kvm_srt_server_* wrappers.
#include "remote_glue.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_dispatch.h"
#include "kvm_srt_server.h"
#include "net_status.h"
#include "nvs.h"

static const char *TAG = "remote";

#define GLUE_TASK_STACK 4096
#define GLUE_TASK_PRIO 3
#define GLUE_PERIOD_MS 100
#define APPROVAL_PROMPT_MS 30000u // 03 §6.5
#define NVS_NS "kvm_srt"
#define ACTION_QUEUE_LEN 8

typedef struct {
    remote_glue_setting_fn apply; // NULL = a pairing / approval answer
    remote_glue_action_t act;
    uint64_t session;
    uint8_t setting;
    uint8_t index;
    int32_t value;
} queued_action_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static remote_glue_view_t s_view; // under s_mux
static uint32_t s_view_seq;       // under s_mux
static uint64_t s_accept_deadline_ms;
static uint64_t s_approval_deadline_ms;
static uint64_t s_remote_session; // the session the arbiter granted last (under s_mux)
static bool s_released_by_hook;   // the last REMOTE loss came through control_release (under s_mux)
static const kvm_srt_identity_t *s_id;
static bool s_started;
static remote_glue_link_t s_link; // under s_mux
static QueueHandle_t s_actions;   // taps from the LVGL task (remote_glue_post_action)
static uint32_t s_actions_dropped;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void bump(void)
{
    s_view_seq++;
}

// ---- identity blobs on NVS --------------------------------------------------------------------
static bool id_load(void *ctx, const char *key, uint8_t *out, size_t cap, size_t *out_len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    *out_len = len;
    return true;
}

static bool id_save(void *ctx, const char *key, const uint8_t *in, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, key, in, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

// ---- pairing platform -------------------------------------------------------------------------
static bool plat_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len); // hardware RNG; pairing needs Wi-Fi up (SEC-106), so the RF entropy source runs
    return true;
}

static uint64_t plat_now_ms(void *ctx)
{
    (void)ctx;
    return now_ms();
}

static uint64_t plat_unix(void *ctx)
{
    (void)ctx;
    const time_t t = time(NULL);
    return t > 1700000000 ? (uint64_t)t : 0u; // 0 = no wall clock yet (accepted by the core)
}

// ---- transport hooks (server task, transport lock held) ----------------------------------------
static kvm_hid_err_t h_intent(void *ctx, uint64_t session_id, uint32_t lease_epoch, const kvm_hid_intent_t *intent)
{
    (void)ctx;
    const esp_err_t err = hid_dispatch_remote(session_id, lease_epoch, intent);
    if (err == ESP_OK) {
        return KVM_HID_OK; // the arbiter re-checks owner and epoch in the TinyUSB task and counts a refusal
    }
    return err == ESP_ERR_TIMEOUT ? KVM_HID_ERR_HID_QUEUE_OVERFLOW : KVM_HID_ERR_BAD_INTENT;
}

static kvm_hid_err_t h_request(void *ctx, uint64_t session_id)
{
    (void)ctx;
    return hid_dispatch_remote_request(session_id);
}

static kvm_hid_err_t h_grant(void *ctx, uint64_t session_id, uint32_t *lease_epoch)
{
    (void)ctx;
    const kvm_hid_err_t e = hid_dispatch_remote_grant(session_id, lease_epoch);
    if (e == KVM_HID_OK) {
        portENTER_CRITICAL(&s_mux);
        s_remote_session = session_id;
        s_released_by_hook = false;
        portEXIT_CRITICAL(&s_mux);
    }
    return e;
}

static void h_release(void *ctx, uint64_t session_id, kvm_release_reason_t reason, uint32_t *new_epoch)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mux);
    if (s_remote_session == session_id) {
        s_released_by_hook = true; // the transport already told this client why
    }
    portEXIT_CRITICAL(&s_mux);
    const uint32_t epoch = hid_dispatch_remote_disconnect_sync(session_id, reason);
    if (new_epoch != NULL) {
        *new_epoch = epoch;
    }
}

static void h_heartbeat(void *ctx, uint64_t session_id)
{
    (void)ctx;
    hid_dispatch_remote_heartbeat(session_id);
}

static void h_release_all(void *ctx, kvm_release_reason_t reason)
{
    (void)ctx;
    hid_dispatch_release_all(reason);
}

static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t n = src != NULL ? strnlen(src, cap - 1u) : 0u;
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static void h_status(void *ctx, kvm_rp_status_t *out)
{
    (void)ctx;
    kvm_hid_snapshot_t snap;
    hid_dispatch_get_snapshot(&snap, NULL);
    net_status_info_t ni;
    net_status_get(&ni);
    out->host_usb = snap.host_usb;
    out->maintenance = snap.maintenance;
    out->wifi_connected = ni.link.ip_present;
    out->owner = snap.owner_kind;
    out->lease_epoch = snap.owner_epoch;
    copy_name(out->device_id, sizeof(out->device_id), s_id != NULL ? s_id->device_name : "kvmpad");
    copy_name(out->fw, sizeof(out->fw), esp_app_get_description()->version);
    app_config_host_layout(out->host_layout, sizeof(out->host_layout));
}

static kvm_srt_approval_t h_approval_begin(void *ctx, uint64_t session_id, const char *client_name, const char *reason)
{
    (void)ctx;
    (void)reason;
#if CONFIG_KVM_UI_PLATFORM
    portENTER_CRITICAL(&s_mux);
    s_view.approval_pending = true;
    s_view.approval_session = session_id;
    copy_name(s_view.approval_name, sizeof(s_view.approval_name), client_name);
    s_approval_deadline_ms = now_ms() + APPROVAL_PROMPT_MS;
    s_view.approval_remaining_ms = APPROVAL_PROMPT_MS;
    bump();
    portEXIT_CRITICAL(&s_mux);
    return KVM_SRT_APPROVAL_PENDING;
#else
    (void)session_id;
    (void)client_name;
    return KVM_SRT_APPROVAL_DENIED; // no screen, no physical approval: refuse (fail closed)
#endif
}

static void h_approval_cancel(void *ctx, uint64_t session_id)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mux);
    if (s_view.approval_pending && s_view.approval_session == session_id) {
        s_view.approval_pending = false;
        bump();
    }
    portEXIT_CRITICAL(&s_mux);
}

static bool h_trusted(void *ctx, int client_index)
{
    (void)ctx;
    return app_config_client_trusted(client_index);
}

static void set_result(remote_glue_result_t r, const char *name)
{
    s_view.result = r;
    s_view.result_seq++;
    copy_name(s_view.result_name, sizeof(s_view.result_name), name);
}

static void h_pair_event(void *ctx, const kvm_pair_event_t *ev)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mux);
    switch (ev->kind) {
    case KVM_PAIR_EVENT_WINDOW_OPENED:
        s_view.accept_pending = false;
        break;
    case KVM_PAIR_EVENT_ACCEPT_SCREEN:
        s_view.accept_pending = true;
        copy_name(s_view.client_name, sizeof(s_view.client_name), ev->client_name);
        s_accept_deadline_ms = now_ms() + ev->remaining_ms;
        s_view.accept_remaining_ms = ev->remaining_ms;
        break;
    case KVM_PAIR_EVENT_PAIRED_TOAST:
        set_result(REMOTE_GLUE_RESULT_PAIRED, ev->client_name);
        break;
    case KVM_PAIR_EVENT_FAILED_ATTEMPT:
        set_result(REMOTE_GLUE_RESULT_FAILED, "");
        break;
    case KVM_PAIR_EVENT_LOCKOUT:
        set_result(REMOTE_GLUE_RESULT_LOCKED, "");
        break;
    case KVM_PAIR_EVENT_WINDOW_CLOSED:
        s_view.accept_pending = false;
        switch (ev->reason) {
        case KVM_PAIR_CLOSE_COMPLETED:
            set_result(REMOTE_GLUE_RESULT_PAIRED, s_view.client_name);
            break;
        case KVM_PAIR_CLOSE_LOCAL_REJECT:
            set_result(REMOTE_GLUE_RESULT_REJECTED, s_view.client_name);
            break;
        case KVM_PAIR_CLOSE_CODE_EXPIRED:
        case KVM_PAIR_CLOSE_WINDOW_CAP:
        case KVM_PAIR_CLOSE_ACCEPT_TIMEOUT:
            set_result(REMOTE_GLUE_RESULT_EXPIRED, "");
            break;
        case KVM_PAIR_CLOSE_TOO_MANY_FAILURES:
        case KVM_PAIR_CLOSE_LOCKED:
            set_result(REMOTE_GLUE_RESULT_LOCKED, "");
            break;
        case KVM_PAIR_CLOSE_INTERNAL_ERROR:
            set_result(REMOTE_GLUE_RESULT_FAILED, "");
            break;
        default:
            break; // local cancel: the user knows, no toast
        }
        break;
    default:
        break;
    }
    bump();
    portEXIT_CRITICAL(&s_mux);
}

// ---- glue task: authoritative state -> transport, pairing view -> UI ------------------------------
static kvm_rp_revoke_reason_t revoke_reason(const kvm_hid_snapshot_t *s)
{
    if (s->maintenance) {
        return KVM_RP_REVOKE_MAINTENANCE;
    }
    if (s->host_usb != KVM_HOST_USB_MOUNTED) {
        return KVM_RP_REVOKE_HOST_OFFLINE;
    }
    if (s->owner_kind == OWNER_LOCAL) {
        return KVM_RP_REVOKE_LOCAL_OVERRIDE;
    }
    return KVM_RP_REVOKE_CLIENT_STALE; // lease expired in the arbiter
}

static void glue_task(void *arg)
{
    (void)arg;
    kvm_host_usb_t last_usb = (kvm_host_usb_t)-1;
    bool last_maint = false;
    bool last_wifi = false;
    bool first = true;
    kvm_owner_kind_t last_owner = OWNER_NONE;
    uint32_t last_epoch = 0;
    app_config_remote_t last_pol = {0};
    uint32_t pair_tick = 0;
    int mdns_pair = 0; // last `pair=` flag advertised; kvm_srt_mdns_start() publishes pair=0
    for (;;) {
        queued_action_t qa;
        while (xQueueReceive(s_actions, &qa, 0) == pdTRUE) {
            if (qa.apply != NULL) {
                qa.apply(qa.setting, qa.value, qa.index);
                continue;
            }
            const esp_err_t aerr = remote_glue_action(qa.act, qa.session);
            if (aerr != ESP_OK) {
                ESP_LOGW(TAG, "ui action %d: %s", (int)qa.act, esp_err_to_name(aerr)); // no code, no name
            }
        }
        kvm_hid_snapshot_t snap;
        hid_dispatch_get_snapshot(&snap, NULL);
        net_status_info_t ni;
        net_status_get(&ni);
        const bool wifi = ni.link.ip_present;
        if (first || snap.host_usb != last_usb || snap.maintenance != last_maint || wifi != last_wifi) {
            kvm_srt_server_set_host(snap.host_usb, snap.maintenance, wifi);
            last_usb = snap.host_usb;
            last_maint = snap.maintenance;
            last_wifi = wifi;
        }
        if (first || snap.owner_kind != last_owner || snap.owner_epoch != last_epoch) {
            portENTER_CRITICAL(&s_mux);
            const uint64_t session = s_remote_session;
            const bool by_hook = s_released_by_hook;
            portEXIT_CRITICAL(&s_mux);
            if (!first && last_owner == OWNER_REMOTE && session != 0 && !by_hook &&
                (snap.owner_kind != OWNER_REMOTE || snap.owner_epoch != last_epoch)) {
                // The arbiter took control away (local override, USB loss, maintenance, lease): tell the client.
                kvm_srt_server_notify_revoked(session, revoke_reason(&snap), snap.owner_epoch);
            }
            kvm_srt_server_set_owner(snap.owner_kind, snap.owner_kind == OWNER_REMOTE ? session : 0u,
                                     snap.owner_epoch);
            last_owner = snap.owner_kind;
            last_epoch = snap.owner_epoch;
        }
        app_config_remote_t pol;
        app_config_remote(&pol);
        if (first || pol.remote_enabled != last_pol.remote_enabled) {
            kvm_srt_server_set_remote_enabled(pol.remote_enabled);
        }
        if (first || pol.pair_require_accept != last_pol.pair_require_accept ||
            pol.control_approval != last_pol.control_approval) {
            kvm_srt_server_set_policy(pol.pair_require_accept, pol.control_approval);
        }
        last_pol = pol;
        first = false;

        kvm_srt_counters_t c;
        kvm_srt_server_get_counters(&c);
        const remote_glue_link_t link = {
            .running = kvm_srt_server_running(),
            .enabled = pol.remote_enabled,
            .control_sessions = (uint8_t)(c.conns_opened[KVM_SRT_CONN_CONTROL] - c.conns_closed[KVM_SRT_CONN_CONTROL]),
            .pairing_sessions = (uint8_t)(c.conns_opened[KVM_SRT_CONN_PAIRING] - c.conns_closed[KVM_SRT_CONN_PAIRING]),
        };
        portENTER_CRITICAL(&s_mux);
        s_link = link;
        portEXIT_CRITICAL(&s_mux);

        // Pairing view for the screen (every 200 ms is plenty for a seconds countdown).
        if ((pair_tick++ & 1u) == 0u) {
            kvm_srt_pair_view_t pv;
            kvm_srt_server_pair_view(&pv);
            // Only on a change: every TXT update restarts the mDNS probe (or sends a fresh announcement), so a
            // periodic call keeps the responder probing for ever and it never answers a PTR query.
            if ((int)pv.window_open != mdns_pair && kvm_srt_mdns_set_pairing(pv.window_open) == ESP_OK) {
                mdns_pair = (int)pv.window_open;
            }
            const uint64_t now = now_ms();
            portENTER_CRITICAL(&s_mux);
            const bool changed = pv.window_open != s_view.window_open || strcmp(pv.code, s_view.code) != 0 ||
                                 pv.code_remaining_ms / 1000u != s_view.code_remaining_ms / 1000u ||
                                 pv.window_remaining_ms / 1000u != s_view.window_remaining_ms / 1000u;
            s_view.window_open = pv.window_open;
            memcpy(s_view.code, pv.code, sizeof(s_view.code) < sizeof(pv.code) ? sizeof(s_view.code) : sizeof(pv.code));
            s_view.code[sizeof(s_view.code) - 1u] = '\0';
            s_view.code_remaining_ms = pv.code_remaining_ms;
            s_view.window_remaining_ms = pv.window_remaining_ms;
            if (!pv.window_open) {
                s_view.accept_pending = false;
            }
            if (s_view.accept_pending) {
                s_view.accept_remaining_ms = s_accept_deadline_ms > now ? (uint32_t)(s_accept_deadline_ms - now) : 0u;
            }
            if (s_view.approval_pending) {
                s_view.approval_remaining_ms =
                    s_approval_deadline_ms > now ? (uint32_t)(s_approval_deadline_ms - now) : 0u;
            }
            if (changed || s_view.accept_pending || s_view.approval_pending) {
                bump();
            }
            portEXIT_CRITICAL(&s_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(GLUE_PERIOD_MS));
    }
}

// ---- public API ---------------------------------------------------------------------------------
esp_err_t remote_glue_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const kvm_srt_identity_store_t ids = {.load = id_load, .save = id_save, .ctx = NULL};
    esp_err_t err = kvm_srt_identity_init(&ids, NULL, &s_id);
    if (err != ESP_OK) {
        // Never regenerate silently: that would break every CLIENT pin (ADR-028 §3). Remote stays off.
        ESP_LOGE(TAG, "device identity: %s - remote access stays off", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "device identity %s (%s)", s_id->device_name, s_id->generated ? "created" : "loaded");

    kvm_srt_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.identity = s_id;
    cfg.core.platform = (kvm_pair_platform_t){.random = plat_random, .now_ms = plat_now_ms, .unix_time = plat_unix};
    cfg.core.store = app_config_pair_store();
    app_config_remote_t pol;
    app_config_remote(&pol);
    cfg.core.pair_require_accept = pol.pair_require_accept;
    cfg.core.control_approval = pol.control_approval;
    cfg.hooks = (kvm_srt_hooks_t){.hid_intent = h_intent,
                                  .control_request = h_request,
                                  .control_grant = h_grant,
                                  .control_release = h_release,
                                  .heartbeat = h_heartbeat,
                                  .release_all = h_release_all,
                                  .status_fill = h_status,
                                  .approval_begin = h_approval_begin,
                                  .approval_cancel = h_approval_cancel,
                                  .client_trusted = h_trusted,
                                  .pair_event = h_pair_event,
                                  .ctx = NULL};
    cfg.advertise_mdns = true;
    err = kvm_srt_server_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "kvm_srt_server_start: %s", esp_err_to_name(err));
        return err;
    }
    s_actions = xQueueCreate(ACTION_QUEUE_LEN, sizeof(queued_action_t));
    if (s_actions == NULL ||
        xTaskCreate(glue_task, "remote_glue", GLUE_TASK_STACK, NULL, GLUE_TASK_PRIO, NULL) != pdPASS) {
        kvm_srt_server_stop();
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "wss server on port %u, remote %s", (unsigned)kvm_srt_server_port(), pol.remote_enabled ? "on" : "off");
    return ESP_OK;
}

void remote_glue_get_view(remote_glue_view_t *out, uint32_t *seq)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_view;
    if (seq != NULL) {
        *seq = s_view_seq;
    }
    portEXIT_CRITICAL(&s_mux);
}

void remote_glue_get_link(remote_glue_link_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_link;
    portEXIT_CRITICAL(&s_mux);
}

bool remote_glue_post_action(remote_glue_action_t act, uint64_t session_id)
{
    if (!s_started || s_actions == NULL) {
        return false;
    }
    const queued_action_t qa = {.apply = NULL, .act = act, .session = session_id};
    if (xQueueSend(s_actions, &qa, 0) != pdTRUE) {
        s_actions_dropped++;
        return false;
    }
    return true;
}

bool remote_glue_post_setting(remote_glue_setting_fn apply, uint8_t id, int32_t value, uint8_t index)
{
    if (!s_started || s_actions == NULL || apply == NULL) {
        return false;
    }
    const queued_action_t qa = {.apply = apply, .setting = id, .value = value, .index = index};
    if (xQueueSend(s_actions, &qa, 0) != pdTRUE) {
        s_actions_dropped++;
        return false;
    }
    return true;
}

bool remote_glue_forget_client(uint8_t index)
{
    return s_started && kvm_srt_server_clients_remove(index);
}

esp_err_t remote_glue_action(remote_glue_action_t act, uint64_t session_id)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    kvm_pair_err_t pe = KVM_PAIR_OK;
    switch (act) {
    case REMOTE_GLUE_ACT_PAIR_OPEN:
        pe = kvm_srt_server_pair_open();
        break;
    case REMOTE_GLUE_ACT_PAIR_CANCEL:
        pe = kvm_srt_server_pair_cancel();
        break;
    case REMOTE_GLUE_ACT_PAIR_ACCEPT:
        pe = kvm_srt_server_pair_accept();
        break;
    case REMOTE_GLUE_ACT_PAIR_REJECT:
        pe = kvm_srt_server_pair_reject();
        break;
    case REMOTE_GLUE_ACT_APPROVE:
    case REMOTE_GLUE_ACT_DENY: {
        portENTER_CRITICAL(&s_mux);
        const bool mine = s_view.approval_pending && s_view.approval_session == session_id;
        if (mine) {
            s_view.approval_pending = false;
            bump();
        }
        portEXIT_CRITICAL(&s_mux);
        if (!mine) {
            return ESP_ERR_INVALID_STATE;
        }
        kvm_srt_server_approval_result(session_id, act == REMOTE_GLUE_ACT_APPROVE);
        return ESP_OK;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return pe == KVM_PAIR_OK ? ESP_OK : ESP_FAIL;
}

void remote_glue_drop_all_for_maintenance(void *ctx)
{
    (void)ctx;
    if (s_started) {
        kvm_srt_server_drop_all(KVM_SRT_LOSS_MAINTENANCE);
    }
}

// ---- console ------------------------------------------------------------------------------------
static void print_status(void)
{
    app_config_remote_t pol;
    app_config_remote(&pol);
    printf("REMOTE running=%d port=%u remote_enabled=%d require_accept=%d approval=%d mdns=%d\n",
           kvm_srt_server_running(), (unsigned)kvm_srt_server_port(), pol.remote_enabled, pol.pair_require_accept,
           (int)pol.control_approval, kvm_srt_mdns_available());
    if (s_id != NULL) {
        // The certificate digest is public: it is the pin a CLIENT shows for comparison (ADR-028 §3).
        printf("REMOTE device=%s cert_sha256=", s_id->device_name);
        for (size_t i = 0; i < sizeof(s_id->cert_sha256); i++) {
            printf("%02x", s_id->cert_sha256[i]);
        }
        printf(" cert_len=%u\n", (unsigned)s_id->cert_len);
    }
    kvm_srt_counters_t c;
    kvm_srt_server_get_counters(&c);
    printf("REMOTE admit ok_ctl=%" PRIu32 " ok_pair=%" PRIu32 " unauthorized=%" PRIu32 " forbidden=%" PRIu32
           " busy=%" PRIu32 " auth_absent=%" PRIu32 " auth_bad=%" PRIu32 " auth_unknown=%" PRIu32 " cooldowns=%" PRIu32 "\n",
           c.admit_ok[KVM_SRT_CONN_CONTROL], c.admit_ok[KVM_SRT_CONN_PAIRING], c.admit_unauthorized,
           c.admit_forbidden, c.admit_busy, c.auth_absent, c.auth_bad_scheme + c.auth_bad_encoding,
           c.auth_unknown_token, c.auth_cooldowns);
    printf("REMOTE conns opened=%" PRIu32 "/%" PRIu32 " closed=%" PRIu32 "/%" PRIu32 " rejected=%" PRIu32
           " release_all=%" PRIu32 " approvals auto=%" PRIu32 " trusted=%" PRIu32 " prompted=%" PRIu32 "\n",
           c.conns_opened[KVM_SRT_CONN_CONTROL], c.conns_opened[KVM_SRT_CONN_PAIRING],
           c.conns_closed[KVM_SRT_CONN_CONTROL], c.conns_closed[KVM_SRT_CONN_PAIRING], c.conns_rejected,
           c.release_all_requests, c.approvals_auto, c.approvals_trusted, c.approvals_prompted);
    kvm_pair_counters_t pc;
    kvm_srt_server_get_pair_counters(&pc);
    remote_glue_view_t v;
    remote_glue_get_view(&v, NULL);
    printf("PAIR window=%d remaining_s=%" PRIu32 " accept_pending=%d attempts=%" PRIu32 " failures=%" PRIu32
           " lockouts=%" PRIu32 " successes=%" PRIu32 " windows=%" PRIu32 " clients=%u\n",
           v.window_open, v.window_remaining_ms / 1000u, v.accept_pending, pc.pair_attempts, pc.pair_failures,
           pc.pair_lockouts, pc.pair_successes, pc.windows_opened, (unsigned)kvm_srt_server_clients_count());
    remote_glue_link_t l;
    remote_glue_get_link(&l);
    printf("LINK running=%d enabled=%d control_sessions=%u pairing_sessions=%u approval_pending=%d "
           "ui_actions_dropped=%" PRIu32 "\n",
           l.running, l.enabled, (unsigned)l.control_sessions, (unsigned)l.pairing_sessions, v.approval_pending,
           s_actions_dropped);
}

int remote_glue_console_cmd(int argc, char **argv)
{
    if (!s_started) {
        printf("remote transport not running\n");
        return 1;
    }
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(argv[1], "pair") == 0 && argc >= 3) {
        // The console is physical access, like the device screen. The code is shown on the screen only.
        const bool open = strcmp(argv[2], "open") == 0;
        const bool cancel = strcmp(argv[2], "cancel") == 0;
        if (!open && !cancel) {
            printf("usage: remote pair open|cancel\n");
            return 1;
        }
        const esp_err_t e = remote_glue_action(open ? REMOTE_GLUE_ACT_PAIR_OPEN : REMOTE_GLUE_ACT_PAIR_CANCEL, 0);
        printf("pair %s: %s\n", argv[2], e == ESP_OK ? "ok" : "refused");
        return e == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "enable") == 0 && argc >= 3) {
        const bool on = strcmp(argv[2], "on") == 0;
        if (!on && strcmp(argv[2], "off") != 0) {
            printf("usage: remote enable on|off\n");
            return 1;
        }
        const bool ok = app_config_set_remote_enabled(on);
        printf("remote.enabled=%d %s\n", on, ok ? "saved" : "refused");
        return ok ? 0 : 1;
    }
    if (strcmp(argv[1], "forget") == 0 && argc >= 3) {
        if (strcmp(argv[2], "all") == 0) {
            printf("removed %u client(s)\n", (unsigned)kvm_srt_server_clients_remove_all());
            return 0;
        }
        const int idx = atoi(argv[2]);
        const bool ok = idx >= 0 && idx < 255 && kvm_srt_server_clients_remove((uint8_t)idx);
        printf("forget %d: %s\n", idx, ok ? "removed" : "no such client");
        return ok ? 0 : 1;
    }
    printf("usage: remote [status | pair open|cancel | enable on|off | forget <index>|all]\n");
    return 1;
}
