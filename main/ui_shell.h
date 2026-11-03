// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Product shell (07 §5): ui_core router on its own screen, the TASK-060D top bar (brand, mode switcher, HOST and
// CLIENT indicators), the primary views of ui_views and the local action sink (navigation, overlays). Bench scenes
// (TASK-060A) take the screen with ui_shell_suspend() and `ui shell` brings the shell back.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "ui_app.h"
#include "ui_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the shell once (after ui_glue_start), load it and turn pad -> HID on. */
esp_err_t ui_shell_show(void);

/** Before a bench scene takes the screen: cancel touch, pad -> HID off, the active view leaves its press state. */
void ui_shell_suspend(void);

/** True while the shell screen is the active LVGL screen. */
bool ui_shell_active(void);

/** If the shell is on screen, hand its pad rectangle to the touch bridge and return true. */
bool ui_shell_refresh_pad(void);

typedef struct {
    ui_router_counters_t router;
    ui_app_counters_t app;
    ui_view_mouse_counters_t mouse;
    ui_view_keyboard_counters_t keyboard;
    ui_view_shortcuts_counters_t shortcuts;
    uint32_t details_opens; // HOST / CLIENT details cards opened
    uint32_t details_closes;
    uint8_t views;
} ui_shell_counters_t;

/**
 * Bench / HIL: submit a LOCAL action (navigate, open details, dismiss) through the same action gateway the
 * mode switcher and the indicators use. Domain actions (keys, buttons, shortcuts) are refused: a console line
 * never becomes HID. ESP_ERR_INVALID_STATE while the shell is not on screen; ESP_FAIL when the shell refused it.
 */
esp_err_t ui_shell_local_action(ui_action_type_t type, uint16_t value);

/** Diagnostics: shell, router and view counters (zeroed before the shell exists). */
void ui_shell_status(ui_shell_counters_t *out);

#ifdef __cplusplus
}
#endif
