// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-060C with the real LVGL 9.6 (headless 1024x600 test display, LVGL test pointer, address sanitizer):
// - atomic view switch order: cancel input + HID release request -> on_leave -> hide -> show -> on_enter ->
//   publish active_view -> pad re-read (07 §5, AGENTS-UI §6);
// - navigation with a held key / drag leaves nothing DOWN, also when the release request is refused
//   (AT-UIFW-08 at router level);
// - lazy creation, cache of at most three views, re-entrancy and create failures;
// - overlay stack: blocking scrim absorbs clicks, the pad is taken away and given back, LIFO, no duplicates,
//   dismiss from a click handler inside the overlay (AT-UIFW-10, 1000 cycles);
// - 10 000 view switches and observer lifetime across deinit orders without live-allocation growth
//   (AT-UIFW-02, AT-UIFW-09).
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_overlay.h"
#include "ui_present.h"
#include "ui_router.h"
#include "ui_store.h"

#define SCR_W 1024
#define SCR_H 600
#define TOP_H 64

// ---- call log ------------------------------------------------------------------------------
typedef enum {
    EV_CANCEL = 1,
    EV_LEAVE,
    EV_ENTER,
    EV_AREA,
    EV_CREATE,
    EV_DESTROY,
    EV_OV_CLOSE,
} ev_t;

typedef struct {
    ev_t ev;
    int arg;
    int32_t active_view_subject; // value of the store subject at the time of the call
} log_entry_t;

static log_entry_t s_log[64];
static int s_log_n;
static ui_mailbox_t s_mb;
static ui_store_t s_store;
static bool s_store_on;

static void log_ev(ev_t ev, int arg)
{
    if (s_log_n < (int)(sizeof(s_log) / sizeof(s_log[0]))) {
        s_log[s_log_n].ev = ev;
        s_log[s_log_n].arg = arg;
        s_log[s_log_n].active_view_subject = s_store_on ? ui_store_get(&s_store, UI_SUBJ_ACTIVE_VIEW) : -2;
        s_log_n++;
    }
}

// ---- fake domain: what the HOST holds ------------------------------------------------------
static int s_keys_down;      // keys reported DOWN to the HOST
static bool s_button_down;   // drag in progress
static bool s_refuse_cancel; // the HID queue refuses the release request
static int s_cancels[3];

static bool hook_cancel(void *ctx, ui_cancel_reason_t why)
{
    (void)ctx;
    log_ev(EV_CANCEL, (int)why);
    s_cancels[why]++;
    if (s_refuse_cancel) {
        return false;
    }
    s_keys_down = 0; // RELEASE_ALL accepted by the HID core
    s_button_down = false;
    return true;
}

static int s_area_calls;
static bool s_pad_ok;
static lv_area_t s_pad_area;
static ui_router_t s_r;

static void hook_area(void *ctx)
{
    (void)ctx;
    s_area_calls++;
    log_ev(EV_AREA, 0);
    s_pad_ok = ui_router_pad_area(&s_r, &s_pad_area);
}

static const ui_nav_hooks_t k_hooks = {.cancel_input = hook_cancel, .input_area_changed = hook_area};

// ---- views ---------------------------------------------------------------------------------
typedef struct {
    ui_view_id_t id;
    lv_obj_t *root;
    lv_obj_t *pad;
    lv_obj_t *button;
    int clicks;
    int enters, leaves, creates, destroys;
    bool fail_create;
    bool nav_from_enter; // re-entrancy probe
    bool keys_down_at_leave_checked;
    int keys_down_seen_at_leave;
} view_t;

static view_t s_views[3];

static void count_click(lv_event_t *e)
{
    int *n = lv_event_get_user_data(e);
    (*n)++;
}

static void host_label_cb(lv_observer_t *o, lv_subject_t *subj)
{
    lv_label_set_text(lv_observer_get_target_obj(o), ui_present_host(lv_subject_get_int(subj)));
}

