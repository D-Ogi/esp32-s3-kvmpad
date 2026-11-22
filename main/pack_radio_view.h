// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { PACK_RADIO_STBY, PACK_RADIO_ON, PACK_RADIO_ALT } pack_radio_mode_t;
typedef struct {
    uint32_t active_khz[2],standby_khz[2];
    uint16_t squawk; // Four displayed octal digits encoded as an integer 0..07777.
    pack_radio_mode_t mode;
    uint8_t mode_mask; // Bit (1 << mode) for each selectable transponder mode.
} pack_radio_readback_t;
typedef enum { PACK_RADIO_TUNE, PACK_RADIO_SWAP, PACK_RADIO_SQUAWK, PACK_RADIO_MODE } pack_radio_action_t;
typedef struct {
    uint32_t id,value;
    pack_radio_action_t action;
    unsigned com; // Zero-based, relevant to tune and swap only.
} pack_radio_request_t;
// A sender copies the request before returning true. That only acknowledges dispatch.
typedef bool (*pack_radio_sender_t)(const pack_radio_request_t *request,void *ctx);

bool pack_radio_view_open(lv_obj_t *parent,int width,int height,bool package_selection);
// A changed package selection closes the draft. Requests already sent keep their original bank.
void pack_radio_view_select(unsigned index);
void pack_radio_view_tick(void);
void pack_radio_view_close(void);
// UI-thread adapter. Rebind on transport changes. NULL disconnects and clears the panel.
// Deliver snapshots in order. Pass reply_id=0 for external cockpit changes.
void pack_radio_view_bind(pack_radio_sender_t sender,void *ctx);
bool pack_radio_view_readback(const pack_radio_readback_t *snapshot,uint32_t reply_id,bool rejected);
