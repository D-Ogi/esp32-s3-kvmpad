// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "kvmbridge.h"
#include "remote_json.h"

typedef struct {
    kvmbridge_crop_t geometry;
    uint32_t sequence, image_crc;
} kvb_crop_record_t;

bool kvb_crop_parse(const kvm_json_doc_t *doc,uint32_t stream,unsigned cw,unsigned ch,kvb_crop_record_t *record);
bool kvb_crop_match(const kvb_crop_record_t *record,kvm_live_receiver_t *receiver,const uint8_t *image,size_t size);