static lv_obj_t *view_create(void *ctx, lv_obj_t *slot)
{
    view_t *v = ctx;
    log_ev(EV_CREATE, (int)v->id);
    if (v->fail_create) {
        return NULL;
    }
    v->creates++;
    v->root = lv_obj_create(slot);
    lv_obj_set_size(v->root, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollable(v->root, false);
    v->button = lv_button_create(v->root);
    lv_obj_set_pos(v->button, 16, 16);
    lv_obj_set_size(v->button, 120, 60);
    lv_obj_add_event_cb(v->button, count_click, LV_EVENT_CLICKED, &v->clicks);
    if (s_store_on) { // a status label bound through the object (observer dies with the widget)
        lv_obj_t *l = lv_label_create(v->root);
        lv_obj_set_pos(l, 200, 16);
        lv_subject_add_observer_obj(ui_store_subject(&s_store, UI_SUBJ_HOST_USB), host_label_cb, l, NULL);
    }
    if (v->id == UI_VIEW_MOUSE) {
        v->pad = lv_obj_create(v->root);
        lv_obj_set_pos(v->pad, 160, 100);
        lv_obj_set_size(v->pad, 800, 400);
    }
    return v->root;
}

static void view_enter(void *ctx)
{
    view_t *v = ctx;
    v->enters++;
    log_ev(EV_ENTER, (int)v->id);
    if (v->nav_from_enter) {
        CHECK(!ui_router_show(&s_r, UI_VIEW_SHORTCUTS)); // re-entrant switch refused
        CHECK(!ui_overlay_show(&s_r, 1));                // inside a switch: refused too
    }
}

static void view_leave(void *ctx)
{
    view_t *v = ctx;
    v->leaves++;
    v->keys_down_seen_at_leave = s_keys_down;
    v->keys_down_at_leave_checked = true;
    log_ev(EV_LEAVE, (int)v->id);
}

static void view_destroy(void *ctx)
{
    view_t *v = ctx;
    v->destroys++;
    log_ev(EV_DESTROY, (int)v->id);
    v->root = NULL;
    v->pad = NULL;
    v->button = NULL;
}

static lv_obj_t *view_pad(void *ctx)
{
    return ((view_t *)ctx)->pad;
}

static ui_view_descriptor_t s_desc[3];

// ---- overlays ------------------------------------------------------------------------------
typedef struct {
    uint16_t id;
    lv_obj_t *close_btn;
    int closes;
    int creates;
    bool fail;
    bool dismiss_on_click;
} overlay_t;

static overlay_t s_ovs[3];

static void close_click(lv_event_t *e)
{
    overlay_t *o = lv_event_get_user_data(e);
    if (o->dismiss_on_click) {
        ui_overlay_dismiss(&s_r); // deletes this button's tree asynchronously
    }
}

static bool ov_create(void *ctx, lv_obj_t *card)
{
    overlay_t *o = ctx;
    if (o->fail) {
        return false;
    }
    o->creates++;
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, "Overlay");
    o->close_btn = lv_button_create(card);
    lv_obj_set_size(o->close_btn, 100, 50);
    lv_obj_add_event_cb(o->close_btn, close_click, LV_EVENT_CLICKED, o);
    return true;
}

static void ov_close(void *ctx)
{
    overlay_t *o = ctx;
    o->closes++;
    o->close_btn = NULL;
    log_ev(EV_OV_CLOSE, (int)o->id);
}

static ui_overlay_descriptor_t s_ov_desc[3];

// ---- fixture -------------------------------------------------------------------------------
static lv_obj_t *s_scr;
static lv_obj_t *s_home; // permanent screen loaded between tests

