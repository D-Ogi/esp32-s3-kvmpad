// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifndef PACK_MOUNT
#define PACK_MOUNT "/packs"
#endif
esp_err_t pack_card_mount(void);
void pack_card_close(void);
// Only the mounting task may probe the card. A failed probe or changed volume
// serial invalidates this mount until the task closes it and mounts again.
bool pack_card_present(void);

typedef struct {
    uint64_t sector;
    uint32_t serial;
    unsigned serial_offset;
    uint32_t mount_generation;
} pack_card_volume_t;

// Read the mounted volume identity. A caller can also request its available capacity.
bool pack_card_read_volume(pack_card_volume_t *volume, uint64_t *free_bytes);
bool pack_card_same_volume(const pack_card_volume_t *volume);
