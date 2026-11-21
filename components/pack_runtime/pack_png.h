// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Each RGBA8 pixel is emitted once. Adam7 spans have a horizontal step greater than one.
typedef void (*pack_png_span_t)(void *ctx, unsigned x, unsigned y, unsigned step, const uint8_t *rgba, unsigned count);
// Exceeding the declared budget is unsupported; a transient heap allocation failure is retryable.
esp_err_t pack_png_decode(const uint8_t *data, size_t size, size_t budget, pack_png_span_t emit, void *ctx);
esp_err_t pack_png_decode_measured(const uint8_t *data, size_t size, size_t budget, pack_png_span_t emit, void *ctx, size_t *peak);