static void reset_fixture(void)
{
    memset(s_views, 0, sizeof(s_views));
    memset(s_ovs, 0, sizeof(s_ovs));
    s_log_n = 0;
    s_keys_down = 0;
    s_button_down = false;
    s_refuse_cancel = false;
    memset(s_cancels, 0, sizeof(s_cancels));
    s_area_calls = 0;
    s_pad_ok = false;
    const ui_view_id_t ids[3] = {UI_VIEW_MOUSE, UI_VIEW_KEYBOARD, UI_VIEW_SHORTCUTS};
    for (int i = 0; i < 3; i++) {
        s_views[i].id = ids[i];
        s_desc[i] = (ui_view_descriptor_t){.id = ids[i],
                                           .create = view_create,
                                           .on_enter = view_enter,
                                           .on_leave = view_leave,
                                           .destroy = view_destroy,
                                           .pad_obj = view_pad,
                                           .ctx = &s_views[i]};
    }
    const bool blocking[3] = {true, true, false};
    for (int i = 0; i < 3; i++) {
        s_ovs[i].id = (uint16_t)(i + 1);
        s_ov_desc[i] = (ui_overlay_descriptor_t){
            .id = (uint16_t)(i + 1), .create = ov_create, .on_close = ov_close, .blocking = blocking[i], .ctx = &s_ovs[i]};
    }
}

static bool start(bool with_store)
{
    reset_fixture();
    s_store_on = with_store;
    if (with_store) {
        ui_mailbox_init(&s_mb, NULL);
        CHECK(ui_store_init(&s_store, &s_mb));
    }
    s_scr = lv_obj_create(NULL);
    lv_screen_load(s_scr);
    const ui_shell_layout_t lay = {.width = SCR_W, .height = SCR_H, .topbar_h = TOP_H};
    return ui_router_init(&s_r, s_scr, &lay, with_store ? &s_store : NULL, s_desc, 3, s_ov_desc, 3, &k_hooks);
}

static void stop(void)
{
    ui_router_deinit(&s_r);
    if (s_store_on) {
        ui_store_deinit(&s_store);
        s_store_on = false;
    }
    lv_screen_load(s_home);
    lv_test_wait(5); // the screen switch completes on the next refresh
    lv_obj_delete(s_scr);
    lv_test_wait(10); // pending async deletes and refresh
}

static int find_ev(ev_t ev, int from)
{
    for (int i = from; i < s_log_n; i++) {
        if (s_log[i].ev == ev) {
            return i;
        }
    }
    return -1;
}

static void click_obj(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    lv_test_mouse_click_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_wait(50);
}

// ---- tests ---------------------------------------------------------------------------------
static void test_init_validation(void)
{
    reset_fixture();
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_router_t r;
    const ui_shell_layout_t lay = {.width = SCR_W, .height = SCR_H, .topbar_h = TOP_H};
    const ui_shell_layout_t bad = {.width = SCR_W, .height = SCR_H, .topbar_h = SCR_H};
    CHECK(!ui_router_init(&r, NULL, &lay, NULL, s_desc, 3, NULL, 0, NULL));
    CHECK(!ui_router_init(&r, scr, &bad, NULL, s_desc, 3, NULL, 0, NULL));
    ui_view_descriptor_t dup[2] = {s_desc[0], s_desc[0]};
    CHECK(!ui_router_init(&r, scr, &lay, NULL, dup, 2, NULL, 0, NULL));
    ui_view_descriptor_t settings = s_desc[0];
    settings.id = UI_VIEW_SETTINGS; // settings is an overlay, not a primary view
    CHECK(!ui_router_init(&r, scr, &lay, NULL, &settings, 1, NULL, 0, NULL));
    ui_overlay_descriptor_t ov_dup[2] = {s_ov_desc[0], s_ov_desc[0]};
    CHECK(!ui_router_init(&r, scr, &lay, NULL, s_desc, 3, ov_dup, 2, NULL));
    ui_overlay_descriptor_t ov_zero = s_ov_desc[0];
    ov_zero.id = UI_OVERLAY_NONE;
    CHECK(!ui_router_init(&r, scr, &lay, NULL, s_desc, 3, &ov_zero, 1, NULL));
    CHECK_EQ(lv_obj_get_child_count(scr), 0); // nothing left behind by a refused init
    CHECK(ui_router_init(&r, scr, &lay, NULL, s_desc, 3, s_ov_desc, 3, NULL));
    CHECK_EQ(ui_router_active(&r), -1);
    CHECK(!ui_router_pad_area(&r, &(lv_area_t){0}));
    ui_router_deinit(&r);
    CHECK_EQ(lv_obj_get_child_count(scr), 0);
    lv_obj_delete(scr);
}

