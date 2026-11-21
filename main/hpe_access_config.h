// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t scan_ms, dwell_ms, debounce_ms, cooldown_ms;
} hpe_access_timing_t;

#define HPE_ACCESS_DEFAULTS {1200, 900, 40, 300}

static inline bool hpe_access_valid(hpe_access_timing_t v)
{
    return v.scan_ms >= 600 && v.scan_ms <= 4000 && v.dwell_ms >= 600 && v.dwell_ms <= 1800 &&
           v.debounce_ms >= 20 && v.debounce_ms <= 200 && v.cooldown_ms >= 100 && v.cooldown_ms <= 1000;
}

static inline uint64_t hpe_access_pack(hpe_access_timing_t v)
{
    return v.scan_ms | (uint64_t)v.dwell_ms << 16 | (uint64_t)v.debounce_ms << 32 | (uint64_t)v.cooldown_ms << 48;
}

static inline hpe_access_timing_t hpe_access_unpack(uint64_t word)
{
    hpe_access_timing_t v = {(uint16_t)word, (uint16_t)(word >> 16), (uint16_t)(word >> 32), (uint16_t)(word >> 48)};
    const hpe_access_timing_t defaults = HPE_ACCESS_DEFAULTS;
    return hpe_access_valid(v) ? v : defaults;
}

// The app_config writer commits an explicit local Save outside the LVGL task.
bool app_config_access_read(hpe_access_timing_t *out);
bool app_config_access_save(hpe_access_timing_t value);
