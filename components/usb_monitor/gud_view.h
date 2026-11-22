// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "gud_receiver.h"

// The LVGL owner supplies GUD_FRAME_BYTES. Rows are packed at the returned width.
typedef enum { GUD_VIEW_WAITING, GUD_VIEW_BLANK, GUD_VIEW_PICTURE } gud_view_state_t;
bool gud_surface_open(void);
void gud_surface_close(void);
bool gud_surface_read(uint8_t *destination, gud_rect_t *changed, gud_view_state_t *state,
                      uint16_t *width, uint16_t *height);