static void test_atomic_switch_order(void)
{
    CHECK(start(true));
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    CHECK_EQ(ui_router_active(&s_r), UI_VIEW_MOUSE);
    CHECK(s_pad_ok); // MOUSE has a pad and no overlay is open
    s_log_n = 0;
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    // create (lazy) -> cancel -> leave(MOUSE) -> enter(KEYBOARD) -> area; subject published before the area hook
    const int i_create = find_ev(EV_CREATE, 0);
    const int i_cancel = find_ev(EV_CANCEL, 0);
    const int i_leave = find_ev(EV_LEAVE, 0);
    const int i_enter = find_ev(EV_ENTER, 0);
    const int i_area = find_ev(EV_AREA, 0);
    CHECK(i_create >= 0 && i_create < i_cancel);
    CHECK(i_cancel >= 0 && i_cancel < i_leave);
    CHECK(i_leave < i_enter && i_enter < i_area);
    CHECK_EQ(s_log[i_cancel].arg, UI_CANCEL_VIEW_CHANGE);
    CHECK_EQ(s_log[i_leave].arg, UI_VIEW_MOUSE);
    CHECK_EQ(s_log[i_enter].arg, UI_VIEW_KEYBOARD);
    CHECK_EQ(s_log[i_enter].active_view_subject, UI_VIEW_MOUSE);     // not yet published during on_enter
    CHECK_EQ(s_log[i_area].active_view_subject, UI_VIEW_KEYBOARD);   // published before the pad re-read
    CHECK(!s_pad_ok);                                                 // KEYBOARD has no pad
    CHECK(lv_obj_is_hidden(s_views[0].root));
    CHECK(!lv_obj_is_hidden(s_views[1].root));
    // same view: no-op, no cancel
    s_log_n = 0;
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    CHECK_EQ(find_ev(EV_CANCEL, 0), -1);
    CHECK_EQ(s_r.c.show_noop, 1);
    stop();
}

static void test_nav_with_key_down_and_drag(void)
{
    CHECK(start(false));
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    s_keys_down = 3; // Ctrl + Shift + a held through the on-screen keyboard
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    CHECK(s_views[1].keys_down_at_leave_checked);
    CHECK_EQ(s_views[1].keys_down_seen_at_leave, 0); // release requested before the old view left
    CHECK_EQ(s_keys_down, 0);
    s_button_down = true; // drag on the pad
    CHECK(ui_router_show(&s_r, UI_VIEW_SHORTCUTS));
    CHECK(!s_button_down);
    // the HID queue refuses the release request: the switch still happens, the refusal is counted,
    // and the HID core remains the safety owner (it releases on its own)
    s_refuse_cancel = true;
    s_keys_down = 1;
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    CHECK_EQ(ui_router_active(&s_r), UI_VIEW_KEYBOARD);
    CHECK_EQ(s_r.c.cancel_refused, 1);
    CHECK_EQ(s_cancels[UI_CANCEL_VIEW_CHANGE], 4);
    stop();
}

