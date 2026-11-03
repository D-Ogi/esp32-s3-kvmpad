// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ESP-IDF adapter for the secure remote transport (TASK-100; TECH-040..042, SEC-103, SEC-106,
// FR-723, ADR-028 §2, ADR-032).
//
// This is the only file in the component that knows about TLS, sockets, tasks or NVS. It owns:
//   * the per-device identity (generate once, then read back) -- kvm_srt_identity_init();
//   * one `esp_https_server` instance with two WebSocket URIs and nothing else:
//       GET /v1/ws     subprotocol `kvmpad.v1`       Authorization: Bearer <token>
//       GET /v1/pair   subprotocol `kvmpad.pair.v1`  no token, only inside a pairing window
//     Both are `wss`. The server is started in HTTPD_SSL_TRANSPORT_SECURE mode, so there is no
//     plaintext listener anywhere in the build (TECH-041, AT-SEC-01).
//   * the single kvm_srt_t instance and the lock that keeps it single-owner;
//   * the 50 ms tick, posted into the server task so timers run on the stack we sized.
//
// Authentication happens in the WebSocket pre-handshake callback, which is the earliest fail-closed
// point there is: a peer without a valid token gets HTTP 401 and never reaches a WebSocket state
// machine or a JSON parser (ADR-028 §2).
//
// Every function below is safe to call from any task; each takes the instance lock for the length
// of one core call. Nothing here calls LVGL, and nothing logs a token, a code or a payload.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "kvm_srt.h"
#include "kvm_srt_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default TLS port. The CLIENT takes the real one from the mDNS SRV record (ADR-028 §2). */
#define KVM_SRT_DEFAULT_PORT 8443

/**
 * Server task stack. The deepest path is a pairing frame: the envelope document, a round-1 blob
 * and its base64 form are all on the stack, on top of the mbedTLS record layer.
 */
#define KVM_SRT_HTTPD_STACK 8192

/** Tick period. kvm_rp_poll() wants at least every 50 ms; kvm_pair_tick() every 250 ms. */
#define KVM_SRT_TICK_MS 50

typedef struct {
    /** The identity to present. It must outlive the server; use the static one from this file. */
    const kvm_srt_identity_t *identity;

    /**
     * Core configuration. `device_name` and `cert_sha256` are taken from `identity` and need not be
     * filled. `platform` and `store` are required (randomness, monotonic clock, paired register).
     */
    kvm_srt_config_t core;

    /**
     * Integrator hooks. `send` and `close` are supplied by this adapter and whatever the caller
     * puts there is ignored -- the adapter owns the sockets. Everything else is the caller's.
     */
    kvm_srt_hooks_t hooks;

    uint16_t port;        // 0 = KVM_SRT_DEFAULT_PORT
    uint32_t stack_size;  // 0 = KVM_SRT_HTTPD_STACK
    int task_priority;    // 0 = 5
    bool advertise_mdns;  // FR-723; ignored when the mDNS component is not in the build
} kvm_srt_server_config_t;

// ---- identity ------------------------------------------------------------------------------

/**
 * Write the default advertised name, "KVMpad-XXXX" from the last two bytes of the base MAC, into
 * `out` (needs KVM_PAIR_NAME_CAP bytes, which is always enough). Stable across reboots and OTA,
 * which is what a name shown beside a pairing code has to be (FR-722, FR-723).
 */
void kvm_srt_device_name(char *out, size_t cap);

/**
 * Load the per-device identity, generating it on the first boot and persisting both blobs through
 * `store`. The returned pointer is to static storage that lives for the life of the process.
 *
 * `device_name` is the advertised name and the certificate CN; pass NULL to derive it from the base
 * MAC. Generating the key takes one P-256 keygen and one ECDSA signature -- a one-off cost that
 * never recurs, because regenerating it would invalidate every CLIENT pin (ADR-028 §3).
 *
 * ESP_ERR_INVALID_STATE when the stored blobs exist but do not parse: the caller must decide
 * whether to re-pair (regenerate) rather than have this function silently break every pin.
 */
