// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "usb_monitor.h"

typedef struct { int x1, y1, x2, y2; } monitor_damage_t;

// LVGL selects completed VGA pictures and starts pending native decodes on this timer.
enum { MONITOR_POLL_MS = 8 };

// LVGL task. Stop after the visible image descriptor has been detached.
bool monitor_start(bool native);
void monitor_stop(void);
// Detach all LVGL references to receiver pixels before calling. The two receiver
// configurations share the same PSRAM budget and cannot be allocated together.
static inline usb_monitor_mode_result_t monitor_change_receiver(bool native, bool (*select_profile)(bool))
{
    monitor_stop();
    if (!monitor_start(native)) return monitor_start(!native) ? USB_MONITOR_MODE_RESTORED : USB_MONITOR_MODE_STOPPED;
    if (select_profile(native)) return USB_MONITOR_MODE_CHANGED;
    monitor_stop();
    if (monitor_start(!native)) {
        if (select_profile(!native)) return USB_MONITOR_MODE_RESTORED;
        monitor_stop();
    }
    return USB_MONITOR_MODE_STOPPED;
}
// Set only for a rejected JPEG in this stream. A good decode or bus reset clears it.
bool monitor_last_decode_failed(void);
// LVGL task only. Selected pixels are stable between calls. Native decodes
// inside this call before the next LVGL render, restoring the picture on error.
// A NULL surface removes a previous picture after a bus reset.
// Damage is relative to the previous selection, in inclusive image coordinates.
// NULL requests a full repaint. Otherwise zero, one or two rectangles cover all changes.
void monitor_present(void (*select)(const uint16_t *pixels, bool has_nonblack,
                                    const monitor_damage_t *damage, unsigned count));
void monitor_refresh_complete(void);
#ifdef KVM_USB_PICTURE_CHECK
usb_monitor_picture_t monitor_checked_picture(const uint16_t *selected, const char *decoded_picture);
#endif
