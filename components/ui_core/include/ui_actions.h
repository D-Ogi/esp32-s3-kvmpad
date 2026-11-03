// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Typed UI action gateway (TASK-060B, 07 §1 second invariant, §3 item 4). A view callback builds a
// ui_action_t and submits it here; local actions (navigation, overlays) go to the router sink, domain
// actions go to the domain sink installed by the integrator (firmware: hid_dispatch, which feeds the
// InputArbiter -> HidScheduler; simulator: a fake). Nothing here sends a USB report and the result of
// a domain action is never assumed: it comes back as state or as a UI_EV_ACTION_RESULT event.
// Pure C, no allocation; called from the UI owner context only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_contracts.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Returns true when the action was handed over (queued), not when it was "executed". */
typedef bool (*ui_action_sink_fn)(void *ctx, const ui_action_t *action);

typedef struct {
    uint32_t submitted;
    uint32_t local;
    uint32_t domain;
    uint32_t rejected_invalid;  // out-of-range type / value: never forwarded
    uint32_t rejected_no_sink;
    uint32_t rejected_by_sink;  // sink refused (queue full, ...)
} ui_actions_counters_t;

typedef struct {
    ui_action_sink_fn local_sink;
    void *local_ctx;
    ui_action_sink_fn domain_sink;
    void *domain_ctx;
    uint32_t next_request_id;
    ui_actions_counters_t c;
} ui_actions_t;

void ui_actions_init(ui_actions_t *a);
void ui_actions_set_local_sink(ui_actions_t *a, ui_action_sink_fn fn, void *ctx);
void ui_actions_set_domain_sink(ui_actions_t *a, ui_action_sink_fn fn, void *ctx);

/** Range check of 07 §3 values (usage, button, chord id, view, toggle). */
bool ui_action_valid(ui_action_type_t type, uint16_t value);

/**
 * Validate, stamp (request_id, time) and forward. Returns the request id (never 0) when a sink took
 * the action, 0 when it was refused; `out` (optional) receives the stamped action either way.
 */
uint32_t ui_actions_submit(ui_actions_t *a, ui_action_type_t type, uint16_t control_id, uint16_t value,
                           uint64_t now_us, ui_action_t *out);

const char *ui_action_name(ui_action_type_t type);

#ifdef __cplusplus
}
#endif