static void test_lazy_cache_and_failures(void)
{
    CHECK(start(false));
    s_views[1].fail_create = true;
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    s_log_n = 0;
    CHECK(!ui_router_show(&s_r, UI_VIEW_KEYBOARD)); // create failed
    CHECK_EQ(ui_router_active(&s_r), UI_VIEW_MOUSE);
    CHECK_EQ(find_ev(EV_CANCEL, 0), -1); // nothing cancelled, old view untouched
    CHECK_EQ(find_ev(EV_LEAVE, 0), -1);
    CHECK(!lv_obj_is_hidden(s_views[0].root));
    s_views[1].fail_create = false;
    for (int i = 0; i < 30; i++) {
        CHECK(ui_router_show(&s_r, (ui_view_id_t)(i % 3)));
    }
    CHECK_EQ(s_views[0].creates + s_views[1].creates + s_views[2].creates, 3); // each created exactly once
    CHECK_EQ(s_r.c.views_created, 3);
    CHECK(!ui_router_show(&s_r, UI_VIEW_SETTINGS)); // not a primary view
    CHECK(!ui_router_show(&s_r, (ui_view_id_t)99));
    // re-entrancy from on_enter
    s_views[1].nav_from_enter = true;
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    CHECK_EQ(ui_router_active(&s_r), UI_VIEW_KEYBOARD);
    stop();
    CHECK_EQ(s_views[0].destroys + s_views[1].destroys + s_views[2].destroys, 3);
}

static void test_blocking_overlay_absorbs_clicks(void)
{
    CHECK(start(true));
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    lv_test_wait(20);
    click_obj(s_views[0].button);
    CHECK_EQ(s_views[0].clicks, 1);
    CHECK(s_pad_ok);

    s_button_down = true; // a drag is active when the modal opens
    s_log_n = 0;
    CHECK(ui_overlay_show(&s_r, 1));
    CHECK_EQ(s_log[0].ev, EV_CANCEL);
    CHECK_EQ(s_log[0].arg, UI_CANCEL_OVERLAY_OPEN);
    CHECK(!s_button_down);
    CHECK(!s_pad_ok);                          // the pad is taken away
    CHECK_EQ(ui_overlay_top(&s_r), 1);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OVERLAY), 1);
    CHECK(!ui_router_show(&s_r, UI_VIEW_KEYBOARD)); // no view switch under a blocking modal
    lv_test_wait(20);
    click_obj(s_views[0].button);
    CHECK_EQ(s_views[0].clicks, 1); // absorbed by the scrim

    ui_overlay_dismiss(&s_r);
    lv_test_wait(20);
    CHECK(s_pad_ok); // given back
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OVERLAY), 0);
    CHECK_EQ(s_ovs[0].closes, 1);
    click_obj(s_views[0].button);
    CHECK_EQ(s_views[0].clicks, 2); // the view below works again
    stop();
}

// The scrim tint comes from the shell, so the router's blocking overlays and the UiModal of ui_components dim
// the screen by the same amount. ui_core owns no design tokens (07 §2), hence the layout field and the
// built-in fallback when the shell leaves it at zero.
static void test_scrim_tint_follows_the_shell(void)
{
    CHECK(start(true)); // the default layout leaves scrim_opa at 0
    CHECK(ui_overlay_show(&s_r, 1));
    lv_test_wait(10);
    lv_obj_t *scrim = s_r.stack[0].scrim;
    CHECK_EQ(lv_obj_get_style_bg_opa(scrim, LV_PART_MAIN), LV_OPA_50);
    CHECK_EQ(lv_obj_get_style_bg_color(scrim, LV_PART_MAIN).red, 0x00);
    stop();

    reset_fixture();
    s_scr = lv_obj_create(NULL);
    lv_screen_load(s_scr);
    const ui_shell_layout_t tinted = {.width = SCR_W,
                                      .height = SCR_H,
                                      .topbar_h = TOP_H,
                                      .scrim_color = 0x1D1D1Fu,
                                      .scrim_opa = LV_OPA_40};
    CHECK(ui_router_init(&s_r, s_scr, &tinted, NULL, s_desc, 3, s_ov_desc, 3, &k_hooks));
    CHECK(ui_overlay_show(&s_r, 1));
    lv_test_wait(10);
    scrim = s_r.stack[0].scrim;
    CHECK_EQ(lv_obj_get_style_bg_opa(scrim, LV_PART_MAIN), LV_OPA_40);
    CHECK_EQ(lv_obj_get_style_bg_color(scrim, LV_PART_MAIN).red, 0x1D);
    // Still full-screen: a blocking overlay covers the top bar slot as well as the content slot.
    lv_obj_update_layout(scrim);
    lv_area_t a;
    lv_obj_get_coords(scrim, &a);
    CHECK_EQ(lv_area_get_width(&a), SCR_W);
    CHECK_EQ(lv_area_get_height(&a), SCR_H);
    stop();
}

