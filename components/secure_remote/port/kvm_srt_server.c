// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TLS + WebSocket transport for the secure remote session glue (TASK-100; TECH-040..042,
// SEC-103, FR-721..727, ADR-028 §2, ADR-032).
//
// Layering: `esp_https_server` provides the TLS 1.3 listener and RFC 6455 framing, this file
// provides admission, connection bookkeeping and the one timer; `kvm_srt` provides every decision.
// No protocol knowledge lives here beyond "a text frame is one protocol frame".
//
// Why esp_https_server rather than an own framer: the pinned IDF already contains a reviewed RFC
// 6455 implementation behind the same TLS session as the HTTP upgrade, including masking,
// fragmentation limits, PING/PONG and the control-frame opcodes. Writing a second framer would add
// an attack surface with no requirement behind it. What the component does NOT give us is
// authentication before the 101, which is why `ws_pre_handshake_cb` carries the whole token check.
//
// Serialization. There is exactly one `kvm_srt_t` and one mutex. Every core call and every socket
// write happens with that mutex held, which is what keeps two tasks from interleaving WebSocket
// frames on one socket. Reads are done before the mutex is taken, so the hold time is one frame's
// processing plus at most `send_wait_timeout` for a stalled peer; that is the bound a caller from
// the UI task can wait. The alternative -- marshalling every call onto the server task with
// httpd_queue_work() -- was rejected because the calls that the UI makes (pair accept/reject,
// client removal) need an answer, and waiting for a queued work item has the same bound plus a
// queue that can be full.
//
// Shutdown. httpd_stop() makes the server task close every session, so close_fn runs while
// kvm_srt_server_stop() would still be holding the mutex. The flag `s_stopping` is therefore set
// before httpd_stop() and makes close_fn skip the core entirely: the core has already been told
// about every connection through kvm_srt_drop_all(KVM_SRT_LOSS_SHUTDOWN).

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_srt_server.h"
#include "sdkconfig.h"

#if !CONFIG_HTTPD_WS_SUPPORT
#error "secure_remote needs CONFIG_HTTPD_WS_SUPPORT=y"
#endif
#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#error "secure_remote needs CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y for pre-upgrade auth"
#endif
#if !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "secure_remote needs CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y"
#endif

#ifdef CONFIG_KVM_SRT_MAX_SOCKETS
#define SRT_MAX_SOCKETS CONFIG_KVM_SRT_MAX_SOCKETS
#else
#define SRT_MAX_SOCKETS 3
#endif

#ifdef CONFIG_KVM_SRT_SOCKET_TIMEOUT_S
#define SRT_SOCKET_TIMEOUT_S CONFIG_KVM_SRT_SOCKET_TIMEOUT_S
#else
#define SRT_SOCKET_TIMEOUT_S 2
#endif

// The core instance is a few kB of session state; keep it out of internal RAM where PSRAM exists.
#ifdef CONFIG_SPIRAM
#include "esp_attr.h"
#define SRT_BSS EXT_RAM_BSS_ATTR
#else
#define SRT_BSS
#endif

static const char *TAG = "srt_srv";

#define LOCK_WAIT_MS 2000
#define TICK_TASK_STACK 4096
#define TICK_TASK_PRIO 4

// ---- state ---------------------------------------------------------------------------------

typedef struct {
    int fd;
    uint64_t id;
    kvm_srt_conn_kind_t kind;
    kvm_srt_loss_t loss; // what close_fn will report; set by the core's close hook
    bool loss_set;
} conn_map_t;

static SRT_BSS kvm_srt_t s_srt;
static SRT_BSS uint8_t s_rx[KVM_JSON_MAX_FRAME + 1];

static SemaphoreHandle_t s_lock;
static httpd_handle_t s_server;
static uint16_t s_port;
// Both are written by the task that stops the server and read by the server task and the tick task,
// outside the mutex -- the whole point of `s_stopping` is to be readable without taking it. They are
// plain flags, so volatile is enough and an atomic would buy nothing.
static volatile bool s_stopping;
static volatile bool s_core_ready;
static TaskHandle_t volatile s_tick_task;
static const kvm_srt_identity_t *s_identity;

