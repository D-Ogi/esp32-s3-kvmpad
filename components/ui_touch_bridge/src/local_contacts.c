// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "local_contacts.h"
#include "gesture_engine.h"
#include <stdlib.h>

void local_contacts_cancel(local_contacts_queue_t *q)
{
    q->blocked = true;
    q->last = (local_contacts_t){.timestamp_us=q->last_time, .count = 3};
    q->pending[0] = q->last;
    q->used = 1;
}

static bool same_contacts(const local_contacts_t *a, const local_contacts_t *b)
{
    if (a->count != b->count) return false;
    for (unsigned i = 0; i < a->count && i < 2; ++i)
        if (a->points[i].track_id != b->points[i].track_id) return false;
    return true;
}

// A single-pixel retreat after a stroke can be the start of a slow return.
// Keep the turning sample once the preceding travel exceeds sensor jitter.
static bool turns(int start, int middle, int end, int origin, int rest, int drag, int jitter)
{
    if (!((start < middle && end < middle) || (start > middle && end > middle))) return false;
    int from = abs(start - origin), peak = abs(middle - origin);
    if (from <= rest) return peak > rest;
    if (from <= drag && peak > drag) return true;
    return abs(middle - start) > jitter;
}

static bool changes_direction(const kvm_touch_point_t origin[2], const local_contacts_t *first, const local_contacts_t *last,
                              const local_contacts_t *next)
{
    static const kvm_gesture_cfg_t gesture = KVM_GESTURE_CFG_DEFAULT;
    int rest = next->count == 1 ? gesture.move_start_px : gesture.two_max_move_px;
    int drag = next->count == 1 ? gesture.dtd_move_px : rest;
    int first_x = 0, first_y = 0, last_x = 0, last_y = 0, next_x = 0, next_y = 0;
    for (unsigned i = 0; i < next->count; ++i) {
        const kvm_touch_point_t *a = &first->points[i], *b = &last->points[i], *c = &next->points[i];
        if (turns(a->x_px, b->x_px, c->x_px, origin[i].x_px, rest, drag, 2) ||
            turns(a->y_px, b->y_px, c->y_px, origin[i].y_px, rest, drag, 2)) return true;
        first_x += a->x_px; first_y += a->y_px;
        last_x += b->x_px; last_y += b->y_px;
        next_x += c->x_px; next_y += c->y_px;
    }
    // Opposing fingers can each continue in their original direction while
    // their center, which drives scrolling, reverses.
    int center_rest = 2 * gesture.scroll_start_px;
    if (next->count != 2) return false;
    return turns(first_x, last_x, next_x, origin[0].x_px + origin[1].x_px, center_rest, center_rest, 4) ||
           turns(first_y, last_y, next_y, origin[0].y_px + origin[1].y_px, center_rest, center_rest, 4);
}

void local_contacts_expire(local_contacts_queue_t *q, uint64_t now_us)
{
    if (!q->blocked && q->last.count && now_us > q->last_time &&
        now_us - q->last_time > KVM_TOUCH_GAP_CANCEL_US) local_contacts_cancel(q);
}

void local_contacts_post(local_contacts_queue_t *q, const kvm_touch_sample_t *s, local_pointer_t pointer)
{
    bool gap = q->last.count && (s->sample_seq != q->last_seq + 1 ||
                                 s->timestamp_us - q->last_time > KVM_TOUCH_GAP_CANCEL_US);
    q->last_seq = s->sample_seq;
    q->last_time = s->timestamp_us;
    if (!s->valid || s->point_count > 2) {
        if (!q->blocked) local_contacts_cancel(q);
        return;
    }
    if (gap && !q->blocked) local_contacts_cancel(q);
    if (q->blocked && s->point_count) return;
    q->blocked = false;
    local_contacts_t frame = {.timestamp_us=s->timestamp_us, .count=s->point_count, .pointer=pointer};
    for (unsigned i = 0; i < frame.count; ++i) frame.points[i] = s->point[i];
    if (frame.count == 2) {
        if (frame.points[0].track_id == frame.points[1].track_id) {
            local_contacts_cancel(q);
            return;
        }
        if (frame.points[0].track_id > frame.points[1].track_id) {
            frame.points[0] = s->point[1];
            frame.points[1] = s->point[0];
        }
        if (q->last.count == 2 && !same_contacts(&q->last, &frame)) {
            local_contacts_cancel(q);
            return;
        }
    }
    if (!frame.count && !q->last.count) return;
    if (!same_contacts(&q->last, &frame)) {
        for (unsigned i = 0; i < frame.count; ++i) q->origin[i] = frame.points[i];
    }
    q->last = frame;
    // Keep down/up edges and enough intermediate samples for the gesture gap
    // check when several sensor frames accumulate during a display refresh.
    if (q->used >= 2 && q->pending[q->used - 2].pointer.pressed == pointer.pressed &&
        frame.timestamp_us - q->pending[q->used - 2].timestamp_us <= KVM_TOUCH_GAP_CANCEL_US &&
        q->pending[q->used - 1].pointer.pressed == pointer.pressed && same_contacts(&q->pending[q->used - 2], &frame) &&
        same_contacts(&q->pending[q->used - 1], &frame) &&
        !changes_direction(q->origin, &q->pending[q->used - 2], &q->pending[q->used - 1], &frame)) {
        q->pending[q->used - 1] = frame;
    } else if (q->used == sizeof(q->pending) / sizeof(q->pending[0])) {
        local_contacts_cancel(q);
    } else {
        q->pending[q->used++] = frame;
    }
}