static void test_overlay_stack_rules(void)
{
    CHECK(start(false));
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    const int area0 = s_area_calls;
    CHECK(ui_overlay_show(&s_r, 3)); // non-blocking panel: pad stays
    CHECK_EQ(s_area_calls, area0);
    CHECK(ui_router_pad_area(&s_r, &(lv_area_t){0}));
    CHECK(!ui_overlay_show(&s_r, 3)); // duplicate
    CHECK(ui_overlay_show(&s_r, 1));  // blocking over it
    CHECK_EQ(s_area_calls, area0 + 1);
    CHECK(!s_pad_ok);
    CHECK(!ui_overlay_show(&s_r, 2)); // stack full (max 2)
    CHECK_EQ(s_r.c.overlay_refused, 2);
    CHECK(!ui_overlay_show(&s_r, 77)); // unknown
    CHECK_EQ(s_cancels[UI_CANCEL_OVERLAY_OPEN], 1);
    ui_overlay_dismiss(&s_r); // LIFO: the blocking one goes first
    CHECK_EQ(s_ovs[0].closes, 1);
    CHECK_EQ(s_ovs[2].closes, 0);
    CHECK_EQ(ui_overlay_top(&s_r), 3);
    CHECK(s_pad_ok);
    CHECK_EQ(s_area_calls, area0 + 2);
    s_ovs[1].fail = true;
    CHECK(!ui_overlay_show(&s_r, 2)); // create failure: nothing pushed, pad not taken
    CHECK_EQ(ui_overlay_top(&s_r), 3);
    CHECK(s_pad_ok);
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD)); // non-blocking overlay allows navigation
    ui_overlay_dismiss_all(&s_r);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    ui_overlay_dismiss(&s_r); // no-op on an empty stack
    stop();
}

static void test_dismiss_from_click_1000(void)
{
    CHECK(start(true));
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    s_ovs[0].dismiss_on_click = true;
    lv_test_wait(20);
    size_t base = 0;
    for (int i = 0; i < 1000; i++) {
        CHECK(ui_overlay_show(&s_r, 1));
        lv_test_wait(5);
        click_obj(s_ovs[0].close_btn); // the close button dismisses its own overlay
        CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
        click_obj(s_views[0].button); // focus/pad not blocked after closing (AT-UIFW-10)
        if (i == 9) {
            base = host_lv_live_count();
        }
    }
    CHECK_EQ(s_views[0].clicks, 1000);
    CHECK_EQ(s_ovs[0].closes, 1000);
    CHECK(s_pad_ok);
    printf("    1000 modal open/click-close cycles: live allocations %zu -> %zu\n", base, host_lv_live_count());
    CHECK_EQ(host_lv_live_count(), base);
    stop();
}

