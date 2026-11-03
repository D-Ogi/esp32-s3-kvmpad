// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// See sim_checks.h.
#include "sim_checks.h"

#include <stdio.h>
#include <string.h>

#include "host_lv_mem.h"
#include "sim_fixture.h"
#include "ui_action_key.h"
#include "ui_modal.h"
#include "ui_overlay.h"
#include "ui_segmented.h"
#include "ui_tokens.h"
#include "ui_topbar.h"
#include "ui_view_status.h"

#define SIM_OBJ_MAX 512

typedef struct {
    lv_obj_t *obj[SIM_OBJ_MAX];
    lv_area_t area[SIM_OBJ_MAX];
    uint32_t n;
    uint32_t overflow;
} obj_set_t;

// ---- object tree -------------------------------------------------------------------------------------------------

/**
 * The object that owns the input right now: the blocking card on top, else the shell screen. A press cannot
 * reach anything outside it, so only its subtree can hold a hit target.
 */
static lv_obj_t *input_root(sim_app_t *s)
{
    if (s->app.shortcuts.modal) {
        return s->app.shortcuts.modal; // FR-734 confirmation: its own scrim, outside the router stack
    }
    if (ui_overlay_blocking(&s->app.router) && s->app.router.depth > 0) {
        return s->app.router.stack[s->app.router.depth - 1].scrim;
    }
    return s->app.screen;
}

static bool has_area(const lv_area_t *a)
{
    return a->x2 >= a->x1 && a->y2 >= a->y1;
}

static bool is_ancestor(const lv_obj_t *maybe_parent, lv_obj_t *obj)
{
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        if (p == maybe_parent) {
            return true;
        }
    }
    return false;
}

/** Whether `obj` itself or any ancestor is hidden right now (the walk's `hidden_branch`, recomputed). */
static bool in_hidden_branch(lv_obj_t *obj)
{
    for (lv_obj_t *p = obj; p != NULL; p = lv_obj_get_parent(p)) {
        if (lv_obj_is_hidden(p)) {
            return true;
        }
    }
    return false;
}

static bool overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static void add(obj_set_t *set, lv_obj_t *obj)
{
    if (set->n >= SIM_OBJ_MAX) {
        set->overflow++;
        return;
    }
    lv_obj_get_coords(obj, &set->area[set->n]);
    set->obj[set->n] = obj;
    set->n++;
}

/** Walk `root`'s descendants; `hidden_branch` stays true once an ancestor is hidden. */
static void walk(lv_obj_t *root, bool hidden_branch, obj_set_t *visible_hits, obj_set_t *hidden_hits,
                 obj_set_t *labels)
{
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        const bool hidden = hidden_branch || lv_obj_is_hidden(child);
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        if (has_area(&a)) {
            if (lv_obj_is_clickable(child)) {
                add(hidden ? hidden_hits : visible_hits, child);
            } else if (!hidden && labels && lv_obj_check_type(child, &lv_label_class)) {
                add(labels, child);
            }
        }
        walk(child, hidden, visible_hits, hidden_hits, labels);
    }
}

// ---- AT-UIFW-01 --------------------------------------------------------------------------------------------------

