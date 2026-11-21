// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "lvgl.h"
#include <stdbool.h>

// Called only by the LVGL owner. The shell supplies its Home transition.
bool pack_panel_open(void (*home)(void), void (*tools)(void));
// Paired request under the UI lock. May only reconfigure a locally running monitor.
bool pack_panel_remote_monitor_mode(bool native);
#if CONFIG_KVM_USB_MONITOR_EVAL
bool pack_panel_open_monitor(bool native, bool controls);
bool pack_panel_open_windows_monitor(bool controls);
#ifdef KVM_USB_MOTION_CHECK
bool pack_panel_open_motion_fixture(bool native, bool (*fixture_link)(bool));
#endif
#endif
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
#if CONFIG_KVM_USB_MONITOR_EVAL
typedef enum {
    USB_MONITOR_TRIAL_IDLE, USB_MONITOR_TRIAL_CHART, USB_MONITOR_TRIAL_LINK, USB_MONITOR_TRIAL_DESKTOP
} usb_monitor_trial_t;
bool pack_panel_usb_monitor_test(usb_monitor_trial_t trial);
#endif
bool pack_panel_card_reference(void (*home)(void));
bool pack_panel_card_run(void (*home)(void), const char *file, const char *mode, const char *source, bool pixel_size);
bool pack_panel_card_action(const char *action);
bool pack_panel_card_home(void);
#endif
void pack_panel_leave(void);
void pack_panel_magnify(const char *mode);
unsigned pack_panel_scans(void);
unsigned pack_panel_menus(void);
unsigned pack_panel_choices(void);
bool pack_panel_local_contacts(unsigned count, const lv_point_t physical[2], const uint8_t ids[2], uint64_t timestamp_us);
