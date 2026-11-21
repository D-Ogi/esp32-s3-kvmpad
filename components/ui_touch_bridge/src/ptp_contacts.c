// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "ptp_contacts.h"

static uint16_t axis(int point, int low, int high)
{
    if (point <= low) return 0;
    if (point >= high) return 32767;
    return (uint16_t)(((point - low) * 32767 + (high - low) / 2) / (high - low));
}

static int find(const hpe_frame_t *frame, uint8_t id)
{
    for (uint8_t i = 0; i < frame->count; i++) if (frame->contact[i].id == id) return i;
    return -1;
}

int ptp_contacts_update(hpe_frame_t *active, const touch_rect_t *r, const kvm_touch_sample_t *s, hpe_frame_t out[2])
{
    if (!s->valid || s->point_count > HPE_CONTACTS || r->x2 <= r->x1 || r->y2 <= r->y1) return -1;
    hpe_frame_t next = {.scan_time = (uint16_t)(s->timestamp_us / 100)};
    for (unsigned i = 0; i < s->point_count; i++) {
        const kvm_touch_point_t *p = &s->point[i];
        if (find(&next, p->track_id) >= 0) return -1;
        next.contact[next.count++] = (hpe_contact_t){
            .id = p->track_id, .tip = true, .confidence = true,
            .x = axis(p->x_px, r->x1, r->x2), .y = axis(p->y_px, r->y1, r->y2)};
    }
    if (!next.count && !active->count) return 0;
    hpe_frame_t first = {.scan_time = next.scan_time};
    for (unsigned i = 0; i < active->count; i++) {
        hpe_contact_t c = active->contact[i];
        int j = find(&next, c.id);
        if (j >= 0) c = next.contact[j];
        else c.tip = false;
        first.contact[first.count++] = c;
    }
    bool split = false;
    for (unsigned i = 0; i < next.count; i++) {
        if (find(active, next.contact[i].id) >= 0) continue;
        if (first.count == HPE_CONTACTS) {
            split = true;
            break;
        }
        first.contact[first.count++] = next.contact[i];
    }
    *active = next;
    out[0] = first;
    if (split) out[1] = next;
    return split ? 2 : 1;
}