esp_err_t kvm_srt_identity_init(const kvm_srt_identity_store_t *store, const char *device_name,
                                const kvm_srt_identity_t **out);

/** Discard the stored identity and generate a new one. Every paired CLIENT has to pair again. */
esp_err_t kvm_srt_identity_regenerate(const kvm_srt_identity_store_t *store,
                                      const char *device_name, const kvm_srt_identity_t **out);

// ---- server --------------------------------------------------------------------------------

esp_err_t kvm_srt_server_start(const kvm_srt_server_config_t *cfg);
esp_err_t kvm_srt_server_stop(void);
bool kvm_srt_server_running(void);

/** The port the server is listening on, or 0 when it is not running. */
uint16_t kvm_srt_server_port(void);

// ---- thread-safe wrappers over the core ----------------------------------------------------
//
// One lock, one short critical section per call. The names and the semantics are those of kvm_srt.h.

void kvm_srt_server_set_host(kvm_host_usb_t usb, bool maintenance, bool wifi_connected);
void kvm_srt_server_set_remote_enabled(bool enabled);
void kvm_srt_server_set_owner(kvm_owner_kind_t kind, uint64_t session_id, uint32_t epoch);
void kvm_srt_server_set_policy(bool pair_require_accept, kvm_control_approval_t control_approval);
void kvm_srt_server_notify_revoked(uint64_t session_id, kvm_rp_revoke_reason_t reason,
                                   uint32_t new_epoch);
void kvm_srt_server_approval_result(uint64_t session_id, bool granted);
void kvm_srt_server_drop_all(kvm_srt_loss_t loss);

kvm_pair_err_t kvm_srt_server_pair_open(void);
kvm_pair_err_t kvm_srt_server_pair_accept(void);
kvm_pair_err_t kvm_srt_server_pair_reject(void);
kvm_pair_err_t kvm_srt_server_pair_cancel(void);

/** Everything the pairing screen needs in one read, so the UI never holds the lock twice. */
typedef struct {
    bool window_open;
    char code[KVM_PAIR_CODE_CAP]; // 6 ASCII digits, rendered `ddd ddd`; never logged (SEC-105)
    uint32_t code_remaining_ms;
    uint32_t window_remaining_ms;
} kvm_srt_pair_view_t;

void kvm_srt_server_pair_view(kvm_srt_pair_view_t *out);

uint8_t kvm_srt_server_clients_count(void);
bool kvm_srt_server_clients_get(uint8_t index, kvm_pair_record_t *out);
bool kvm_srt_server_clients_remove(uint8_t index);
uint8_t kvm_srt_server_clients_remove_all(void);

void kvm_srt_server_get_counters(kvm_srt_counters_t *out);
void kvm_srt_server_get_pair_counters(kvm_pair_counters_t *out);

// ---- mDNS (FR-723) -------------------------------------------------------------------------
//
// A thin, optional adapter. It compiles to nothing when the managed `espressif/mdns` component is
// not in the build, so the transport never depends on discovery being available.

/** True when the build contains the mDNS component and the service can be advertised. */
bool kvm_srt_mdns_available(void);

/**
 * Advertise `_kvmpad._tcp` on `port` with TXT `name`, `v=1`, `pair=0` (FR-723, ADR-029 §4).
 * Safe to call when mDNS is absent; it then returns ESP_ERR_NOT_SUPPORTED and changes nothing.
 */
esp_err_t kvm_srt_mdns_start(const char *device_name, uint16_t port);

/** Update the TXT `pair` flag, which is 1 only while a pairing window is open (SEC-103). */
esp_err_t kvm_srt_mdns_set_pairing(bool pair_open);

esp_err_t kvm_srt_mdns_stop(void);

#ifdef __cplusplus
}
#endif