bool sim_check_geometry(sim_app_t *s, const char *fixture, sim_check_result_t *out)
{
    static obj_set_t hits, hidden, labels;
    memset(&hits, 0, sizeof(hits));
    memset(&hidden, 0, sizeof(hidden));
    memset(&labels, 0, sizeof(labels));

    lv_obj_t *root = input_root(s);
    lv_obj_update_layout(s->app.screen);
    walk(root, false, &hits, &hidden, &labels);

    bool ok = true;
    for (uint32_t i = 0; i < hits.n; i++) {
        const lv_area_t *a = &hits.area[i];
        const int32_t w = a->x2 - a->x1 + 1;
        const int32_t h = a->y2 - a->y1 + 1;
        if (w < UI_TOUCH_MIN || h < UI_TOUCH_MIN) {
            printf("    %s: AT-UIFW-01 hit box %ux%u at [%d..%d]x[%d..%d] is below %d px\n", fixture, (unsigned)w,
                   (unsigned)h, (int)a->x1, (int)a->x2, (int)a->y1, (int)a->y2, UI_TOUCH_MIN);
            out->too_small++;
            ok = false;
        }
        if (a->x1 < 0 || a->y1 < 0 || a->x2 >= UI_SCREEN_W || a->y2 >= UI_SCREEN_H) {
            printf("    %s: AT-UIFW-01 hit box [%d..%d]x[%d..%d] leaves the %dx%d panel\n", fixture, (int)a->x1,
                   (int)a->x2, (int)a->y1, (int)a->y2, UI_SCREEN_W, UI_SCREEN_H);
            out->out_of_bounds++;
            ok = false;
        }
        for (uint32_t j = i + 1; j < hits.n; j++) {
            if (is_ancestor(hits.obj[i], hits.obj[j]) || is_ancestor(hits.obj[j], hits.obj[i])) {
                continue; // a hit target inside another one is nesting, not an overlap
            }
            if (overlaps(a, &hits.area[j])) {
                printf("    %s: AT-UIFW-01 hit boxes [%d..%d]x[%d..%d] and [%d..%d]x[%d..%d] overlap\n", fixture,
                       (int)a->x1, (int)a->x2, (int)a->y1, (int)a->y2, (int)hits.area[j].x1, (int)hits.area[j].x2,
                       (int)hits.area[j].y1, (int)hits.area[j].y2);
                out->overlapping++;
                ok = false;
            }
        }
    }
    // Warning only: a label that is not contained in its parent is clipped or spills over its neighbour.
    for (uint32_t i = 0; i < labels.n; i++) {
        lv_obj_t *parent = lv_obj_get_parent(labels.obj[i]);
        lv_area_t p;
        lv_obj_get_coords(parent, &p);
        const lv_area_t *a = &labels.area[i];
        if (a->x1 < p.x1 || a->y1 < p.y1 || a->x2 > p.x2 || a->y2 > p.y2) {
            printf("    %s: label \"%s\" [%d..%d]x[%d..%d] is not contained in its parent "
                   "[%d..%d]x[%d..%d]\n",
                   fixture, lv_label_get_text(labels.obj[i]), (int)a->x1, (int)a->x2, (int)a->y1, (int)a->y2,
                   (int)p.x1, (int)p.x2, (int)p.y1, (int)p.y2);
            out->label_clipped++;
        }
    }
    if (hits.overflow || hidden.overflow || labels.overflow) {
        printf("    %s: object set overflow (%u/%u/%u dropped, raise SIM_OBJ_MAX)\n", fixture, hits.overflow,
               hidden.overflow, labels.overflow);
        ok = false;
    }
    out->hit_targets = hits.n;
    return ok;
}

// ---- hidden elements ---------------------------------------------------------------------------------------------

bool sim_check_hidden(sim_app_t *s, const char *fixture, sim_check_result_t *out)
{
    static obj_set_t hits, hidden;
    memset(&hits, 0, sizeof(hits));
    memset(&hidden, 0, sizeof(hidden));
    lv_obj_update_layout(s->app.screen);
    walk(input_root(s), false, &hits, &hidden, NULL);

    bool ok = true;
    for (uint32_t i = 0; i < hidden.n; i++) {
        lv_obj_t *obj = hidden.obj[i];
        const lv_area_t *a = &hidden.area[i];
        if (a->x1 < 0 || a->y1 < 0 || a->x2 >= UI_SCREEN_W || a->y2 >= UI_SCREEN_H) {
            continue; // not reachable by a finger at all
        }
        if (!in_hidden_branch(obj)) {
            // An earlier probe pressed the visible control under a hidden one, that control acted (which the
            // check allows), and the branch this object sits in is on screen now: it is not a hidden object
            // any more, so pressing it proves nothing.
            continue;
        }
        out->hidden_probed++;
        sim_press_point(a->x1 + (a->x2 - a->x1) / 2, a->y1 + (a->y2 - a->y1) / 2);
        // A hidden object may neither take the press (LV_STATE_PRESSED) nor emit a down edge
        // (ui_action_key's LV_STATE_USER_1). Whatever visible object is underneath may react; that is
        // correct and is not what this check is about.
        if (lv_obj_has_state(obj, LV_STATE_PRESSED) || lv_obj_has_state(obj, LV_STATE_USER_1)) {
            printf("    %s: a hidden object at [%d..%d]x[%d..%d] took a press\n", fixture, (int)a->x1,
                   (int)a->x2, (int)a->y1, (int)a->y2);
            out->hidden_reached++;
            ok = false;
        }
        sim_release();
    }
    return ok;
}

// ---- AT-UIFW-02 --------------------------------------------------------------------------------------------------

static uint32_t observer_total(sim_app_t *s)
{
    uint32_t n = 0;
    for (int id = 0; id < UI_SUBJ_COUNT; id++) {
        lv_subject_t *subj = ui_store_subject(&s->store, (ui_subject_id_t)id);
        if (subj) {
            n += lv_ll_get_len(&subj->subs_ll);
        }
    }
    return n;
}

