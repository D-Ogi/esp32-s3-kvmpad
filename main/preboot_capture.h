// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint16_t *pixels;
    uint16_t width, height;
    char name[13];
} preboot_picture_t;

// The card worker owns the FILE. Read only after the MSC owner is detached.
esp_err_t preboot_volume_open(FILE **image);
esp_err_t preboot_read_latest(FILE *image, preboot_picture_t *picture);
