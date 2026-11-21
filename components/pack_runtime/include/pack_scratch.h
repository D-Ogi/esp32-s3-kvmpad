// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// All operations, including the playback presence callback, run on the sole card worker.
FILE *pack_scratch_open(void);
FILE *pack_scratch_open_for(uint32_t bytes);
bool pack_scratch_present(void);
bool pack_scratch_close(void);
