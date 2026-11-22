// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// USB-owner callbacks. The caller verifies direction, interface, configuration
// and actual OUT data first. GET returns the reply size or -1 for a stall.
int gud_panel_get(uint8_t request, uint16_t value, uint8_t *reply, size_t capacity);
bool gud_panel_set(uint8_t request, uint16_t value, const uint8_t *data, size_t length);
bool gud_panel_bulk(const uint8_t *data, size_t length);
void gud_panel_reset(void);