static conn_map_t s_map[KVM_SRT_MAX_CONNS];
static uint64_t s_next_conn_id = 1;

// At most one upgrade request is in flight, because the server task handles requests one by one.
static kvm_srt_admission_t s_pending;
static int s_pending_fd = -1;

static inline uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static bool lock_for(uint32_t ms)
{
    if (s_lock == NULL) {
        return false;
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static bool lock_wait(void)
{
    if (s_lock == NULL) {
        return false;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

// ---- connection map ------------------------------------------------------------------------

static conn_map_t *map_by_fd(int fd)
{
    for (size_t i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (s_map[i].id != 0 && s_map[i].fd == fd) {
            return &s_map[i];
        }
    }
    return NULL;
}

static conn_map_t *map_by_id(uint64_t id)
{
    if (id == 0) {
        return NULL;
    }
    for (size_t i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (s_map[i].id == id) {
            return &s_map[i];
        }
    }
    return NULL;
}

static conn_map_t *map_alloc(void)
{
    for (size_t i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        if (s_map[i].id == 0) {
            return &s_map[i];
        }
    }
    return NULL;
}

static void map_free(conn_map_t *m)
{
    if (m != NULL) {
        memset(m, 0, sizeof(*m));
        m->fd = -1;
    }
}

// ---- hooks the adapter owns ----------------------------------------------------------------

// Called from inside a core call, so the mutex is already held by this task.
static void hook_send(void *ctx, uint64_t conn_id, kvm_srt_conn_kind_t kind, const char *frame,
                      size_t len)
{
    (void)ctx;
    (void)kind;
    conn_map_t *m = map_by_id(conn_id);
    if (m == NULL || s_server == NULL || frame == NULL || len == 0) {
        return;
    }
    httpd_ws_frame_t out;
    memset(&out, 0, sizeof(out));
    out.type = HTTPD_WS_TYPE_TEXT;
    out.payload = (uint8_t *)frame; // the API is not const-correct; the frame is not modified
    out.len = len;
    esp_err_t err = httpd_ws_send_frame_async(s_server, m->fd, &out);
    if (err != ESP_OK) {
        // The peer is gone or stalled. Report the loss now rather than waiting for the socket to
        // time out, so no lease survives a client that cannot be reached.
        ESP_LOGW(TAG, "send failed on fd %d: %s", m->fd, esp_err_to_name(err));
        m->loss = KVM_SRT_LOSS_SOCKET;
        m->loss_set = true;
        (void)httpd_sess_trigger_close(s_server, m->fd);
    }
}

static void hook_close(void *ctx, uint64_t conn_id, kvm_srt_conn_kind_t kind)
{
    (void)ctx;
    (void)kind;
    conn_map_t *m = map_by_id(conn_id);
    if (m == NULL || s_server == NULL) {
        return;
    }
    if (!m->loss_set) {
        // The core decided to drop the connection after answering it; the usual cause is frame
        // abuse past the bad-frame budget (03 §6.5).
        m->loss = KVM_SRT_LOSS_PROTOCOL;
        m->loss_set = true;
    }
    (void)httpd_sess_trigger_close(s_server, m->fd);
}

// ---- WebSocket endpoints -------------------------------------------------------------------

/**
 * True when `Sec-WebSocket-Protocol` offers `want` exactly, in a comma- or space-separated list.
 *
 * esp_http_server answers `101` **without** the `Sec-WebSocket-Protocol` header when the offer does
 * not match `supported_subprotocol`, rather than failing the handshake. The CLIENT of ADR-028
 * aborts when the echo is missing, so the connection dies either way -- but it dies after the
 * upgrade, with a session open on our side. Checking here makes it die before, and makes the
 * requirement symmetric: the DEVICE insists on the subprotocol just as the CLIENT does.
 */
static bool subprotocol_offered(httpd_req_t *req, const char *want)
{
    char offer[64];
    if (httpd_req_get_hdr_value_str(req, "Sec-WebSocket-Protocol", offer, sizeof(offer)) != ESP_OK) {
        return false;
    }
    const size_t want_len = strlen(want);
    const char *at = offer;
    while (*at != '\0') {
        while (*at == ' ' || *at == ',' || *at == '\t') {
            at++;
        }
        const char *start = at;
        while (*at != '\0' && *at != ',' && *at != ' ' && *at != '\t') {
            at++;
        }
        if ((size_t)(at - start) == want_len && memcmp(start, want, want_len) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t pre_handshake(httpd_req_t *req, kvm_srt_conn_kind_t kind)
{
    const char *want = (kind == KVM_SRT_CONN_CONTROL) ? KVM_SRT_SUBPROTO_CONTROL
                                                      : KVM_SRT_SUBPROTO_PAIRING;
    if (!subprotocol_offered(req, want)) {
        (void)httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "subprotocol");
        return ESP_FAIL;
    }

    char auth[8 + KVM_SRT_TOKEN_B64URL_LEN + 24];
    const char *authorization = NULL;
    if (kind == KVM_SRT_CONN_CONTROL) {
        const esp_err_t got = httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth));
        if (got == ESP_OK) {
            authorization = auth;
        } else if (got != ESP_ERR_NOT_FOUND) {
            // The header is there but longer than any valid credential can be. Hand the core a
            // value it will reject on encoding, so the counters say "malformed" rather than
            // "absent"; a truncated copy must never be compared against anything.
            authorization = KVM_SRT_AUTH_SCHEME "?";
        }
    }

    kvm_srt_admit_t admit = KVM_SRT_ADMIT_FORBIDDEN;
    kvm_srt_admission_t adm;
    memset(&adm, 0, sizeof(adm));
    if (lock_wait()) {
        admit = kvm_srt_admit(&s_srt, kind, authorization, now_us(), &adm);
        unlock();
    }
    // The header held a bearer token; do not leave a copy on the stack for the handshake path.
    memset(auth, 0, sizeof(auth));

    if (admit == KVM_SRT_ADMIT_OK) {
        s_pending = adm;
        s_pending_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }

    s_pending_fd = -1;
    memset(&s_pending, 0, sizeof(s_pending));

    // The bodies carry no detail: a peer must not learn from the response whether a token was
    // merely unknown, whether Remote is off or whether a pairing window exists (SEC-105).
    switch (admit) {
    case KVM_SRT_ADMIT_UNAUTHORIZED:
        (void)httpd_resp_set_hdr(req, "WWW-Authenticate", KVM_SRT_AUTH_SCHEME_CHALLENGE);
        (void)httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        break;
    case KVM_SRT_ADMIT_BUSY:
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
        (void)httpd_resp_send(req, "busy", HTTPD_RESP_USE_STRLEN);
        break;
    case KVM_SRT_ADMIT_FORBIDDEN:
    default:
        (void)httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
        break;
    }
    // ESP_FAIL closes the socket after the response, so no WebSocket state is ever created for a
    // peer that failed admission (ADR-028 §2).
    return ESP_FAIL;
}

static esp_err_t pre_handshake_control(httpd_req_t *req)
{
    return pre_handshake(req, KVM_SRT_CONN_CONTROL);
}

static esp_err_t pre_handshake_pairing(httpd_req_t *req)
{
    return pre_handshake(req, KVM_SRT_CONN_PAIRING);
}

static esp_err_t post_handshake(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (s_pending_fd != fd) {
        // Nothing was admitted for this socket. Refuse rather than open an unadmitted connection.
        ESP_LOGW(TAG, "handshake without admission on fd %d", fd);
        return ESP_FAIL;
    }
    const kvm_srt_admission_t adm = s_pending;
    s_pending_fd = -1;
    memset(&s_pending, 0, sizeof(s_pending));

    esp_err_t out = ESP_FAIL;
    if (lock_wait()) {
        conn_map_t *m = map_alloc();
        if (m != NULL) {
            const uint64_t id = s_next_conn_id++;
            if (kvm_srt_conn_open(&s_srt, id, &adm, now_us()) >= 0) {
                m->id = id;
                m->fd = fd;
                m->kind = adm.kind;
                m->loss_set = false;
                out = ESP_OK;
            } else {
                map_free(m);
            }
        }
        unlock();
    }
    if (out != ESP_OK) {
        ESP_LOGW(TAG, "no connection slot for fd %d", fd);
    }
    return out;
}

// Drain and answer a control frame. Returns ESP_OK to keep the socket, ESP_FAIL to drop it.
static esp_err_t handle_control_frame(httpd_req_t *req, const httpd_ws_frame_t *f, int fd)
{
    switch (f->type) {
    case HTTPD_WS_TYPE_PING: {
        httpd_ws_frame_t pong;
        memset(&pong, 0, sizeof(pong));
        pong.type = HTTPD_WS_TYPE_PONG;
        pong.payload = (uint8_t *)f->payload;
        pong.len = f->len;
        return httpd_ws_send_frame(req, &pong);
    }
    case HTTPD_WS_TYPE_PONG:
        return ESP_OK;
    case HTTPD_WS_TYPE_CLOSE: {
        httpd_ws_frame_t bye;
        memset(&bye, 0, sizeof(bye));
        bye.type = HTTPD_WS_TYPE_CLOSE;
        (void)httpd_ws_send_frame(req, &bye);
        if (lock_wait()) {
            conn_map_t *m = map_by_fd(fd);
            if (m != NULL) {
                kvm_srt_conn_close(&s_srt, m->id, KVM_SRT_LOSS_CLIENT_CLOSE, now_us());
                map_free(m);
            }
            unlock();
        }
        return ESP_OK; // httpd closes the session itself once it has seen the CLOSE opcode
    }
    default:
        return ESP_FAIL;
    }
}

static void note_loss(int fd, kvm_srt_loss_t loss)
{
    conn_map_t *m = map_by_fd(fd);
    if (m != NULL) {
        m->loss = loss;
        m->loss_set = true;
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);

    httpd_ws_frame_t f;
    memset(&f, 0, sizeof(f));
    f.payload = s_rx;
    // KVM_JSON_MAX_FRAME is the protocol ceiling (03 §6.2). A larger frame is refused here without
    // its payload being read, which desynchronizes the stream -- hence the socket goes with it.
    esp_err_t err = httpd_ws_recv_frame(req, &f, KVM_JSON_MAX_FRAME);
    if (err != ESP_OK) {
        if (lock_wait()) {
            note_loss(fd, err == ESP_ERR_INVALID_SIZE ? KVM_SRT_LOSS_PROTOCOL
                                                      : KVM_SRT_LOSS_SOCKET);
            unlock();
        }
        return ESP_FAIL;
    }

    if (f.type != HTTPD_WS_TYPE_TEXT) {
        if (f.type == HTTPD_WS_TYPE_BINARY || f.type == HTTPD_WS_TYPE_CONTINUE) {
            // Protocol v1 is text-only and one frame per message; a binary or continuation frame is
            // either a different client or an attempt to smuggle a split document past the parser.
            if (lock_wait()) {
                note_loss(fd, KVM_SRT_LOSS_PROTOCOL);
                unlock();
            }
            return ESP_FAIL;
        }
        return handle_control_frame(req, &f, fd);
    }
    if (!f.final) {
        if (lock_wait()) {
            note_loss(fd, KVM_SRT_LOSS_PROTOCOL);
            unlock();
        }
        return ESP_FAIL;
    }

    s_rx[f.len] = '\0';
    if (!lock_wait()) {
        return ESP_FAIL;
    }
    conn_map_t *m = map_by_fd(fd);
    if (m != NULL) {
        kvm_srt_conn_frame(&s_srt, m->id, (const char *)s_rx, f.len, now_us());
    }
    unlock();
    return ESP_OK;
}

// ---- session close -------------------------------------------------------------------------

static void session_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    // During httpd_stop() the core has already been told about every connection, and the stopping
    // task may still hold the mutex; touching the core here would deadlock.
    if (!s_stopping && lock_wait()) {
        conn_map_t *m = map_by_fd(sockfd);
        if (m != NULL) {
            kvm_srt_conn_close(&s_srt, m->id, m->loss_set ? m->loss : KVM_SRT_LOSS_SOCKET,
                               now_us());
            map_free(m);
        }
        unlock();
    }
    if (s_pending_fd == sockfd) {
        s_pending_fd = -1;
        memset(&s_pending, 0, sizeof(s_pending));
    }
    // esp_https_server does not override close_fn, so closing the descriptor is ours to do. The
    // TLS context is freed right after this by the session's transport-context destructor.
    (void)close(sockfd);
}

// ---- tick ----------------------------------------------------------------------------------

static void tick_task(void *arg)
{
    (void)arg;
    while (!s_stopping) {
        vTaskDelay(pdMS_TO_TICKS(KVM_SRT_TICK_MS));
        if (s_stopping) {
            break;
        }
        if (lock_wait()) {
            kvm_srt_tick(&s_srt, now_us());
            unlock();
        }
    }
    s_tick_task = NULL;
    vTaskDelete(NULL);
}

// ---- start / stop --------------------------------------------------------------------------

static const httpd_uri_t s_uri_control = {
    .uri = KVM_SRT_URI_CONTROL,
    .method = HTTP_GET,
    .handler = ws_handler,
    .user_ctx = NULL, // the connection is identified by its socket, not by a per-URI context
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = KVM_SRT_SUBPROTO_CONTROL,
    .ws_pre_handshake_cb = pre_handshake_control,
    .ws_post_handshake_cb = post_handshake,
};

static const httpd_uri_t s_uri_pairing = {
    .uri = KVM_SRT_URI_PAIRING,
    .method = HTTP_GET,
    .handler = ws_handler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = KVM_SRT_SUBPROTO_PAIRING,
    .ws_pre_handshake_cb = pre_handshake_pairing,
    .ws_post_handshake_cb = post_handshake,
};

esp_err_t kvm_srt_server_start(const kvm_srt_server_config_t *cfg)
{
    if (cfg == NULL || cfg->identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->identity->cert_len == 0 || cfg->identity->key_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    for (size_t i = 0; i < KVM_SRT_MAX_CONNS; i++) {
        map_free(&s_map[i]);
    }
    s_stopping = false;
    s_identity = cfg->identity;

    // The core gets the integrator's hooks with the two transport ones replaced by ours.
    kvm_srt_config_t core = cfg->core;
    memcpy(core.cert_sha256, s_identity->cert_sha256, sizeof(core.cert_sha256));
    snprintf(core.device_name, sizeof(core.device_name), "%s", s_identity->device_name); // truncates, terminates

    kvm_srt_hooks_t hooks = cfg->hooks;
    hooks.send = hook_send;
    hooks.close = hook_close;

    kvm_pair_err_t perr = kvm_srt_init(&s_srt, &core, &hooks);
    if (perr != KVM_PAIR_OK) {
        ESP_LOGE(TAG, "core init failed (%s)", kvm_pair_err_name(perr));
        return ESP_ERR_INVALID_STATE;
    }
    s_core_ready = true;

    httpd_ssl_config_t sc = HTTPD_SSL_CONFIG_DEFAULT();
    sc.transport_mode = HTTPD_SSL_TRANSPORT_SECURE; // the only mode; there is no plaintext listener
    sc.port_secure = cfg->port != 0 ? cfg->port : KVM_SRT_DEFAULT_PORT;
    sc.servercert = s_identity->cert_der;
    sc.servercert_len = s_identity->cert_len;
    sc.prvtkey_pem = s_identity->key_der; // DER is accepted; mbedTLS sniffs PEM vs DER
    sc.prvtkey_len = s_identity->key_len;
    sc.tls_version = ESP_TLS_VER_TLS_1_3;
    sc.session_tickets = false; // one long-lived session per CLIENT; tickets only add stored state

    sc.httpd.stack_size = cfg->stack_size != 0 ? cfg->stack_size : KVM_SRT_HTTPD_STACK;
    sc.httpd.task_priority = cfg->task_priority != 0 ? cfg->task_priority : (tskIDLE_PRIORITY + 5);
    sc.httpd.max_open_sockets = SRT_MAX_SOCKETS;
    sc.httpd.max_uri_handlers = 2;
    // LRU purging would let any peer that completes a handshake evict the session that currently
    // owns the HOST. A connection beyond the limit is refused instead.
    sc.httpd.lru_purge_enable = false;
    sc.httpd.recv_wait_timeout = SRT_SOCKET_TIMEOUT_S;
    sc.httpd.send_wait_timeout = SRT_SOCKET_TIMEOUT_S;
    sc.httpd.close_fn = session_close;
    sc.httpd.uri_match_fn = NULL; // exact match only: no wildcard can reach an endpoint

    esp_err_t err = httpd_ssl_start(&s_server, &sc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start: %s", esp_err_to_name(err));
        s_server = NULL;
        // Without a transport there is nothing for the core to serve; leaving it initialized would
        // let a wrapper call operate on a state machine no client can reach.
        kvm_srt_shutdown(&s_srt, now_us());
        s_core_ready = false;
        return err;
    }
    if ((err = httpd_register_uri_handler(s_server, &s_uri_control)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_server, &s_uri_pairing)) != ESP_OK) {
        ESP_LOGE(TAG, "uri handler: %s", esp_err_to_name(err));
        (void)httpd_ssl_stop(s_server);
        s_server = NULL;
        kvm_srt_shutdown(&s_srt, now_us());
        s_core_ready = false;
        return err;
    }
    s_port = sc.port_secure;

    TaskHandle_t tick = NULL;
    if (xTaskCreate(tick_task, "srt_tick", TICK_TASK_STACK, NULL, TICK_TASK_PRIO, &tick) !=
        pdPASS) {
        ESP_LOGE(TAG, "tick task could not be created");
        s_tick_task = NULL;
        (void)kvm_srt_server_stop();
        return ESP_ERR_NO_MEM;
    }
    s_tick_task = tick;

    if (cfg->advertise_mdns) {
        esp_err_t m = kvm_srt_mdns_start(s_identity->device_name, s_port);
        if (m != ESP_OK && m != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "mDNS not advertised: %s", esp_err_to_name(m));
        }
    }
    ESP_LOGI(TAG, "wss listening on %u", (unsigned)s_port);
    return ESP_OK;
}

esp_err_t kvm_srt_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)kvm_srt_mdns_stop();

    // Tell the core first, so every lease is released and every RELEASE_ALL raised while the
    // sockets are still there to carry the last frame.
    if (lock_wait()) {
        kvm_srt_drop_all(&s_srt, KVM_SRT_LOSS_SHUTDOWN, true, now_us());
        unlock();
    }

    s_stopping = true;
    while (s_tick_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t err = httpd_ssl_stop(s_server);
    s_server = NULL;
    s_port = 0;

    if (lock_wait()) {
        kvm_srt_shutdown(&s_srt, now_us());
        s_core_ready = false;
        for (size_t i = 0; i < KVM_SRT_MAX_CONNS; i++) {
            map_free(&s_map[i]);
        }
        unlock();
    }
    s_stopping = false;
    ESP_LOGI(TAG, "stopped");
    return err;
}

bool kvm_srt_server_running(void)
{
    return s_server != NULL;
}

uint16_t kvm_srt_server_port(void)
{
    return s_port;
}

// ---- thread-safe wrappers ------------------------------------------------------------------

#define SRT_WRAP_VOID(body)                   \
    do {                                      \
        if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) { \
            return;                           \
        }                                     \
        body;                                 \
        unlock();                             \
    } while (0)

