// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *pack_alloc(size_t bytes);
void pack_free(void *allocation);
uint32_t pack_crc(const uint8_t *data, size_t len);
bool pack_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
#ifdef KVM_PACK_HAS_PUBLISHERS
bool pack_sha512(const uint8_t *data, size_t len, uint8_t out[64]);
#endif
esp_err_t pack_inflate(const uint8_t *data, size_t compressed, uint8_t *out, size_t size);
// The synchronous video worker provides bounded inflater storage on its stack.
esp_err_t pack_inflate_band(const uint8_t *data, size_t compressed, uint8_t *out, size_t size);
