// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"

esp_err_t usb_monitor_port_init(void (*lost_hid)(void));
void usb_monitor_port_deinit(void);
bool usb_monitor_port_select(bool monitor, bool native, bool controls, bool windows_modes);
#if CONFIG_KVM_USB_GUD
bool usb_monitor_port_select_gud(bool active);
#endif
#if CONFIG_KVM_USB_SCREENSHOT
esp_err_t usb_monitor_port_screenshot(bool enable);
#endif
/* TinyUSB task only. Disconnected intervals also exclude HID callbacks. */
bool usb_monitor_port_owns_bus(void);
void usb_monitor_port_reset(void);
void usb_monitor_port_mount(void);
void usb_monitor_port_unmount(void);
void usb_monitor_port_suspend(bool suspended);