void kvm_srt_server_set_host(kvm_host_usb_t usb, bool maintenance, bool wifi_connected)
{
    SRT_WRAP_VOID(kvm_srt_set_host(&s_srt, usb, maintenance, wifi_connected, now_us()));
}

void kvm_srt_server_set_remote_enabled(bool enabled)
{
    SRT_WRAP_VOID(kvm_srt_set_remote_enabled(&s_srt, enabled, now_us()));
}

void kvm_srt_server_set_owner(kvm_owner_kind_t kind, uint64_t session_id, uint32_t epoch)
{
    SRT_WRAP_VOID(kvm_srt_set_owner(&s_srt, kind, session_id, epoch, now_us()));
}

void kvm_srt_server_set_policy(bool pair_require_accept, kvm_control_approval_t control_approval)
{
    SRT_WRAP_VOID(kvm_srt_set_policy(&s_srt, pair_require_accept, control_approval));
}

void kvm_srt_server_notify_revoked(uint64_t session_id, kvm_rp_revoke_reason_t reason,
                                   uint32_t new_epoch)
{
    SRT_WRAP_VOID(kvm_srt_notify_revoked(&s_srt, session_id, reason, new_epoch, now_us()));
}

void kvm_srt_server_approval_result(uint64_t session_id, bool granted)
{
    SRT_WRAP_VOID(kvm_srt_approval_result(&s_srt, session_id, granted, now_us()));
}

