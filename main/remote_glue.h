// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Firmware wiring of the secure remote transport (TASK-100, ADR-032): the device identity on NVS, the paired
// register on config_store, the REMOTE HID path through hid_dispatch, the authoritative state pushed into the
// transport, and a read-only view of the pairing / approval state for the device UI. The UI answers through
// remote_glue_action(); nothing here calls LVGL and nothing logs a code, a token or a payload (SEC-105).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_GLUE_NAME_CAP 24

typedef enum {
    REMOTE_GLUE_RESULT_NONE = 0,
    REMOTE_GLUE_RESULT_PAIRED,
    REMOTE_GLUE_RESULT_REJECTED,
    REMOTE_GLUE_RESULT_FAILED,
    REMOTE_GLUE_RESULT_LOCKED,
    REMOTE_GLUE_RESULT_EXPIRED,
} remote_glue_result_t;

typedef enum {
    REMOTE_GLUE_ACT_PAIR_OPEN = 0,
    REMOTE_GLUE_ACT_PAIR_CANCEL,
    REMOTE_GLUE_ACT_PAIR_ACCEPT,
    REMOTE_GLUE_ACT_PAIR_REJECT,
    REMOTE_GLUE_ACT_APPROVE,
    REMOTE_GLUE_ACT_DENY,
} remote_glue_action_t;

/** Everything the pairing and approval screens show. `code` is for the screen only, never for a log. */
typedef struct {
    bool window_open;
    char code[8];
    uint32_t code_remaining_ms;
    uint32_t window_remaining_ms;
    bool accept_pending;
    char client_name[REMOTE_GLUE_NAME_CAP];
    uint32_t accept_remaining_ms;
    remote_glue_result_t result;
    uint32_t result_seq;
    char result_name[REMOTE_GLUE_NAME_CAP];
    bool approval_pending;
    uint64_t approval_session;
    char approval_name[REMOTE_GLUE_NAME_CAP];
    uint32_t approval_remaining_ms;
} remote_glue_view_t;

/** Coarse transport state for the CLIENT indicator (FR-403); refreshed by the glue task every 100 ms. */
typedef struct {
    bool running;             // the WSS server is up
    bool enabled;             // Settings -> Remote is on
    uint8_t control_sessions; // authenticated /v1/ws sessions open now
    uint8_t pairing_sessions; // /v1/pair connections open now
} remote_glue_link_t;

/** Lock-free read of the last link state (safe from the LVGL task: no transport mutex is taken). */
void remote_glue_get_link(remote_glue_link_t *out);

/** Load or create the device identity and start the WSS server. Call after net_status_start(). */
esp_err_t remote_glue_start(void);

/** Read the current view; `seq` changes whenever anything in it changed (the UI polls it). */
void remote_glue_get_view(remote_glue_view_t *out, uint32_t *seq);

/** A local answer from the device UI or the console (physical presence). */
esp_err_t remote_glue_action(remote_glue_action_t act, uint64_t session_id);

/**
 * The same answer from the LVGL task: queued and carried out by the glue task within 100 ms, so a tap never
 * waits for the transport mutex. False when the queue is full or the transport is not running (the prompt
 * then stays up and expires, fail closed).
 */
bool remote_glue_post_action(remote_glue_action_t act, uint64_t session_id);

/** A Settings write from the LVGL task, carried out by the glue task (never waits for a lock in LVGL). */
typedef void (*remote_glue_setting_fn)(uint8_t id, int32_t value, uint8_t index);
bool remote_glue_post_setting(remote_glue_setting_fn apply, uint8_t id, int32_t value, uint8_t index);

/** FR-726: remove a register entry through the transport, which closes that client's sessions first. */
bool remote_glue_forget_client(uint8_t index);

/** OTA / factory reset: drop every remote session before maintenance (update_manager hook). */
void remote_glue_drop_all_for_maintenance(void *ctx);

/** Console `remote`: status, counters, `pair open|cancel`, `enable on|off`, `forget <i>|all`. */
int remote_glue_console_cmd(int argc, char **argv);

#ifdef __cplusplus
}
#endif
