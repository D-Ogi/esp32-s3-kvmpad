// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "esp_err.h"

// Reserve the slot and verify or establish native-mode pin levels before SD initialization.
esp_err_t pack_card_mount_on_bus(esp_err_t (*prepare)(void));