bool sim_check_at02(sim_app_t *s, uint32_t switches)
{
    // One full round first, so every view is created and every glyph is rasterized before the baseline.
    for (uint8_t i = 0; i < s->app.nav_desc.count; i++) {
        sim_submit(s, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, (uint16_t)s->app.segment_view[i]);
    }
    lv_test_wait(20);
    const size_t base_alloc = host_lv_live_count();
    const uint32_t base_obs = observer_total(s);
    const uint32_t base_created = s->app.router.c.views_created;

    for (uint32_t i = 0; i < switches; i++) {
        const uint16_t view = (uint16_t)s->app.segment_view[i % s->app.nav_desc.count];
        if (!sim_submit(s, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, view)) {
            printf("    AT-UIFW-02: switch %u to view %u refused\n", i, view);
            return false;
        }
        if ((i % 64u) == 0u) {
            lv_test_wait(2);
        }
    }
    lv_test_wait(20);
    const size_t alloc = host_lv_live_count();
    const uint32_t obs = observer_total(s);
    printf("    AT-UIFW-02: %u view switches, live allocations %zu -> %zu, observers %u -> %u, "
           "views created %u -> %u\n",
           switches, base_alloc, alloc, base_obs, obs, base_created, s->app.router.c.views_created);
    bool ok = true;
    if (alloc != base_alloc) {
        printf("    AT-UIFW-02: live allocation count grew by %zd\n", (ptrdiff_t)alloc - (ptrdiff_t)base_alloc);
        ok = false;
    }
    if (obs != base_obs) {
        printf("    AT-UIFW-02: observer count changed\n");
        ok = false;
    }
    if (s->app.router.c.views_created != base_created) {
        printf("    AT-UIFW-02: a view was rebuilt after the first visit\n");
        ok = false;
    }
    return ok;
}

// ---- AT-UIFW-04 --------------------------------------------------------------------------------------------------

bool sim_check_at04(sim_app_t *s)
{
    if (!sim_submit(s, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_MOUSE)) {
        printf("    AT-UIFW-04: cannot reach the MOUSE view\n");
        return false;
    }
    lv_test_wait(20);
    lv_area_t pad;
    if (!ui_router_pad_area(&s->app.router, &pad) || !s->pad_on) {
        printf("    AT-UIFW-04: the MOUSE view reports no pad rectangle\n");
        return false;
    }
    sim_log_reset(s);
    const int before_view = ui_router_active(&s->app.router);
    const int32_t before_tab = ui_segmented_get_active(s->app.nav);

    sim_press_point(pad.x1 + (pad.x2 - pad.x1) / 2, pad.y1 + (pad.y2 - pad.y1) / 2);
    lv_test_wait(200); // long enough for a long-press or a click to fire, if the pad were an LVGL target
    bool ok = true;
    if (sim_domain_actions(s) != 0) {
        printf("    AT-UIFW-04: a press on the touch surface produced %u action(s)\n", sim_domain_actions(s));
        ok = false;
    }
    for (uint16_t i = 0; i < s->app.nav_desc.count; i++) {
        if (lv_obj_has_state(ui_segmented_tab(s->app.nav, i), LV_STATE_PRESSED)) {
            printf("    AT-UIFW-04: mode switcher tab %u is pressed while the pad is pressed\n", i);
            ok = false;
        }
    }
    sim_release();
    if (sim_domain_actions(s) != 0) {
        printf("    AT-UIFW-04: releasing the touch surface produced %u action(s)\n", sim_domain_actions(s));
        ok = false;
    }
    if (ui_router_active(&s->app.router) != before_view ||
        ui_segmented_get_active(s->app.nav) != before_tab) {
        printf("    AT-UIFW-04: the pad press changed the active view / tab\n");
        ok = false;
    }
    if (ok) {
        printf("    AT-UIFW-04: pad [%d..%d]x[%d..%d] pressed and released: no action, no phantom tab click\n",
               (int)pad.x1, (int)pad.x2, (int)pad.y1, (int)pad.y2);
    }
    return ok;
}

// ---- AT-UIFW-08 --------------------------------------------------------------------------------------------------

