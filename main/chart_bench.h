// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "esp_err.h"
#include <stdbool.h>

/** Time chart repaints, pans and pinches on the local LCD using a card archive. */
esp_err_t chart_bench_run(const char *filename, bool navigation, unsigned target_page, unsigned band_width);
