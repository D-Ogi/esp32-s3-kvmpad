// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Firmware integration of ui_core (TASK-060B): domain publishers -> UI mailbox -> ui_store in the
// LVGL task, and the domain sink that hands UI actions to hid_dispatch.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "ui_actions.h"
#include "ui_mailbox.h"
#include "ui_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/** After ui_platform_start(): creates the store (LVGL task), the UI tick timer and the publisher timer. */
esp_err_t ui_glue_start(void);
bool ui_glue_running(void);

/** Re-read the pad rectangle of the current screen and hand it to the touch bridge. */
void ui_glue_refresh_pad(void);

/** The store and the action gateway, for the shell (ui_shell.c). LVGL owner context only. */
ui_store_t *ui_glue_store(void);
ui_actions_t *ui_glue_actions(void);

/** Diagnostics: copies taken under the proper locks. */
void ui_glue_status(ui_state_snapshot_t *applied, ui_store_counters_t *store, ui_mailbox_counters_t *mb,
                    ui_actions_counters_t *actions);

#ifdef __cplusplus
}
#endif
