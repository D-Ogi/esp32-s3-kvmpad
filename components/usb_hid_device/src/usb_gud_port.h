// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include <stdint.h>

const uint8_t *usb_gud_device_descriptor(void);
const uint8_t *usb_gud_config_descriptor(void);
const uint16_t *usb_gud_product_string(void);
bool usb_monitor_port_gud_ready(void);
