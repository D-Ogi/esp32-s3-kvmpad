// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "lvgl.h"

// Supply the unmodified font from the pinned LVGL tree when the global
// configuration omits it. If the application enables it, LVGL owns the symbol.
#if !LV_FONT_MONTSERRAT_36
#undef LV_FONT_MONTSERRAT_36
#define LV_FONT_MONTSERRAT_36 1
#include "src/font/lv_font_montserrat_36.c"
#endif
