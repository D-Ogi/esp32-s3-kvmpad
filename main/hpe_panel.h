// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once
#include <stdbool.h>

// LVGL owner only. Home restores the shell's touch region and the ordinary mouse path.
bool hpe_panel_open(void (*home)(void));
void hpe_panel_leave(void);
