// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Touch router (TASK-060E). See touch_router.h.
#include "touch_router.h"

#include <string.h>

void touch_router_init(touch_router_t *r)
{
    memset(r, 0, sizeof(*r));
}

void touch_router_set_pad(touch_router_t *r, const touch_rect_t *pad, bool enabled)
{
    if (pad) {
        r->pad = *pad;
    }
    r->pad_enabled = enabled && pad != NULL && r->pad.x2 >= r->pad.x1 && r->pad.y2 >= r->pad.y1;
}

static bool in_rect(const touch_rect_t *rc, int16_t x, int16_t y)
{
    return x >= rc->x1 && x <= rc->x2 && y >= rc->y1 && y <= rc->y2;
}

static touch_contact_t *find(touch_router_t *r, uint8_t id)
{
    for (int i = 0; i < KVM_TOUCH_MAX_POINTS; i++) {
        if (r->c[i].used && r->c[i].id == id) {
            return &r->c[i];
        }
    }
    return NULL;
}

static bool any_target(const touch_router_t *r, touch_target_t t)
{
    for (int i = 0; i < KVM_TOUCH_MAX_POINTS; i++) {
        if (r->c[i].used && r->c[i].target == t) {
            return true;
        }
    }
    return false;
}

touch_target_t touch_router_target(const touch_router_t *r, uint8_t track_id)
{
    for (int i = 0; i < KVM_TOUCH_MAX_POINTS; i++) {
        if (r->c[i].used && r->c[i].id == track_id) {
            return r->c[i].target;
        }
    }
    return TOUCH_TARGET_NONE;
}

void touch_router_cancel(touch_router_t *r)
{
    r->st.cancels++;
    for (int i = 0; i < KVM_TOUCH_MAX_POINTS; i++) {
        if (r->c[i].used) {
            r->c[i].target = TOUCH_TARGET_BLOCKED;
        }
    }
    if (r->ptr_active) {
        r->ptr_active = false;
        r->cancel_pending = true;
    }
}

void touch_router_route(touch_router_t *r, const kvm_touch_sample_t *s, touch_route_out_t *out)
{
    r->st.samples++;
    memset(out, 0, sizeof(*out));
    out->pad.timestamp_us = s->timestamp_us;
    out->pad.sample_seq = s->sample_seq;
    out->pad.valid = s->valid;

    if (!s->valid || s->point_count > KVM_TOUCH_MAX_POINTS) {
        // controller error: no target survives, the LVGL press is cancelled (never a click)
        r->st.invalid++;
        if (r->ptr_active) {
            r->ptr_active = false;
            r->cancel_pending = true;
        }
        memset(r->c, 0, sizeof(r->c));
        out->pad.valid = false;
        out->pad.point_count = 0;
    } else {
        // 1. contacts that lifted
        for (int i = 0; i < KVM_TOUCH_MAX_POINTS; i++) {
            if (!r->c[i].used) {
                continue;
            }
            bool present = false;
            for (uint8_t k = 0; k < s->point_count; k++) {
                if (s->point[k].track_id == r->c[i].id) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                if (r->ptr_active && r->ptr_id == r->c[i].id) {
                    r->ptr_active = false; // normal release: LVGL decides about the click
                }
                r->c[i].used = false;
            }
        }
        // 2. new contacts are classified once; held ones keep their target
        for (uint8_t k = 0; k < s->point_count; k++) {
            const kvm_touch_point_t *p = &s->point[k];
            touch_contact_t *c = find(r, p->track_id);
            if (!c) {
                for (int i = 0; i < KVM_TOUCH_MAX_POINTS; i++) {
                    if (!r->c[i].used) {
                        c = &r->c[i];
                        break;
                    }
                }
                if (!c) {
                    continue; // cannot happen with <= 5 points and unique ids
                }
                c->used = true;
                c->id = p->track_id;
                c->target = TOUCH_TARGET_NONE; // the slot may hold a lifted contact's target
                const bool on_pad = r->pad_enabled && in_rect(&r->pad, p->x_px, p->y_px);
                if (on_pad) {
                    c->target = r->ptr_active ? TOUCH_TARGET_BLOCKED : TOUCH_TARGET_PAD;
                } else {
                    c->target =
                        (r->ptr_active || any_target(r, TOUCH_TARGET_PAD)) ? TOUCH_TARGET_BLOCKED : TOUCH_TARGET_CHROME;
                }
                switch (c->target) {
                case TOUCH_TARGET_PAD: r->st.pad_downs++; break;
                case TOUCH_TARGET_CHROME:
                    r->st.chrome_downs++;
                    r->ptr_active = true;
                    r->ptr_id = c->id;
                    break;
                default: r->st.blocked_downs++; break;
                }
            }
            if (c->target == TOUCH_TARGET_CHROME && r->ptr_active && r->ptr_id == c->id) {
                r->ptr_x = p->x_px;
                r->ptr_y = p->y_px;
            } else if (c->target == TOUCH_TARGET_PAD) {
                out->pad.point[out->pad.point_count++] = *p;
            }
        }
    }

    out->ptr_pressed = r->ptr_active;
    out->ptr_x = r->ptr_x;
    out->ptr_y = r->ptr_y;
    if (r->cancel_pending) {
        out->ptr_cancel = true;
        r->cancel_pending = false;
        r->st.pointer_cancels++;
    }
}

const char *touch_target_name(touch_target_t t)
{
    switch (t) {
    case TOUCH_TARGET_NONE: return "NONE";
    case TOUCH_TARGET_CHROME: return "CHROME";
    case TOUCH_TARGET_PAD: return "PAD";
    case TOUCH_TARGET_BLOCKED: return "BLOCKED";
    }
    return "?";
}
