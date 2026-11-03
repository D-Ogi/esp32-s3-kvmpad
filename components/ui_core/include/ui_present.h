// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Status wording of 01 §6.1 (HOST) and §6.2 (CLIENT) for a subject value. Pure C. An unknown or
// out-of-range value reads "Unknown": the UI never upgrades a status it was not told (FR-401/403).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *ui_present_host(int32_t host_usb_state);
const char *ui_present_client(int32_t client_state);
const char *ui_present_owner(int32_t owner_kind);

#ifdef __cplusplus
}
#endif