static void test_10000_switches_no_growth(void)
{
    CHECK(start(true));
    for (int i = 0; i < 3; i++) {
        CHECK(ui_router_show(&s_r, (ui_view_id_t)i));
    }
    lv_test_wait(20);
    const size_t base_cnt = host_lv_live_count();
    const size_t base_bytes = host_lv_live_bytes();
    for (int i = 0; i < 10000; i++) {
        CHECK(ui_router_show(&s_r, (ui_view_id_t)(i % 3)));
        if (i % 1000 == 0) {
            ui_hid_slice_t h = {.host_usb_state = (uint8_t)(i % 2 ? KVM_HOST_USB_MOUNTED : KVM_HOST_USB_SUSPENDED)};
            ui_mailbox_publish_hid(&s_mb, &h, (uint32_t)(i + 1));
            ui_store_tick(&s_store);
            lv_test_wait(5);
        }
    }
    lv_test_wait(20);
    printf("    10000 view switches: live allocations %zu -> %zu, bytes %zu -> %zu, shows %u\n", base_cnt,
           host_lv_live_count(), base_bytes, host_lv_live_bytes(), (unsigned)s_r.c.shows);
    CHECK_EQ(host_lv_live_count(), base_cnt);
    CHECK_EQ(s_r.c.views_created, 3);
    stop();
}

static void test_deinit_orders(void)
{
    // router first, then store
    CHECK(start(true));
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    CHECK(ui_overlay_show(&s_r, 1));
    s_log_n = 0;
    ui_router_deinit(&s_r);
    CHECK_EQ(s_ovs[0].closes, 1);         // overlays closed first
    CHECK_EQ(s_views[0].leaves, 1);       // the active view left
    CHECK_EQ(s_cancels[UI_CANCEL_SHUTDOWN], 1);
    CHECK_EQ(s_views[0].destroys, 1);
    CHECK_EQ(ui_router_active(&s_r), -1);
    CHECK_EQ(lv_obj_get_child_count(s_scr), 0);
    ui_router_deinit(&s_r); // idempotent
    ui_store_deinit(&s_store);
    s_store_on = false;
    lv_screen_load(s_home);
    lv_test_wait(5);
    lv_obj_delete(s_scr);
    lv_test_wait(10);

    // store first (subjects deleted while the views' labels still observe them), then router
    CHECK(start(true));
    for (int i = 0; i < 3; i++) {
        CHECK(ui_router_show(&s_r, (ui_view_id_t)i));
    }
    ui_store_deinit(&s_store);
    s_store_on = false;
    lv_test_wait(10); // render with orphaned labels: no access to deleted subjects
    stop();
}

#define RUN_MEM(t)                                                                                      \
    do {                                                                                                \
        size_t before_ = host_lv_live_count();                                                          \
        RUN(t);                                                                                         \
        if (host_lv_live_count() != before_) {                                                          \
            printf("    %s: live allocations %zu -> %zu\n", #t, before_, host_lv_live_count());         \
        }                                                                                               \
        CHECK_EQ(host_lv_live_count(), before_);                                                        \
    } while (0)

int main(void)
{
    printf("test_ui_nav (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    lv_test_indev_create_all();
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);
    // warm-up: first text render, first button press, first async call, first screen switch
    CHECK(start(true));
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    s_ovs[0].dismiss_on_click = true;
    CHECK(ui_overlay_show(&s_r, 1));
    lv_test_wait(20);
    click_obj(s_ovs[0].close_btn);
    click_obj(s_views[0].button);
    stop();
    printf("  live allocations after warm-up: %zu\n", host_lv_live_count());

    RUN_MEM(test_init_validation);
    RUN_MEM(test_atomic_switch_order);
    RUN_MEM(test_nav_with_key_down_and_drag);
    RUN_MEM(test_lazy_cache_and_failures);
    RUN_MEM(test_blocking_overlay_absorbs_clicks);
    RUN_MEM(test_scrim_tint_follows_the_shell);
    RUN_MEM(test_overlay_stack_rules);
    RUN_MEM(test_dismiss_from_click_1000);
    RUN_MEM(test_10000_switches_no_growth);
    RUN_MEM(test_deinit_orders);

    lv_test_indev_delete_all();
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_nav");
}
