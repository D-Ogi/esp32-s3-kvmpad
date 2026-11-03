// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-060B with the real LVGL 9.6 (headless test display, address sanitizer):
// - domain subjects stay UNKNOWN until their source publishes (07 §12 item 2);
// - observers bound through objects die with the object; store deinit detaches the rest (AT-UIFW-09);
// - a late / dropped snapshot or a lost event converges to the domain state (AT-UIFW-03);
// - 10 000 create/bind/delete cycles leave no live allocation behind (AT-UIFW-02, store level).
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_present.h"
#include "ui_store.h"

static ui_mailbox_t s_mb;
static ui_store_t s_store;

typedef struct {
    int calls;
    int32_t last;
} obs_t;

static void obs_cb(lv_observer_t *o, lv_subject_t *subj)
{
    obs_t *st = lv_observer_get_user_data(o);
    st->calls++;
    st->last = lv_subject_get_int(subj);
}

static void host_label_cb(lv_observer_t *o, lv_subject_t *subj)
{
    lv_obj_t *label = lv_observer_get_target_obj(o);
    lv_label_set_text(label, ui_present_host(lv_subject_get_int(subj)));
}

static ui_hid_slice_t hid(uint8_t usb, uint8_t owner, uint32_t epoch)
{
    ui_hid_slice_t s = {.host_usb_state = usb, .owner_kind = owner, .owner_epoch = epoch};
    return s;
}

static uint32_t subs(ui_subject_id_t id)
{
    return lv_ll_get_len(&ui_store_subject(&s_store, id)->subs_ll);
}

static void fresh(void)
{
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
}

static void test_unknown_until_published(void)
{
    fresh();
    for (int i = 0; i < UI_SUBJ_DOMAIN_COUNT; i++) {
        CHECK_EQ(ui_store_get(&s_store, (ui_subject_id_t)i), UI_STORE_UNKNOWN);
    }
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_ACTIVE_VIEW), UI_VIEW_MOUSE);
    CHECK(!ui_store_tick(&s_store)); // empty mailbox: nothing applied

    ui_hid_slice_t h = hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 5);
    h.modifiers_down = 0x02;
    h.mods_armed = 0x01;  // one-shot LCtrl
    h.mods_locked = 0x04; // locked LAlt
    ui_mailbox_publish_hid(&s_mb, &h, 1);
    CHECK(ui_store_tick(&s_store));
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_HOST_USB), KVM_HOST_USB_MOUNTED);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OWNER_KIND), OWNER_LOCAL);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OWNER_EPOCH), 5);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_MODIFIERS), 0x02);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_MODS_ARMED), 0x01);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_MODS_LOCKED), 0x04);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_CAPS_LED), UI_STORE_UNKNOWN); // no LED report from the HOST yet
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_CLIENT), UI_STORE_UNKNOWN);   // no network source
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_WIFI), UI_STORE_UNKNOWN);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_DRAG_LOCK), UI_STORE_UNKNOWN);
    CHECK(strcmp(ui_present_client(ui_store_get(&s_store, UI_SUBJ_CLIENT)), "Unknown") == 0);

    h.caps_led_known = true;
    h.caps_led_active = true;
    ui_mailbox_publish_hid(&s_mb, &h, 2);
    ui_store_tick(&s_store);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_CAPS_LED), 1);
    ui_store_deinit(&s_store);
}

static void test_observer_notify_and_label(void)
{
    fresh();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *label = lv_label_create(scr);
    obs_t st = {0};
    lv_subject_add_observer_obj(ui_store_subject(&s_store, UI_SUBJ_HOST_USB), host_label_cb, label, NULL);
    lv_subject_add_observer_obj(ui_store_subject(&s_store, UI_SUBJ_HOST_USB), obs_cb, label, &st);
    CHECK_EQ(st.calls, 1); // LVGL notifies once on subscription
    CHECK(strcmp(lv_label_get_text(label), "Unknown") == 0);

    ui_hid_slice_t h = hid(KVM_HOST_USB_ENUMERATING, OWNER_NONE, 1);
    ui_mailbox_publish_hid(&s_mb, &h, 1);
    ui_store_tick(&s_store);
    CHECK(strcmp(lv_label_get_text(label), "Connecting") == 0);
    CHECK_EQ(st.calls, 2);
    // unchanged values do not notify; an epoch change does not touch the host subject
    h.owner_epoch = 2;
    ui_mailbox_publish_hid(&s_mb, &h, 2);
    ui_store_tick(&s_store);
    CHECK_EQ(st.calls, 2);
    h.host_usb_state = KVM_HOST_USB_MOUNTED;
    ui_mailbox_publish_hid(&s_mb, &h, 3);
    ui_store_tick(&s_store);
    CHECK(strcmp(lv_label_get_text(label), "Active") == 0);
    CHECK_EQ(st.calls, 3);
    lv_refr_now(NULL); // render once with the real software renderer
    CHECK_EQ(subs(UI_SUBJ_HOST_USB), 2);

    // AT-UIFW-09: deleting the widget detaches its observers; later publishes must not touch it
    lv_obj_delete(label);
    CHECK_EQ(subs(UI_SUBJ_HOST_USB), 0);
    h.host_usb_state = KVM_HOST_USB_DETACHED;
    ui_mailbox_publish_hid(&s_mb, &h, 4);
    ui_store_tick(&s_store); // ASan would report a use-after-free here
    CHECK_EQ(st.calls, 3);
    ui_store_deinit(&s_store);
}

