// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "pack_archive.h"
#include "esp_err.h"

#ifdef KVM_PACK_HAS_PUBLISHERS
esp_err_t pack_publisher_verify(const pack_archive_t *archive, const uint8_t signature[64], uint8_t fingerprint[32]);
#endif
