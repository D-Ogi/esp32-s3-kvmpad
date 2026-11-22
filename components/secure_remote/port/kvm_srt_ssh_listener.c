// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "kvm_srt_ssh_listener.h"
#include "kvm_srt_server.h"
#include "ssh_control_identity.h"
#include "ssh_control_session.h"
#include "config.h"           // pinned libssh build configuration for its session header
#include <libssh/session.h>
#include <libssh/server.h>
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static SemaphoreHandle_t sockets_lock, finished;
static TaskHandle_t socket_task;
static atomic_bool stopping;
static ssh_bind host;
static bool libssh_initialized; // service start/stop are serialized by the server owner
static int listener = -1;
static atomic_uint sessions, approvals, grants, commands, stack_free_min;
static portMUX_TYPE diag_lock = portMUX_INITIALIZER_UNLOCKED;
static kvm_srt_ssh_stats_t progress = {.phase = "none", .ended = "none"};

void kvm_ssh_listener_stats(kvm_srt_ssh_stats_t *out)
{
    portENTER_CRITICAL(&diag_lock);
    *out = progress;
    portEXIT_CRITICAL(&diag_lock);
    out->sessions = atomic_load(&sessions);
    out->approval_attempts = atomic_load(&approvals);
    out->grants = atomic_load(&grants);
    out->accepted_commands = atomic_load(&commands);
    out->stack_free_min = atomic_load(&stack_free_min);
}

typedef struct { char address[16]; uint64_t sponsor, id; int64_t started_us; } peer_t;

static void diagnostic(void *ctx, kvm_ssh_diag_phase_t stage, const char *reason)
{
    peer_t *peer = ctx;
    int64_t now = esp_timer_get_time();
    if (stage == KVM_SSH_DIAG_SETUP) peer->started_us = now;
    uint32_t elapsed = (uint32_t)((now - peer->started_us) / 1000);
    portENTER_CRITICAL(&diag_lock);
    progress.elapsed_ms = elapsed;
    switch (stage) {
    case KVM_SSH_DIAG_SETUP:
        progress.phase = "setup";
        progress.ended = "running";
        progress.kex_ms = 0;
        progress.phase_after_kex_ms = 0;
        break;
    case KVM_SSH_DIAG_KEX: progress.phase = "kex"; progress.kex++; progress.kex_ms = elapsed; break;
    case KVM_SSH_DIAG_QUERY: progress.phase = "query"; progress.queries++; break;
    case KVM_SSH_DIAG_QUERY_ACCEPTED: progress.phase = "query_ok"; progress.queries_accepted++; break;
    case KVM_SSH_DIAG_VALID: progress.phase = "signature"; progress.signatures_valid++; break;
    case KVM_SSH_DIAG_AUTHENTICATED: progress.phase = "authenticated"; progress.authenticated++; break;
    case KVM_SSH_DIAG_CHANNEL: progress.phase = "channel"; progress.channels++; break;
    case KVM_SSH_DIAG_SUBSYSTEM: progress.phase = "subsystem"; progress.subsystems++; break;
    case KVM_SSH_DIAG_SHELL: progress.phase = "shell"; progress.shells++; break;
    case KVM_SSH_DIAG_END: progress.ended = reason; break;
    }
    if (stage != KVM_SSH_DIAG_SETUP && stage != KVM_SSH_DIAG_END)
        progress.phase_after_kex_ms = elapsed - progress.kex_ms;
    portEXIT_CRITICAL(&diag_lock);
}

static bool paired(void *ctx, const char *user, const uint8_t key[32])
{
    peer_t *p = ctx;
    if (strcmp(user, "kvmpad") != 0 || atomic_load(&stopping)) return false;
    uint64_t sponsor = kvm_ssh_board_sponsor(p->address, key);
    if (!sponsor || (p->sponsor && sponsor != p->sponsor)) return false;
    p->sponsor = sponsor;
    return true;
}