static void test_deinit_with_live_widgets(void)
{
    size_t base = host_lv_live_count();
    fresh();
    lv_obj_t *cont = lv_obj_create(lv_screen_active());
    obs_t st = {0};
    for (int i = 0; i < 20; i++) {
        lv_obj_t *l = lv_label_create(cont);
        for (int s = 0; s < UI_SUBJ_COUNT; s++) {
            lv_subject_add_observer_obj(ui_store_subject(&s_store, (ui_subject_id_t)s), obs_cb, l, &st);
        }
    }
    CHECK_EQ(subs(UI_SUBJ_ERROR), 20);
    // the store goes away first (e.g. a UI restart): subjects detach every observer ...
    ui_store_deinit(&s_store);
    // ... and deleting the widgets afterwards must not reach the freed observers
    int calls = st.calls;
    lv_obj_delete(cont);
    CHECK_EQ(st.calls, calls);
    CHECK_EQ(host_lv_live_count(), base);
}

static void test_dropped_and_late(void)
{
    fresh();
    obs_t st = {0};
    lv_obj_t *l = lv_label_create(lv_screen_active());
    lv_subject_add_observer_obj(ui_store_subject(&s_store, UI_SUBJ_OWNER_KIND), obs_cb, l, &st);
    // many publishes between two UI ticks: only the newest is applied (dropped intermediates)
    for (uint32_t i = 1; i <= 50; i++) {
        ui_hid_slice_t h = hid(KVM_HOST_USB_MOUNTED, (uint8_t)(i % 3), i);
        ui_mailbox_publish_hid(&s_mb, &h, i);
    }
    ui_store_tick(&s_store);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OWNER_EPOCH), 50);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OWNER_KIND), 50 % 3);
    CHECK_EQ(s_store.c.applied, 1);
    // a late publish of an older domain sequence never rolls the UI back
    ui_hid_slice_t old = hid(KVM_HOST_USB_DETACHED, OWNER_NONE, 7);
    CHECK_EQ(ui_mailbox_publish_hid(&s_mb, &old, 7), UI_PUB_STALE);
    CHECK(!ui_store_tick(&s_store));
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_HOST_USB), KVM_HOST_USB_MOUNTED);

    // lost events: the next tick re-applies and re-notifies every domain observer
    int before = st.calls;
    for (uint32_t i = 0; i < UI_MAILBOX_EVENT_CAP + 3; i++) {
        ui_event_t e = {.kind = UI_EV_ACTION_RESULT, .request_id = i + 1};
        ui_mailbox_post_event(&s_mb, &e);
    }
    CHECK(ui_store_tick(&s_store));
    CHECK_EQ(s_store.c.resyncs, 1);
    CHECK(st.calls > before);
    CHECK_EQ(st.last, 50 % 3);
    CHECK_EQ(s_store.c.events, UI_MAILBOX_EVENT_CAP);
    lv_obj_delete(l);
    ui_store_deinit(&s_store);
}

static int s_ev_calls;
static void on_event(void *ctx, const ui_event_t *ev)
{
    (void)ctx;
    (void)ev;
    s_ev_calls++;
}

static void test_events_bounded_per_tick(void)
{
    fresh();
    s_ev_calls = 0;
    ui_store_set_event_handler(&s_store, on_event, NULL);
    for (int i = 0; i < 5; i++) {
        ui_event_t e = {.kind = UI_EV_RELEASE_ALL, .code = KVM_RA_VIEW_CHANGE};
        ui_mailbox_post_event(&s_mb, &e);
    }
    ui_store_tick(&s_store);
    CHECK_EQ(s_ev_calls, 5);
    ui_store_tick(&s_store);
    CHECK_EQ(s_ev_calls, 5);
    ui_store_deinit(&s_store);
}