void kvm_srt_server_drop_all(kvm_srt_loss_t loss)
{
    SRT_WRAP_VOID(kvm_srt_drop_all(&s_srt, loss, false, now_us()));
}

static kvm_pair_err_t pair_call(kvm_pair_err_t (*fn)(kvm_srt_t *, uint64_t))
{
    if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return KVM_PAIR_ERR_STATE;
    }
    kvm_pair_err_t err = fn(&s_srt, now_us());
    unlock();
    return err;
}

kvm_pair_err_t kvm_srt_server_pair_open(void)
{
    return pair_call(kvm_srt_pair_open);
}

kvm_pair_err_t kvm_srt_server_pair_accept(void)
{
    return pair_call(kvm_srt_pair_accept);
}

kvm_pair_err_t kvm_srt_server_pair_reject(void)
{
    return pair_call(kvm_srt_pair_reject);
}

kvm_pair_err_t kvm_srt_server_pair_cancel(void)
{
    return pair_call(kvm_srt_pair_cancel);
}

void kvm_srt_server_pair_view(kvm_srt_pair_view_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return;
    }
    out->window_open = kvm_srt_pair_endpoint_open(&s_srt);
    const char *code = kvm_srt_pair_code(&s_srt);
    if (code != NULL) {
        strncpy(out->code, code, sizeof(out->code) - 1u);
    }
    out->code_remaining_ms = kvm_srt_pair_code_remaining_ms(&s_srt);
    out->window_remaining_ms = kvm_srt_pair_window_remaining_ms(&s_srt);
    unlock();
}

uint8_t kvm_srt_server_clients_count(void)
{
    if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return 0;
    }
    uint8_t n = kvm_srt_clients_count(&s_srt);
    unlock();
    return n;
}

bool kvm_srt_server_clients_get(uint8_t index, kvm_pair_record_t *out)
{
    if (out == NULL || !s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return false;
    }
    bool ok = kvm_srt_clients_get(&s_srt, index, out);
    unlock();
    return ok;
}

bool kvm_srt_server_clients_remove(uint8_t index)
{
    if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return false;
    }
    bool ok = kvm_srt_clients_remove(&s_srt, index, now_us());
    unlock();
    return ok;
}

uint8_t kvm_srt_server_clients_remove_all(void)
{
    if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return 0;
    }
    uint8_t n = kvm_srt_clients_remove_all(&s_srt, now_us());
    unlock();
    return n;
}

void kvm_srt_server_get_counters(kvm_srt_counters_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return;
    }
    *out = *kvm_srt_counters(&s_srt);
    unlock();
}

void kvm_srt_server_get_pair_counters(kvm_pair_counters_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_core_ready || !lock_for(LOCK_WAIT_MS)) {
        return;
    }
    *out = *kvm_srt_pair_counters(&s_srt);
    unlock();
}
