// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include "src/display/lv_display_private.h"

// LVGL 9.6 sends RENDER_START after merging damage and before choosing its last
// flush. Preserve the merged coverage while using taller tiles from the 12-line
// landscape draw buffer. After 90-degree rotation these are contiguous RGB pixels.
enum { UI_PORTRAIT_BAND_WIDTH=192 };

static inline void ui_portrait_split_damage(lv_display_t *display, int band_width)
{
    if (display->render_mode!=LV_DISPLAY_RENDER_MODE_PARTIAL ||
        lv_display_get_rotation(display)!=LV_DISPLAY_ROTATION_90) return;
    const int max_last_width=band_width+32;
    const unsigned initial=display->inv_p;
    for (unsigned i=0;i<initial;i++) {
        lv_area_t *first=&display->inv_areas[i];
        int width=lv_area_get_width(first);
        if (display->inv_area_joined[i] || width<=max_last_width || lv_area_get_height(first)<64) continue;
        unsigned additional=(unsigned)(width-max_last_width+band_width-1)/band_width;
        if (additional>LV_INV_BUF_SIZE-display->inv_p) continue;
        lv_area_t band=*first;
        first->x2=first->x1+band_width-1;
        for (unsigned n=0;n<additional;n++) {
            band.x1+=band_width;
            lv_area_t part=band;
            if (n+1<additional) part.x2=part.x1+band_width-1;
            display->inv_area_joined[display->inv_p]=0;
            display->inv_areas[display->inv_p++]=part;
        }
    }
}
