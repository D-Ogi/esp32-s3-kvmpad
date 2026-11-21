// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once
#include "touch_router.h"
#include "hpe_reports.h"

// Build a full parallel frame. When five old contacts are replaced by five new ones,
// report old liftoffs and continuing contacts before admitting the new contacts.
int ptp_contacts_update(hpe_frame_t *active, const touch_rect_t *rect, const kvm_touch_sample_t *sample,
                        hpe_frame_t out[2]);
