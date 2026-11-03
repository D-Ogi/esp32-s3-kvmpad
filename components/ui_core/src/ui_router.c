// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Router and root shell (TASK-060C, 07 §5). See ui_router.h for the contract.
#include "ui_router.h"

#include <string.h>

#include "ui_nav_internal.h"

static lv_obj_t *plain_layer(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    if (o == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(o); // transparent, no padding/border: a pure container
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    // Layers are never touch targets themselves: only their children (widgets, the modal scrim) are.
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static bool valid_view_set(const ui_view_descriptor_t *views, uint8_t n)
{
    bool seen[UI_VIEW_COUNT] = {false};
    for (uint8_t i = 0; i < n; i++) {
        const ui_view_descriptor_t *d = &views[i];
        if ((unsigned)d->id >= UI_VIEW_COUNT || d->id == UI_VIEW_SETTINGS || seen[d->id] || d->create == NULL) {
            return false;
        }
        seen[d->id] = true;
    }
    return true;
}

static bool valid_overlay_set(const ui_overlay_descriptor_t *ov, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        if (ov[i].id == UI_OVERLAY_NONE || ov[i].create == NULL) {
            return false;
        }
        for (uint8_t j = 0; j < i; j++) {
            if (ov[j].id == ov[i].id) {
                return false;
            }
        }
    }
    return true;
}

bool ui_nav_cancel(ui_router_t *r, ui_cancel_reason_t why)
{
    r->c.cancel_requests++;
    bool ok = r->hooks.cancel_input == NULL || r->hooks.cancel_input(r->hooks.ctx, why);
    if (!ok) {
        r->c.cancel_refused++; // the HID core still releases on its own; the switch goes on
    }
    return ok;
}

void ui_nav_area_changed(ui_router_t *r)
{
    if (r->hooks.input_area_changed != NULL) {
        r->hooks.input_area_changed(r->hooks.ctx);
    }
}

bool ui_router_init(ui_router_t *r, lv_obj_t *parent, const ui_shell_layout_t *layout, ui_store_t *store,
                    const ui_view_descriptor_t *views, uint8_t n_views, const ui_overlay_descriptor_t *overlays,
                    uint8_t n_overlays, const ui_nav_hooks_t *hooks)
{
    if (r == NULL || parent == NULL || layout == NULL || (views == NULL && n_views > 0) ||
        (overlays == NULL && n_overlays > 0) || layout->width <= 0 || layout->height <= 0 || layout->topbar_h < 0 ||
        layout->topbar_h >= layout->height || !valid_view_set(views, n_views) ||
        !valid_overlay_set(overlays, n_overlays)) {
        return false;
    }
    memset(r, 0, sizeof(*r));
    r->active = -1;
    r->root = parent;
    r->store = store;
    if (hooks != NULL) {
        r->hooks = *hooks;
    }
    for (uint8_t i = 0; i < n_views; i++) {
        r->views[views[i].id] = &views[i];
    }
    r->overlays = overlays;
    r->overlay_count = n_overlays;
    r->scrim_color = layout->scrim_opa != 0u ? layout->scrim_color : 0x000000u;
    r->scrim_opa = layout->scrim_opa != 0u ? layout->scrim_opa : (uint8_t)LV_OPA_50;

    const int32_t w = layout->width;
    const int32_t h = layout->height;
    r->topbar_slot = plain_layer(parent, 0, 0, w, layout->topbar_h);
    r->content_slot = plain_layer(parent, 0, layout->topbar_h, w, h - layout->topbar_h);
    r->overlay_layer = plain_layer(parent, 0, 0, w, h);
    r->toast_layer = plain_layer(parent, 0, 0, w, h);
    if (r->topbar_slot == NULL || r->content_slot == NULL || r->overlay_layer == NULL || r->toast_layer == NULL) {
        lv_obj_t *made[4] = {r->topbar_slot, r->content_slot, r->overlay_layer, r->toast_layer};
        for (int i = 0; i < 4; i++) {
            if (made[i] != NULL) {
                lv_obj_delete(made[i]);
            }
        }
        memset(r, 0, sizeof(*r));
        return false;
    }
    lv_obj_set_hidden(r->overlay_layer, true); // empty stack: nothing drawn, nothing caught
    r->ready = true;
    return true;
}

void ui_router_deinit(ui_router_t *r)
{
    if (r == NULL || !r->ready) {
        return;
    }
    ui_nav_overlays_clear(r, true);
    if (r->active >= 0 && r->views[r->active] != NULL) {
        ui_nav_cancel(r, UI_CANCEL_SHUTDOWN);
        if (r->views[r->active]->on_leave != NULL) {
            r->views[r->active]->on_leave(r->views[r->active]->ctx);
        }
    }
    for (int i = 0; i < UI_VIEW_COUNT; i++) {
        if (r->view_root[i] != NULL) {
            if (r->views[i]->destroy != NULL) {
                r->views[i]->destroy(r->views[i]->ctx);
            }
            lv_obj_delete(r->view_root[i]);
            r->view_root[i] = NULL;
        }
    }
    lv_obj_delete(r->toast_layer);
    lv_obj_delete(r->overlay_layer);
    lv_obj_delete(r->content_slot);
    lv_obj_delete(r->topbar_slot);
    r->ready = false;
    r->active = -1;
    ui_nav_area_changed(r); // no view, no pad
}

bool ui_router_show(ui_router_t *r, ui_view_id_t target)
{
    if (r == NULL || !r->ready || r->busy || (unsigned)target >= UI_VIEW_COUNT || r->views[target] == NULL ||
        ui_overlay_blocking(r)) {
        if (r != NULL) {
            r->c.show_refused++;
        }
        return false;
    }
    if (r->active == (int16_t)target) {
        r->c.show_noop++;
        return true;
    }
    r->busy = true;
    const ui_view_descriptor_t *nd = r->views[target];

    // Create first (lazily, once): a failed create leaves the old view fully active, nothing cancelled.
    if (r->view_root[target] == NULL) {
        lv_obj_t *root = nd->create(nd->ctx, r->content_slot);
        if (root == NULL) {
            r->c.show_refused++;
            r->busy = false;
            return false;
        }
        lv_obj_set_hidden(root, true);
        r->view_root[target] = root;
        r->c.views_created++;
    }

    // 1. cancel input + request the HID release (never assumed successful)
    ui_nav_cancel(r, UI_CANCEL_VIEW_CHANGE);
    // 2. leave and hide the old view
    if (r->active >= 0) {
        const ui_view_descriptor_t *od = r->views[r->active];
        if (od->on_leave != NULL) {
            od->on_leave(od->ctx);
        }
        lv_obj_set_hidden(r->view_root[r->active], true);
    }
    // 3. show and enter the new view
    lv_obj_set_hidden(r->view_root[target], false);
    r->active = (int16_t)target;
    if (nd->on_enter != NULL) {
        nd->on_enter(nd->ctx);
    }
    // 4. publish, then let the touch router re-read the pad
    if (r->store != NULL) {
        ui_store_set_active_view(r->store, target);
    }
    r->c.shows++;
    r->busy = false;
    ui_nav_area_changed(r);
    return true;
}

int ui_router_active(const ui_router_t *r)
{
    return r != NULL && r->ready ? r->active : -1;
}

bool ui_router_pad_area(ui_router_t *r, lv_area_t *out)
{
    if (r == NULL || out == NULL || !r->ready || r->active < 0 || ui_overlay_blocking(r)) {
        return false;
    }
    const ui_view_descriptor_t *d = r->views[r->active];
    lv_obj_t *pad = d->pad_obj != NULL ? d->pad_obj(d->ctx) : NULL;
    if (pad == NULL || lv_obj_is_hidden(pad)) {
        return false;
    }
    lv_obj_update_layout(pad);
    lv_obj_get_coords(pad, out);
    return out->x2 >= out->x1 && out->y2 >= out->y1;
}
