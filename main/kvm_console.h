// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include "esp_err.h"

/** Start the UART0 REPL (esp_console). Diagnostics only; no command emits HID. */
esp_err_t kvm_console_start(void);
