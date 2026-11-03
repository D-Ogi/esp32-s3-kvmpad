// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ESP-IDF glue of net_status (TASK-080, ADR-015). The link task owns net_fsm; esp_wifi / IP events and commands
// arrive through one queue. The radio is started only while someone needs it: the link (a network is configured),
// a scan, or the diagnostic stress load.
#include "net_status.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "net_status";

#define QUEUE_LEN 16
#define TICK_MS 100
#define TASK_STACK 4096
#define TASK_PRIO 3 // below HID (TinyUSB), the GT911 sampler, the touch bridge and LVGL
#define SCAN_MAX_RECORDS 24

typedef enum { MSG_EVENT = 0, MSG_STA_STARTED, MSG_CREDS_SET, MSG_FORGET, MSG_BENCH_RECONNECT } msg_kind_t;

typedef struct {
    msg_kind_t kind;
    net_event_t ev;
} msg_t;

enum { RADIO_LINK = 1u << 0, RADIO_SCAN = 1u << 1, RADIO_DIAG = 1u << 2 };

static net_status_cfg_t s_cfg;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_radio_mtx; // radio start/stop and scans
static uint8_t s_radio_users;
static bool s_radio_on;
static bool s_started;
static bool s_configured;
static net_fsm_t s_fsm; // link task only
static uint64_t s_cycle_start_us; // link task only: start of the current connect cycle, 0 = none
static bool s_cycle_cached;       // link task only: the last attempt of the cycle used the cache

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static net_status_info_t s_info; // published copy

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static void post(const msg_t *m)
{
    bool ok = s_q != NULL && xQueueSend(s_q, m, 0) == pdTRUE;
    portENTER_CRITICAL(&s_mux);
    if (ok) {
        s_info.events++;
    } else {
        s_info.events_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
}

// ---- radio users (call with s_radio_mtx held) ---------------------------------------------------
static esp_err_t radio_apply_locked(void)
{
    const bool want = s_radio_users != 0;
    esp_err_t err = ESP_OK;
    if (want && !s_radio_on) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s_radio_on = true;
            esp_wifi_set_ps(s_cfg.power_save_off ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM); // TECH-070
        }
    } else if (!want && s_radio_on) {
        err = esp_wifi_stop();
        if (err == ESP_OK) {
            s_radio_on = false;
        }
    }
    portENTER_CRITICAL(&s_mux);
    s_info.radio_on = s_radio_on;
    portEXIT_CRITICAL(&s_mux);
    return err;
}

static esp_err_t radio_use(uint8_t user, bool on)
{
    if (xSemaphoreTake(s_radio_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (on) {
        s_radio_users |= user;
    } else {
        s_radio_users &= (uint8_t)~user;
    }
    esp_err_t err = radio_apply_locked();
    xSemaphoreGive(s_radio_mtx);
    return err;
}

// ---- event handlers (default event loop task) ------------------------------------------------------
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    msg_t m = {.kind = MSG_EVENT};
    m.ev.now_us = (uint64_t)esp_timer_get_time();
    switch (id) {
    case WIFI_EVENT_STA_START:
        m.kind = MSG_STA_STARTED;
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *e = data;
        m.ev.kind = NET_EV_ASSOCIATED;
        memcpy(m.ev.bssid, e->bssid, sizeof(m.ev.bssid));
        m.ev.channel = e->channel;
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        m.ev.kind = NET_EV_DISCONNECTED;
        m.ev.reason = e->reason;
        break;
    }
    default:
        return;
    }
    post(&m);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data; // the address itself is not part of the status (ADR-015)
    msg_t m = {.kind = MSG_EVENT};
    m.ev.now_us = (uint64_t)esp_timer_get_time();
    if (id == IP_EVENT_STA_GOT_IP) {
        m.ev.kind = NET_EV_GOT_IP;
    } else if (id == IP_EVENT_STA_LOST_IP) {
        m.ev.kind = NET_EV_LOST_IP;
    } else {
        return;
    }
    post(&m);
}

