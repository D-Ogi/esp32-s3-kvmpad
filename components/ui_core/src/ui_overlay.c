// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Overlay stack of the router shell (TASK-060C, 07 §5). See ui_overlay.h for the contract.
#include "ui_overlay.h"

#include "ui_nav_internal.h"

static const ui_overlay_descriptor_t *find(const ui_router_t *r, uint16_t id)
{
    for (uint8_t i = 0; i < r->overlay_count; i++) {
        if (r->overlays[i].id == id) {
            return &r->overlays[i];
        }
    }
    return NULL;
}

static bool is_open(const ui_router_t *r, uint16_t id)
{
    for (uint8_t i = 0; i < r->depth; i++) {
        if (r->stack[i].d->id == id) {
            return true;
        }
    }
    return false;
}

bool ui_overlay_blocking(const ui_router_t *r)
{
    if (r == NULL || !r->ready) {
        return false;
    }
    for (uint8_t i = 0; i < r->depth; i++) {
        if (r->stack[i].d->blocking) {
            return true;
        }
    }
    return false;
}

uint16_t ui_overlay_top(const ui_router_t *r)
{
    return r != NULL && r->ready && r->depth > 0 ? r->stack[r->depth - 1].d->id : UI_OVERLAY_NONE;
}

static void publish(ui_router_t *r, bool was_blocking)
{
    lv_obj_set_hidden(r->overlay_layer, r->depth == 0);
    if (r->store != NULL) {
        ui_store_set_overlay(r->store, ui_overlay_top(r));
    }
    if (was_blocking != ui_overlay_blocking(r)) {
        ui_nav_area_changed(r); // the pad was taken away or given back
    }
}

bool ui_overlay_show(ui_router_t *r, uint16_t overlay_id)
{
    const ui_overlay_descriptor_t *d = r != NULL && r->ready ? find(r, overlay_id) : NULL;
    if (d == NULL || r->busy || is_open(r, overlay_id) || r->depth >= UI_OVERLAY_STACK_MAX) {
        if (r != NULL) {
            r->c.overlay_refused++;
        }
        return false;
    }
    r->busy = true;
    const bool was_blocking = ui_overlay_blocking(r);
    if (d->blocking && !was_blocking) {
        ui_nav_cancel(r, UI_CANCEL_OVERLAY_OPEN); // a held key, drag or press below must not survive the modal
    }

    lv_obj_t *scrim = lv_obj_create(r->overlay_layer);
    lv_obj_t *card = scrim != NULL ? lv_obj_create(scrim) : NULL;
    bool ok = card != NULL;
    if (ok) {
        lv_obj_remove_style_all(scrim);
        lv_obj_set_size(scrim, lv_pct(100), lv_pct(100));
        lv_obj_set_scrollable(scrim, false);
        if (d->blocking) {
            // absorbs every touch outside the card: nothing below gets a press or a click
            lv_obj_set_clickable(scrim, true);
            lv_obj_set_style_bg_color(scrim, lv_color_hex(r->scrim_color), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(scrim, r->scrim_opa, LV_PART_MAIN);
        } else {
            lv_obj_set_clickable(scrim, false); // touches outside the card reach the view
        }
        lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_center(card);
        ok = d->create(d->ctx, card);
    }
    if (!ok) {
        if (scrim != NULL) {
            lv_obj_delete(scrim);
        }
        r->c.overlay_refused++;
        r->busy = false;
        publish(r, was_blocking); // the pad was never taken away: nothing to give back
        return false;
    }
    r->stack[r->depth].d = d;
    r->stack[r->depth].scrim = scrim;
    r->depth++;
    r->c.overlays_shown++;
    r->busy = false;
    publish(r, was_blocking);
    return true;
}

static void pop(ui_router_t *r, bool sync)
{
    r->depth--;
    const ui_overlay_descriptor_t *d = r->stack[r->depth].d;
    lv_obj_t *scrim = r->stack[r->depth].scrim;
    r->stack[r->depth].d = NULL;
    r->stack[r->depth].scrim = NULL;
    if (sync) {
        lv_obj_delete(scrim);
    } else {
        // may run inside a click handler of an object in this overlay: hide now, delete after the event
        lv_obj_set_hidden(scrim, true);
        lv_obj_delete_async(scrim);
    }
    r->c.overlays_dismissed++;
    if (d->on_close != NULL) {
        d->on_close(d->ctx);
    }
}

void ui_overlay_dismiss(ui_router_t *r)
{
    if (r == NULL || !r->ready || r->busy || r->depth == 0) {
        return;
    }
    const bool was_blocking = ui_overlay_blocking(r);
    pop(r, false);
    publish(r, was_blocking);
}

void ui_overlay_dismiss_all(ui_router_t *r)
{
    if (r == NULL || !r->ready || r->busy || r->depth == 0) {
        return;
    }
    ui_nav_overlays_clear(r, false);
}

void ui_nav_overlays_clear(ui_router_t *r, bool sync)
{
    if (r->depth == 0) {
        return;
    }
    const bool was_blocking = ui_overlay_blocking(r);
    while (r->depth > 0) {
        pop(r, sync);
    }
    publish(r, was_blocking);
}