static void test_local_state(void)
{
    fresh();
    ui_store_set_active_view(&s_store, UI_VIEW_KEYBOARD);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_ACTIVE_VIEW), UI_VIEW_KEYBOARD);
    ui_store_set_active_view(&s_store, UI_VIEW_COUNT); // out of range: ignored
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_ACTIVE_VIEW), UI_VIEW_KEYBOARD);
    ui_store_set_overlay(&s_store, 3);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OVERLAY), 3);
    CHECK(ui_store_subject(&s_store, UI_SUBJ_COUNT) == NULL);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_COUNT), UI_STORE_UNKNOWN);
    CHECK(strcmp(ui_subject_name(UI_SUBJ_OVERLAY), "overlay") == 0);
    ui_store_deinit(&s_store);
    ui_store_deinit(&s_store); // idempotent
    CHECK(ui_store_subject(&s_store, UI_SUBJ_HOST_USB) == NULL);
}

// AT-UIFW-02 (store level): 10 000 cycles of a status bar bound to every subject, created and deleted
// while snapshots keep arriving; live LVGL allocations and observer counts return to the baseline.
static void test_cycles_no_growth(void)
{
    fresh();
    lv_obj_t *scr = lv_screen_active();
    size_t base_cnt = host_lv_live_count();
    size_t base_bytes = host_lv_live_bytes();
    obs_t st = {0};
    for (uint32_t cyc = 1; cyc <= 10000; cyc++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_t *host = lv_label_create(bar);
        lv_subject_add_observer_obj(ui_store_subject(&s_store, UI_SUBJ_HOST_USB), host_label_cb, host, NULL);
        lv_label_bind_text(lv_label_create(bar), ui_store_subject(&s_store, UI_SUBJ_OWNER_EPOCH), "epoch %d");
        for (int s = 0; s < UI_SUBJ_COUNT; s++) {
            lv_subject_add_observer_obj(ui_store_subject(&s_store, (ui_subject_id_t)s), obs_cb, bar, &st);
        }
        ui_hid_slice_t h = hid((uint8_t)(cyc % 6), (uint8_t)(cyc % 3), cyc);
        ui_mailbox_publish_hid(&s_mb, &h, cyc);
        ui_store_tick(&s_store);
        if (cyc % 1000 == 0) {
            lv_refr_now(NULL);
        }
        lv_obj_delete(bar);
    }
    CHECK_EQ(host_lv_live_count(), base_cnt);
    CHECK_EQ(host_lv_live_bytes(), base_bytes);
    for (int s = 0; s < UI_SUBJ_COUNT; s++) {
        CHECK_EQ(subs((ui_subject_id_t)s), 0);
    }
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_OWNER_EPOCH), 10000);
    printf("    10000 cycles: live allocations %zu -> %zu, bytes %zu -> %zu, observer calls %d\n", base_cnt,
           host_lv_live_count(), base_bytes, host_lv_live_bytes(), st.calls);
    ui_store_deinit(&s_store);
}

// Every test must leave the LVGL heap as it found it (subjects, observers, widgets).
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
    printf("test_ui_store (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(1024, 600);
    CHECK(disp != NULL);
    size_t cold = host_lv_live_count();
    lv_refr_now(NULL);
    size_t empty = host_lv_live_count();
    // The first text render creates long-lived renderer state; warm it up once so the baseline is fair.
    lv_obj_t *warm = lv_label_create(lv_screen_active());
    lv_label_set_text(warm, "Warm-up 0123456789 Unknown Active");
    lv_refr_now(NULL);
    lv_obj_delete(warm);
    lv_refr_now(NULL);
    size_t base = host_lv_live_count();
    printf("  live allocations: %zu cold, %zu after an empty render, %zu after a text render + delete\n", cold, empty,
           base);
    lv_refr_now(NULL);
    CHECK_EQ(host_lv_live_count(), base); // a second render allocates nothing new
    RUN_MEM(test_unknown_until_published);
    RUN_MEM(test_observer_notify_and_label);
    RUN_MEM(test_deinit_with_live_widgets);
    RUN_MEM(test_dropped_and_late);
    RUN_MEM(test_events_bounded_per_tick);
    RUN_MEM(test_local_state);
    RUN_MEM(test_cycles_no_growth);
    lv_refr_now(NULL);
    printf("  live allocations after all tests: %zu (baseline after display create %zu)\n", host_lv_live_count(),
           base);
    CHECK_EQ(host_lv_live_count(), base);
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_store");
}