// ---- actions (link task) --------------------------------------------------------------------------
static void sta_connect(const net_cache_t *cached)
{
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) {
        return;
    }
    c.sta.bssid_set = cached != NULL;
    if (cached != NULL) {
        memcpy(c.sta.bssid, cached->bssid, sizeof(c.sta.bssid));
        c.sta.channel = cached->channel;
    } else {
        memset(c.sta.bssid, 0, sizeof(c.sta.bssid));
        c.sta.channel = 0;
    }
    esp_wifi_set_config(WIFI_IF_STA, &c); // storage is RAM at run time: no flash write per attempt
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err)); // the FSM timeout recovers
    }
}

static void run_actions(const net_actions_t *a)
{
    for (uint8_t i = 0; i < a->n; i++) {
        switch (a->a[i].kind) {
        case NET_ACT_CONNECT_CACHED:
            radio_use(RADIO_LINK, true);
            sta_connect(&a->a[i].cache);
            break;
        case NET_ACT_CONNECT_FULL:
            radio_use(RADIO_LINK, true);
            sta_connect(NULL);
            break;
        case NET_ACT_DISCONNECT:
            esp_wifi_disconnect();
            break;
        case NET_ACT_CACHE_STORE:
            if (s_cfg.cache.store != NULL) {
                s_cfg.cache.store(&a->a[i].cache);
            }
            break;
        case NET_ACT_CACHE_INVALIDATE:
            if (s_cfg.cache.invalidate != NULL) {
                s_cfg.cache.invalidate();
            }
            break;
        case NET_ACT_RADIO_STOP:
            radio_use(RADIO_LINK, false);
            break;
        default:
            break;
        }
    }
}

static void publish(void)
{
    net_status_t st;
    net_fsm_status(&s_fsm, &st);
    int8_t rssi = 0;
    if (st.link == NET_LINK_ASSOCIATED || st.link == NET_LINK_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
    }
    portENTER_CRITICAL(&s_mux);
    s_info.link = st;
    s_info.rssi = rssi;
    portEXIT_CRITICAL(&s_mux);
}

static void fsm_event(net_event_t *ev)
{
    net_actions_t a;
    net_fsm_handle(&s_fsm, ev, &a);
    for (uint8_t i = 0; i < a.n; i++) {
        if (a.a[i].kind == NET_ACT_CONNECT_CACHED || a.a[i].kind == NET_ACT_CONNECT_FULL) {
            if (s_cycle_start_us == 0) {
                s_cycle_start_us = ev->now_us;
            }
            s_cycle_cached = a.a[i].kind == NET_ACT_CONNECT_CACHED;
        }
    }
    if (ev->kind == NET_EV_GOT_IP && s_fsm.link == NET_LINK_CONNECTED && s_cycle_start_us != 0) {
        const uint64_t ms = (ev->now_us - s_cycle_start_us) / 1000u;
        portENTER_CRITICAL(&s_mux);
        s_info.last_connect_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
        s_info.last_connect_cached = s_cycle_cached;
        portEXIT_CRITICAL(&s_mux);
        s_cycle_start_us = 0;
    }
    run_actions(&a);
}

