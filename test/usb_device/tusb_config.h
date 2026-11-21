// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define TUP_DCD_ENDPOINT_MAX 8
#define CFG_TUD_HID CONFIG_TINYUSB_HID_COUNT
#define CFG_TUD_HID_EP_BUFSIZE 64
#if CONFIG_KVM_USB_MONITOR_EVAL
#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_RX_BUFSIZE 0
#define CFG_TUD_VENDOR_TX_BUFSIZE 0
#define CFG_TUD_VENDOR_EPSIZE 64
#endif
