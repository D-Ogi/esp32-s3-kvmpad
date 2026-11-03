// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Live allocation counters of the host LVGL allocator (host_lv_mem.c).
#pragma once

#include <stddef.h>

size_t host_lv_live_count(void);
size_t host_lv_live_bytes(void);