static void net_task(void *arg)
{
    (void)arg;
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            switch (m.kind) {
            case MSG_EVENT:
                fsm_event(&m.ev);
                break;
            case MSG_STA_STARTED:
                if (s_fsm.link == NET_LINK_OFF && s_configured) {
                    net_event_t ev = {.kind = NET_EV_START, .now_us = m.ev.now_us, .has_credentials = true};
                    if (s_cfg.cache.load != NULL && !s_cfg.cache.load(&ev.cache)) {
                        memset(&ev.cache, 0, sizeof(ev.cache));
                    }
                    fsm_event(&ev);
                }
                break;
            case MSG_CREDS_SET: {
                s_configured = true;
                net_event_t ev = {.now_us = m.ev.now_us};
                if (s_fsm.link == NET_LINK_OFF) {
                    radio_use(RADIO_LINK, true); // STA_START then starts the FSM
                } else {
                    ev.kind = NET_EV_CREDENTIALS_SET;
                    fsm_event(&ev);
                }
                break;
            }
            case MSG_BENCH_RECONNECT:
                if (s_fsm.link == NET_LINK_CONNECTED) {
                    if (m.ev.reason != 0) {
                        memset(&s_fsm.cache, 0, sizeof(s_fsm.cache)); // RAM only: the full-scan path
                    }
                    s_cycle_start_us = m.ev.now_us;
                    esp_wifi_disconnect(); // the driver's DISCONNECTED drives the FSM back to a connect
                }
                break;
            case MSG_FORGET: {
                s_configured = false;
                net_event_t ev = {.kind = NET_EV_CREDENTIALS_CLEARED, .now_us = m.ev.now_us};
                fsm_event(&ev);
                radio_use(RADIO_LINK, false);
                break;
            }
            }
        }
        net_event_t tick = {.kind = NET_EV_TICK, .now_us = (uint64_t)esp_timer_get_time()};
        fsm_event(&tick);
        publish();
    }
}

// ---- public API ---------------------------------------------------------------------------------------
esp_err_t net_status_start(const net_status_cfg_t *cfg)
{
    if (s_started) {
        return ESP_OK;
    }
    if (cfg != NULL) {
        s_cfg = *cfg;
    }
    s_info.power_save_off = s_cfg.power_save_off;
    // The driver logs the AP's BSSID and the DHCP handler the address at INFO: neither belongs in a console log
    // (ADR-015, TECH-046). Warnings and errors stay; net_status reports the link without them.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_ERR_NO_MEM, TAG, "sta netif");
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&ic), TAG, "wifi init"); // loads the stored network from NVS
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage"); // run-time changes stay in RAM
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    wifi_config_t c;
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &c), TAG, "config");
    s_configured = c.sta.ssid[0] != 0;
    memset(&c, 0, sizeof(c)); // do not keep a copy of the credentials on the stack

    s_q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    s_radio_mtx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_q != NULL && s_radio_mtx != NULL, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL), TAG,
                        "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL, NULL), TAG,
                        "ip events");

    net_fsm_init(&s_fsm, NULL, rnd, NULL);
    if (!s_configured) {
        net_event_t ev = {.kind = NET_EV_START, .now_us = (uint64_t)esp_timer_get_time(), .has_credentials = false};
        net_actions_t a;
        net_fsm_handle(&s_fsm, &ev, &a); // NOT_CONFIGURED, radio stays off
    }
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(net_task, "net_status", TASK_STACK, NULL, TASK_PRIO, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    s_started = true;
    portENTER_CRITICAL(&s_mux);
    s_info.started = true;
    portEXIT_CRITICAL(&s_mux);
    if (s_configured) {
        radio_use(RADIO_LINK, true); // WIFI_EVENT_STA_START then starts the FSM
    }
    publish();
    ESP_LOGI(TAG, "Wi-Fi STA %s; power save %s", s_configured ? "configured, connecting" : "not configured, radio off",
             s_cfg.power_save_off ? "off" : "IDF default");
    return ESP_OK;
}

bool net_status_running(void)
{
    return s_started;
}