static bool approve(void *ctx, const char *user, const uint8_t key[32])
{
    peer_t *p = ctx;
    if (!paired(ctx, user, key)) return false;
    p->id = kvm_ssh_board_begin(p->address, key, p->sponsor);
    if (p->id) atomic_fetch_add(&approvals, 1);
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (p->id && !atomic_load(&stopping) && esp_timer_get_time() < deadline) {
        kvm_srt_ssh_state_t state = kvm_ssh_board_state(p->id);
        if (state == KVM_SSH_GRANTED) {
            atomic_fetch_add(&grants, 1);
            return true;
        }
        if (state != KVM_SSH_PENDING) return false;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return false;
}

static bool active(void *ctx)
{
    return !atomic_load(&stopping) && kvm_ssh_board_state(((peer_t *)ctx)->id) == KVM_SSH_GRANTED;
}
static bool command(void *ctx, const kvm_hid_intent_t *intent)
{
    if (!kvm_ssh_board_input(((peer_t *)ctx)->id, intent)) return false;
    atomic_fetch_add(&commands, 1);
    return true;
}
static bool heartbeat(void *ctx) { return kvm_ssh_board_ping(((peer_t *)ctx)->id); }
static void release(void *ctx) { kvm_ssh_board_release(((peer_t *)ctx)->id); }

static void serve(int fd, const struct sockaddr_in *from)
{
    peer_t p = {0};
    if (!inet_ntop(AF_INET, &from->sin_addr, p.address, sizeof(p.address)) || !kvm_ssh_board_sponsor(p.address, NULL)) {
        close(fd);
        return;
    }
    ssh_session session = ssh_new();
    if (session && ssh_bind_accept_fd(host, session, fd) == SSH_OK) {
        atomic_fetch_add(&sessions, 1);
        const kvm_ssh_session_ops_t ops = {.paired = paired, .request_local_approval = approve, .active = active,
                                           .command = command, .release = release, .peer_activity = heartbeat,
                                           .diagnostic = diagnostic};
        kvm_ssh_serve_session(session, &ops, &p);
    } else if (!session || !session->socket || ssh_get_fd(session) != fd) {
        // accept_fd may fail before allocating its socket or after attaching fd.
        close(fd);
    }
    // The channel handler finishes the SSH channel. Free closes any attached
    // descriptor, including one adopted by a partially constructed session.
    ssh_free(session);
}

static void provision(void *argument)
{
    uint8_t *pin = argument;
    ssh_key key = NULL;
    bool config = false;
    // The pinned port's ssh_finalize() would deinitialize TLS's shared PSA service.
    if (!libssh_initialized) libssh_initialized = ssh_init() == SSH_OK;
    if (libssh_initialized) host = ssh_bind_new();
    // Constrain negotiation on the 7B to the cipher and MAC qualified with
    // signed OpenSSH authentication. Key exchange alone also succeeds on
    // combinations that stall before the client signature is accepted.
    const char *cipher = "aes128-ctr";
    const char *mac = "hmac-sha2-256-etm@openssh.com,hmac-sha2-256";
    if (!host || ssh_bind_options_set(host, SSH_BIND_OPTIONS_PROCESS_CONFIG, &config) != SSH_OK ||
        ssh_bind_options_set(host, SSH_BIND_OPTIONS_KEY_EXCHANGE, "curve25519-sha256,ecdh-sha2-nistp256") != SSH_OK ||
        ssh_bind_options_set(host, SSH_BIND_OPTIONS_CIPHERS_C_S, cipher) != SSH_OK ||
        ssh_bind_options_set(host, SSH_BIND_OPTIONS_CIPHERS_S_C, cipher) != SSH_OK ||
        ssh_bind_options_set(host, SSH_BIND_OPTIONS_HMAC_C_S, mac) != SSH_OK ||
        ssh_bind_options_set(host, SSH_BIND_OPTIONS_HMAC_S_C, mac) != SSH_OK ||
        !kvm_ssh_identity_open_nvs(&key, pin) ||
        ssh_bind_options_set(host, SSH_BIND_OPTIONS_IMPORT_KEY, key) != SSH_OK) {
        ssh_key_free(key);
        ssh_bind_free(host);
        host = NULL;
    }
    xSemaphoreGive(finished);
    vTaskSuspend(NULL); // The owner frees this internal stack before creating the socket task.
}

static void worker(void *argument)
{
    (void)argument;
    atomic_store(&stack_free_min, uxTaskGetStackHighWaterMark(NULL));
    while (!atomic_load(&stopping)) {
        struct sockaddr_in from = {0};
        socklen_t len = sizeof(from);
        xSemaphoreTake(sockets_lock, portMAX_DELAY);
        int fd = listener >= 0 ? accept(listener, (struct sockaddr *)&from, &len) : -1;
        xSemaphoreGive(sockets_lock);
        if (fd >= 0) {
            serve(fd, &from);
            atomic_store(&stack_free_min, uxTaskGetStackHighWaterMark(NULL));
        }
        else vTaskDelay(pdMS_TO_TICKS(40));
    }
    xSemaphoreGive(finished);
    vTaskSuspend(NULL);
}

bool kvm_ssh_listener_open(void)
{
    if (!host || atomic_load(&stopping)) return false;
    xSemaphoreTake(sockets_lock, portMAX_DELAY);
    if (listener < 0) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd >= 0) {
            struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(22), .sin_addr.s_addr = htonl(INADDR_ANY)};
            int reuse = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                listen(fd, 1) == 0) listener = fd;
            else close(fd);
        }
    }
    bool ready = listener >= 0;
    xSemaphoreGive(sockets_lock);
    return ready;
}

void kvm_ssh_listener_close_port(void)
{
    if (!sockets_lock) return;
    xSemaphoreTake(sockets_lock, portMAX_DELAY);
    if (listener >= 0) { close(listener); listener = -1; }
    xSemaphoreGive(sockets_lock);
}

bool kvm_ssh_listener_start(uint8_t pin[32])
{
    memset(pin, 0, 32);
    if (host) return false;
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    return false;
#endif
    sockets_lock = xSemaphoreCreateMutex();
    finished = xSemaphoreCreateBinary();
    atomic_store(&stopping, false);
    TaskHandle_t init_task = NULL;
    if (sockets_lock && finished &&
        xTaskCreateWithCaps(provision, "ssh_provision", 10240, pin, 4, &init_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
        xSemaphoreTake(finished, portMAX_DELAY);
        vTaskDeleteWithCaps(init_task);
        // The worker uses the already-imported key and never opens the NVS store.
        if (host && xTaskCreateWithCaps(worker, "kvmpad_ssh", 24576, NULL, 4, &socket_task,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) return true;
    }
    ssh_bind_free(host);
    host = NULL;
    if (finished) vSemaphoreDelete(finished);
    if (sockets_lock) vSemaphoreDelete(sockets_lock);
    finished = sockets_lock = NULL;
    memset(pin, 0, 32);
    return false;
}

void kvm_ssh_listener_stop(void)
{
    if (!host) return;
    atomic_store(&stopping, true);
    kvm_ssh_listener_close_port();
    xSemaphoreTake(finished, portMAX_DELAY); // handshake and write deadlines are bounded
    vTaskDeleteWithCaps(socket_task);
    socket_task = NULL;
    vSemaphoreDelete(finished);
    vSemaphoreDelete(sockets_lock);
    finished = sockets_lock = NULL;
    ssh_bind_free(host);
    host = NULL;
}