bool sim_check_at08(sim_app_t *s)
{
    if (!sim_submit(s, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_KEYBOARD)) {
        printf("    AT-UIFW-08: cannot reach the KEYBOARD view\n");
        return false;
    }
    lv_test_wait(20);
    uint8_t usage = 0;
    const uint32_t keys = ui_view_keyboard_key_count();
    for (uint32_t i = 0; i < keys; i++) {
        const char *name = ui_view_keyboard_key_name(i);
        if (name && strcmp(name, "A") == 0) {
            usage = ui_view_keyboard_key_usage(i);
        }
    }
    lv_obj_t *key = sim_keyboard_key(s, "A");
    if (!key || usage == 0) {
        printf("    AT-UIFW-08: the KEYBOARD view has no \"A\" cap\n");
        return false;
    }
    sim_log_reset(s);
    sim_press_obj(key);
    if (sim_count_action(s, UI_ACT_KEY_DOWN, usage) != 1u) {
        printf("    AT-UIFW-08: holding \"A\" did not submit exactly one KEY_DOWN\n");
        return false;
    }
    // Forced switch while the finger is still down: the gateway is the only way in, a second touch is impossible.
    sim_submit(s, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_MOUSE);
    lv_test_wait(20);
    bool ok = true;
    const uint32_t ups = sim_count_action(s, UI_ACT_KEY_UP, usage);
    const uint32_t up_seq = sim_first_action_seq(s, UI_ACT_KEY_UP, usage);
    const uint32_t pad_seq = sim_last_pad_seq(s);
    if (ups != 1u) {
        printf("    AT-UIFW-08: %u KEY_UP for the held key (expected 1)\n", ups);
        ok = false;
    }
    if (up_seq == 0u || pad_seq == 0u || up_seq >= pad_seq) {
        printf("    AT-UIFW-08: KEY_UP (seq %u) did not precede the new input area (seq %u)\n", up_seq, pad_seq);
        ok = false;
    }
    bool cancelled_first = false;
    for (uint32_t i = 0; i < s->log_n; i++) {
        if (s->log[i].kind == SIM_LOG_CANCEL && s->log[i].cancel_why == UI_CANCEL_VIEW_CHANGE &&
            s->log[i].seq < up_seq) {
            cancelled_first = true;
        }
    }
    if (!cancelled_first) {
        printf("    AT-UIFW-08: the view change did not ask for a touch cancel / HID release first\n");
        ok = false;
    }
    if (ui_router_active(&s->app.router) != UI_VIEW_MOUSE) {
        printf("    AT-UIFW-08: the forced switch did not complete\n");
        ok = false;
    }
    sim_release();
    if (sim_count_action(s, UI_ACT_KEY_UP, usage) != ups) {
        printf("    AT-UIFW-08: the late release produced a second KEY_UP\n");
        ok = false;
    }
    if (ok) {
        printf("    AT-UIFW-08: KEY_UP(0x%02X) at seq %u, cancel before it, new input area at seq %u, "
               "no second UP\n",
               usage, up_seq, pad_seq);
    }
    return ok;
}

// ---- AT-UIFW-10 --------------------------------------------------------------------------------------------------

bool sim_check_at10(sim_app_t *s, uint32_t cycles)
{
    sim_submit(s, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_MOUSE);
    lv_test_wait(20);
    // One cycle before the baseline: the first card of a fresh shell creates the long-lived renderer state the
    // measured cycles must then leave alone.
    sim_click_obj(ui_topbar_status_item(s->app.bar, UI_TOPBAR_HOST));
    sim_submit(s, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0);
    lv_test_wait(20);
    const size_t base_alloc = host_lv_live_count();
    for (uint32_t i = 0; i < cycles; i++) {
        sim_click_obj(ui_topbar_status_item(s->app.bar, UI_TOPBAR_HOST));
        if (ui_overlay_top(&s->app.router) != UI_OVERLAY_HOST_DETAILS) {
            printf("    AT-UIFW-10: cycle %u did not open the HOST details card\n", i);
            return false;
        }
        if (s->pad_on) {
            printf("    AT-UIFW-10: cycle %u kept the pad while a blocking card was open\n", i);
            return false;
        }
        sim_submit(s, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0);
        lv_test_wait(10);
        if (ui_overlay_top(&s->app.router) != UI_OVERLAY_NONE) {
            printf("    AT-UIFW-10: cycle %u did not close the card\n", i);
            return false;
        }
        if (!s->pad_on) {
            printf("    AT-UIFW-10: cycle %u closed the card but the pad stayed blocked\n", i);
            return false;
        }
        sim_log_reset(s);
    }
    lv_test_wait(20);
    const size_t alloc = host_lv_live_count();
    printf("    AT-UIFW-10: %u open/close cycles, pad available after each, live allocations %zu -> %zu\n",
           cycles, base_alloc, alloc);
    if (alloc != base_alloc) {
        printf("    AT-UIFW-10: live allocation count grew by %zd\n", (ptrdiff_t)alloc - (ptrdiff_t)base_alloc);
        return false;
    }
    return true;
}