void net_status_get(net_status_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t net_status_scan(net_scan_ap_t *aps, uint16_t max, uint16_t *found)
{
    ESP_RETURN_ON_FALSE(s_started && found != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    net_status_info_t info;
    net_status_get(&info);
    const net_link_t l = info.link.link;
    if (l == NET_LINK_CONNECTING || l == NET_LINK_CONNECTING_CACHED || l == NET_LINK_ASSOCIATED) {
        return ESP_ERR_INVALID_STATE; // a connection attempt owns the radio
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_radio_mtx, pdMS_TO_TICKS(5000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "busy");
    s_radio_users |= RADIO_SCAN;
    esp_err_t err = radio_apply_locked();
    if (err == ESP_OK) {
        wifi_scan_config_t sc = {.show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
        err = esp_wifi_scan_start(&sc, true);
    }
    uint16_t n = 0;
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&n);
        EXT_RAM_BSS_ATTR static wifi_ap_record_t rec[SCAN_MAX_RECORDS]; // guarded by s_radio_mtx; PSRAM
        uint16_t got = SCAN_MAX_RECORDS;
        if (esp_wifi_scan_get_ap_records(&got, rec) == ESP_OK && aps != NULL) {
            for (uint16_t i = 0; i < got && i < max; i++) {
                aps[i].channel = rec[i].primary;
                aps[i].rssi = rec[i].rssi;
                aps[i].authmode = (uint8_t)rec[i].authmode;
                aps[i].hidden = rec[i].ssid[0] == 0;
                memcpy(aps[i].ssid, rec[i].ssid, sizeof(aps[i].ssid) - 1);
                aps[i].ssid[sizeof(aps[i].ssid) - 1] = 0;
            }
        } else {
            esp_wifi_clear_ap_list(); // free the driver's list when the records were not taken
        }
        portENTER_CRITICAL(&s_mux);
        s_info.scans++;
        portEXIT_CRITICAL(&s_mux);
    }
    s_radio_users &= (uint8_t)~RADIO_SCAN;
    radio_apply_locked(); // back off when nobody else needs the radio
    xSemaphoreGive(s_radio_mtx);
    *found = n;
    return err;
}

esp_err_t net_status_ensure_radio(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    return radio_use(RADIO_DIAG, true);
}

void net_status_release_radio(void)
{
    if (s_started) {
        radio_use(RADIO_DIAG, false);
    }
}

esp_err_t net_status_set_credentials(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(ssid != NULL && pass != NULL, ESP_ERR_INVALID_ARG, TAG, "args");
    const size_t sl = strlen(ssid);
    const size_t pl = strlen(pass);
    ESP_RETURN_ON_FALSE(sl >= 1 && sl <= 32 && (pl == 0 || (pl >= 8 && pl <= 63)), ESP_ERR_INVALID_ARG, TAG, "length");
    wifi_config_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.sta.ssid, ssid, sl);
    memcpy(c.sta.password, pass, pl);
    c.sta.threshold.authmode = pl == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    c.sta.pmf_cfg.capable = true;
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_FLASH); // the only flash write: the user saved a network
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &c);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    memset(&c, 0, sizeof(c));
    ESP_RETURN_ON_ERROR(err, TAG, "store");
    msg_t m = {.kind = MSG_CREDS_SET};
    m.ev.now_us = (uint64_t)esp_timer_get_time();
    post(&m);
    return ESP_OK;
}

esp_err_t net_status_set_power_save_off(bool off)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    esp_err_t err = ESP_OK;
    if (xSemaphoreTake(s_radio_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_cfg.power_save_off = off;
    if (s_radio_on) {
        err = esp_wifi_set_ps(off ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    }
    xSemaphoreGive(s_radio_mtx);
    portENTER_CRITICAL(&s_mux);
    s_info.power_save_off = off;
    portEXIT_CRITICAL(&s_mux);
    return err;
}

esp_err_t net_status_bench_reconnect(bool full)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    msg_t m = {.kind = MSG_BENCH_RECONNECT};
    m.ev.now_us = (uint64_t)esp_timer_get_time();
    m.ev.reason = full ? 1u : 0u; // carried in an otherwise unused field of the message
    post(&m);
    return ESP_OK;
}

esp_err_t net_status_forget(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    wifi_config_t c;
    memset(&c, 0, sizeof(c));
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &c);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "erase");
    msg_t m = {.kind = MSG_FORGET};
    m.ev.now_us = (uint64_t)esp_timer_get_time();
    post(&m);
    return ESP_OK;
}
