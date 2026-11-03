// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// touch_input (TASK-030): the single GT911 reader. IRQ -> task notification -> bounded read
// under the board I2C lock -> kvm_touch_sample_t into one bounded queue.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "kvm_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t i2c_addr;        // address that answered after the strap (0x5D or 0x14)
    char product_id[5];      // ASCII from 0x8140, e.g. "911"
    uint16_t fw_version;     // 0x8144
    uint16_t info_x_res;     // 0x8146 (product info block)
    uint16_t info_y_res;     // 0x8148
    uint8_t vendor_id;       // 0x814A
    uint8_t config_version;  // 0x8047
    uint16_t cfg_x_max;      // 0x8048 (used for scaling)
    uint16_t cfg_y_max;      // 0x804A
    uint8_t cfg_touch_number;// 0x804C
    uint8_t cfg_int_mode;    // 0x804D bits 0..1: 0 rising, 1 falling, 2 low level, 3 high level
} touch_input_info_t;

typedef struct {
    uint32_t irq_count;        // GT911 INT edges
    uint32_t reads;            // status reads (IRQ + poll)
    uint32_t samples;          // samples published (valid + invalid)
    uint32_t invalid_samples;  // published cancel samples (errors, corrupt frames, gaps)
    uint32_t i2c_errors;       // failed transfers, injected faults included
    uint32_t injected_faults;  // faults consumed from touch_input_inject_faults()
    uint32_t corrupt_frames;   // decode rejected the frame
    uint32_t gap_cancels;      // >120 ms without a frame while fingers were down (TECH-064)
    uint32_t recoveries;       // bus reset + GT911 re-reset
    uint32_t queue_overflows;  // oldest sample dropped because the consumer lagged
    uint32_t max_read_us;      // longest status+points read incl. lock wait
    uint8_t max_points_seen;
} touch_input_stats_t;

/**
 * Detect the GT911 address (reset with the 0x5D strap, probe 0x5D then 0x14 — never guessed),
 * read product/config info, register the INT handler and start the sampling task.
 * board_hal_init() must have run.
 */
esp_err_t touch_input_start(void);

/** Next sample for the single consumer (M1: the diagnostic screen; later ui_touch_bridge). */
bool touch_input_receive(kvm_touch_sample_t *out, TickType_t wait);

/** Latest published sample (diagnostics). */
bool touch_input_get_last(kvm_touch_sample_t *out);

void touch_input_get_info(touch_input_info_t *out);
void touch_input_get_stats(touch_input_stats_t *out);

/** Fault injection (AT-FAULT-01, I2C error injection): the next `count` reads fail. */
void touch_input_inject_faults(uint32_t count);

#ifdef __cplusplus
}
#endif
