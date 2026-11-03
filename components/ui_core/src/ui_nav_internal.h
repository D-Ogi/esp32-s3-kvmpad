// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Shared between ui_router.c and ui_overlay.c only.
#pragma once

#include "ui_overlay.h"
#include "ui_router.h"

bool ui_nav_cancel(ui_router_t *r, ui_cancel_reason_t why);
void ui_nav_area_changed(ui_router_t *r);
/** Remove every overlay; `sync` deletes the objects now (deinit), otherwise asynchronously. */
void ui_nav_overlays_clear(ui_router_t *r, bool sync);
