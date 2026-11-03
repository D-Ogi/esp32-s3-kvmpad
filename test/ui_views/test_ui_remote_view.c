// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pairing and control-approval prompts (TASK-100 UI part) with the real LVGL 9.6 (headless 1024x600, LVGL
// test pointer, ASan + UBSan), the real router, overlay stack, store and theme:
// - FR-722: an open window shows the code as "ddd ddd" with both countdowns; a code that is not exactly six
//   digits is refused and the card says it has none - it never renders a partial code;
// - FR-725: once a client has proved the code the card asks about that client, the code leaves the screen,
//   and Accept / Reject are the same size, the same variant and neither is pre-selected;
// - the buttons report exactly one typed action, APPROVE / DENY carrying the session id of the request;
// - AGENTS-UI MUST 5/6: the prompt cancels the input below it, blocks the view and the bar, and closes only
//   when the published state says so - an expired countdown changes nothing by itself;
// - one outcome toast per result_seq, with no client name on an attempt nobody authenticated (SEC-104/105);
// - a name arriving from the network is truncated and stripped of control characters before it is rendered;
// - lifecycle: 500 open/close cycles with no live-allocation growth (AT-UIFW-09), a sink that closes the
//   window from inside the click handler, and NULL arguments everywhere.
//
// The code never reaches the output: it is compared in memory, and a failing CHECK prints the expression.
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_overlay.h"
#include "ui_remote_view.h"
#include "ui_router.h"
#include "ui_store.h"
#include "ui_theme.h"
#include "ui_tokens.h"

#define SCR_W 1024
#define SCR_H 600
// Longer than the SEC-104 warning toast, so the warm-up leaves no transient object behind.
#define WARN_TOAST_WAIT_MS 7000

// ---- fixture -------------------------------------------------------------------------------
static ui_mailbox_t s_mb;
static ui_store_t s_store;
static ui_remote_view_t s_v;
static ui_router_t s_r;
static lv_obj_t *s_scr;
static lv_obj_t *s_home;
static lv_obj_t *s_below; // a clickable button in the view under the prompt
static int s_below_clicks;
static int s_cancels;
static int s_area_reports;
static ui_cancel_reason_t s_cancel_why;

// recorded actions
#define REC_MAX 16
static struct {
    ui_remote_action_t act;
    uint64_t session;
} s_rec[REC_MAX];
static int s_rec_n;
static bool s_no_sink;
/** When set, the sink answers a Cancel the way the domain would: it closes the window immediately. */
static bool s_close_on_cancel;

static void sink(void *ctx, ui_remote_action_t act, uint64_t session)
{
    (void)ctx;
    if (s_rec_n < REC_MAX) {
        s_rec[s_rec_n].act = act;
        s_rec[s_rec_n].session = session;
        s_rec_n++;
    }
    if (s_close_on_cancel && act == UI_REMOTE_ACT_PAIR_CANCEL) {
        const ui_pair_view_t closed = {0};
        ui_remote_view_set_pairing(&s_v, &closed); // re-entrant: dismiss from inside the click handler
    }
}

static bool hook_cancel(void *ctx, ui_cancel_reason_t why)
{
    (void)ctx;
    s_cancel_why = why;
    s_cancels++;
    return true;
}

static void hook_area(void *ctx)
{
    (void)ctx;
    s_area_reports++;
}

static void below_clicked(lv_event_t *e)
{
    (void)e;
    s_below_clicks++;
}

static lv_obj_t *view_create(void *ctx, lv_obj_t *slot)
{
    (void)ctx;
    lv_obj_t *root = lv_obj_create(slot);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    s_below = lv_button_create(root);
    lv_obj_remove_style_all(s_below);
    ui_theme_apply_button(s_below, UI_BTN_PRIMARY);
    lv_obj_set_size(s_below, 240, UI_TOUCH_COMFORT);
    lv_obj_align(s_below, LV_ALIGN_BOTTOM_LEFT, UI_CONTENT_INSET, -UI_CONTENT_INSET);
    lv_obj_add_event_cb(s_below, below_clicked, LV_EVENT_CLICKED, NULL);
    return root;
}

static ui_view_descriptor_t s_desc;

static bool start(void)
{
    s_below_clicks = 0;
    s_cancels = 0;
    s_area_reports = 0;
    s_rec_n = 0;
    s_below = NULL;
    memset(s_rec, 0, sizeof(s_rec));
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
    ui_remote_view_init(&s_v, &s_r, s_no_sink ? NULL : sink, NULL);
    uint8_t n_ov = 0;
    const ui_overlay_descriptor_t *ov = ui_remote_view_overlays(&s_v, &n_ov);
    CHECK_EQ(n_ov, UI_REMOTE_OVERLAYS);
    s_desc = (ui_view_descriptor_t){.id = UI_VIEW_MOUSE, .create = view_create};
    s_scr = lv_obj_create(NULL);
    ui_theme_apply(s_scr, UI_STYLE_SCREEN, LV_STATE_DEFAULT);
    lv_screen_load(s_scr);
    const ui_shell_layout_t lay = {
        .width = SCR_W, .height = SCR_H, .topbar_h = UI_TOPBAR_H, .scrim_color = UI_COLOR_INK,
        .scrim_opa = UI_SCRIM_OPA};
    const ui_nav_hooks_t hooks = {.cancel_input = hook_cancel, .input_area_changed = hook_area};
    bool ok = ui_router_init(&s_r, s_scr, &lay, &s_store, &s_desc, 1, ov, n_ov, &hooks);
    ok = ok && ui_router_show(&s_r, UI_VIEW_MOUSE);
    lv_test_wait(20);
    return ok;
}

static void stop(void)
{
    ui_remote_view_close(&s_v);
    ui_router_deinit(&s_r);
    ui_store_deinit(&s_store);
    lv_screen_load(s_home);
    lv_test_wait(5);
    lv_obj_delete(s_scr);
    lv_test_wait(20);
}

static void click_obj(lv_obj_t *o)
{
    lv_area_t a;
    CHECK(o != NULL);
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    lv_test_mouse_move_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_mouse_press();
    lv_test_wait(50);
    lv_test_mouse_release();
    lv_test_wait(50);
}

static void click_point(int32_t x, int32_t y)
{
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(50);
    lv_test_mouse_release();
    lv_test_wait(50);
}

static int count_act(ui_remote_action_t act)
{
    int n = 0;
    for (int i = 0; i < s_rec_n; i++) {
        n += s_rec[i].act == act;
    }
    return n;
}

static const char *txt(const char *s)
{
    return s != NULL ? s : "<none>";
}

/** The pairing state the adapter publishes while a window is open and no client has answered yet. */
static ui_pair_view_t window_state(const char *code)
{
    ui_pair_view_t p = {0};
    p.window_open = true;
    if (code != NULL) {
        (void)snprintf(p.code, sizeof(p.code), "%s", code);
    }
    p.code_remaining_ms = 120000u;
    p.window_remaining_ms = 180000u;
    return p;
}

static ui_pair_view_t accept_state(const char *name)
{
    ui_pair_view_t p = window_state("482913");
    p.accept_pending = true;
    (void)snprintf(p.client_name, sizeof(p.client_name), "%s", name);
    p.accept_remaining_ms = 30000u;
    return p;
}

static ui_approval_view_t approval_state(const char *name, uint64_t session)
{
    ui_approval_view_t a = {0};
    a.pending = true;
    a.session_id = session;
    (void)snprintf(a.client_name, sizeof(a.client_name), "%s", name);
    a.remaining_ms = 30000u;
    return a;
}

static void area_of(lv_obj_t *o, lv_area_t *out)
{
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, out);
}

// ---- tests ---------------------------------------------------------------------------------

static void test_window_shows_the_code(void)
{
    CHECK(start());
    const int cancels = s_cancels;
    ui_pair_view_t p = window_state("482913");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_PAIRING);
    CHECK(ui_overlay_blocking(&s_r));
    CHECK(ui_remote_view_card(&s_v, UI_OVERLAY_PAIRING) != NULL);
    CHECK(s_cancels > cancels); // the touch below was cancelled and the HID release requested first
    CHECK_EQ(s_cancel_why, UI_CANCEL_OVERLAY_OPEN);
    CHECK(s_area_reports > 0); // and the pad was taken away from the touch router
    CHECK_EQ(s_v.c.pair_opens, 1u);
    CHECK(!ui_remote_view_accept_mode(&s_v));

    // FR-722: "ddd ddd", both countdowns, and the instruction that tells the user where to type it.
    CHECK(strcmp(txt(ui_remote_view_code_text(&s_v)), "482 913") == 0);
    CHECK(strcmp(txt(ui_remote_view_heading_text(&s_v, UI_OVERLAY_PAIRING)), "Pair a client") == 0);
    CHECK(strcmp(txt(ui_remote_view_timer_text(&s_v, UI_OVERLAY_PAIRING)), "Code expires in 120 s") == 0);
    CHECK(strcmp(txt(ui_remote_view_window_text(&s_v)), "Pairing window closes in 180 s") == 0);
    CHECK(strstr(txt(ui_remote_view_body_text(&s_v, UI_OVERLAY_PAIRING)), "type the code below") != NULL);

    // The countdowns are text: a published update rewrites them, and a running millisecond is not "0 s".
    p.code_remaining_ms = 1u;
    p.window_remaining_ms = 59000u;
    ui_remote_view_set_pairing(&s_v, &p);
    CHECK(strcmp(txt(ui_remote_view_timer_text(&s_v, UI_OVERLAY_PAIRING)), "Code expires in 1 s") == 0);
    CHECK(strcmp(txt(ui_remote_view_window_text(&s_v)), "Pairing window closes in 59 s") == 0);
    CHECK_EQ(s_v.c.pair_opens, 1u); // updated in place, never rebuilt under the user's finger

    const ui_pair_view_t closed = {0};
    ui_remote_view_set_pairing(&s_v, &closed);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK(ui_remote_view_card(&s_v, UI_OVERLAY_PAIRING) == NULL);
    CHECK_EQ(s_v.c.pair_closes, 1u);
    CHECK_EQ(s_v.c.toasts, 0u); // no result was published: nothing is claimed
    stop();
}

static void test_bad_code_fails_closed(void)
{
    const char *bad[] = {"", "12345", "1234567", "48291a", "48 913"};
    CHECK(start());
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ui_pair_view_t p = window_state(bad[i]);
        ui_remote_view_set_pairing(&s_v, &p);
        lv_test_wait(10);
        CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_PAIRING); // the window is open: the card stays up
        CHECK(ui_remote_view_code_text(&s_v) == NULL);      // but no partial code is ever shown
        CHECK(strstr(txt(ui_remote_view_body_text(&s_v, UI_OVERLAY_PAIRING)), "No code to show") != NULL);
    }
    const ui_pair_view_t closed = {0};
    ui_remote_view_set_pairing(&s_v, &closed);
    lv_test_wait(20);
    stop();
}

static void test_accept_question(void)
{
    CHECK(start());
    ui_pair_view_t p = accept_state("DESKTOP-7F3A");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(20);
    CHECK(ui_remote_view_accept_mode(&s_v));
    CHECK_EQ(s_v.c.pair_opens, 1u);
    CHECK(strcmp(txt(ui_remote_view_heading_text(&s_v, UI_OVERLAY_PAIRING)), "DESKTOP-7F3A wants to pair") == 0);
    CHECK(strcmp(txt(ui_remote_view_timer_text(&s_v, UI_OVERLAY_PAIRING)),
                 "Rejected automatically in 30 s") == 0);
    // The code has been used: it is off the screen, and the question is about the client instead.
    CHECK(ui_remote_view_code_text(&s_v) == NULL);
    CHECK(lv_obj_is_hidden(s_v.code_box));
    CHECK(!lv_obj_is_hidden(s_v.accept_box));
    CHECK(strstr(txt(ui_remote_view_body_text(&s_v, UI_OVERLAY_PAIRING)), "does not give it control") != NULL);

    // FR-603 / FR-725: Reject is exactly as easy to hit as Accept and neither is the default.
    CHECK(!lv_obj_is_hidden(s_v.btn_accept));
    CHECK(!lv_obj_is_hidden(s_v.btn_reject));
    CHECK(lv_obj_is_hidden(s_v.btn_cancel));
    CHECK(!lv_obj_is_clickable(s_v.btn_cancel)); // and what is hidden cannot be pressed either
    lv_area_t acc, rej;
    area_of(s_v.btn_accept, &acc);
    area_of(s_v.btn_reject, &rej);
    CHECK_EQ(acc.x2 - acc.x1, rej.x2 - rej.x1);
    CHECK_EQ(acc.y2 - acc.y1, rej.y2 - rej.y1);
    CHECK(acc.x2 - acc.x1 + 1 >= UI_TOUCH_MIN && acc.y2 - acc.y1 + 1 >= UI_TOUCH_MIN);
    CHECK(!lv_obj_has_state(s_v.btn_accept, LV_STATE_FOCUSED));
    CHECK(!lv_obj_has_state(s_v.btn_reject, LV_STATE_FOCUSED));
    CHECK(!lv_obj_has_state(s_v.btn_accept, LV_STATE_CHECKED));
    CHECK(!lv_obj_has_state(s_v.btn_reject, LV_STATE_CHECKED));

    click_obj(s_v.btn_accept);
    CHECK_EQ(count_act(UI_REMOTE_ACT_PAIR_ACCEPT), 1);
    CHECK_EQ(s_rec[0].session, 0u);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_PAIRING); // the UI does not close on its own decision
    click_obj(s_v.btn_reject);
    CHECK_EQ(count_act(UI_REMOTE_ACT_PAIR_REJECT), 1);
    CHECK_EQ(s_rec_n, 2);

    // Back to the code question: the buttons swap again and the code reappears.
    p.accept_pending = false;
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK(!ui_remote_view_accept_mode(&s_v));
    CHECK(strcmp(txt(ui_remote_view_code_text(&s_v)), "482 913") == 0);
    CHECK(!lv_obj_is_hidden(s_v.btn_cancel));
    CHECK(!lv_obj_is_clickable(s_v.btn_accept));
    CHECK(!lv_obj_is_clickable(s_v.btn_reject));
    click_obj(s_v.btn_cancel);
    CHECK_EQ(count_act(UI_REMOTE_ACT_PAIR_CANCEL), 1);

    const ui_pair_view_t closed = {0};
    ui_remote_view_set_pairing(&s_v, &closed);
    lv_test_wait(20);
    stop();
}

static void test_prompt_blocks_everything_below(void)
{
    CHECK(start());
    click_obj(s_below);
    CHECK_EQ(s_below_clicks, 1);
    lv_area_t a;
    area_of(s_below, &a);

    ui_pair_view_t p = window_state("000000");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(20);
    click_point((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    CHECK_EQ(s_below_clicks, 1);                       // the scrim took the press, the view did not
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_PAIRING); // and a tap outside the card dismisses nothing
    CHECK_EQ(s_rec_n, 0);

    // An expired window stays up: only the published state closes it (AGENTS-UI MUST 6).
    p.code_remaining_ms = 0u;
    p.window_remaining_ms = 0u;
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(5000);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_PAIRING);
    CHECK(strcmp(txt(ui_remote_view_timer_text(&s_v, UI_OVERLAY_PAIRING)), "Code expires in 0 s") == 0);

    const ui_pair_view_t closed = {0};
    ui_remote_view_set_pairing(&s_v, &closed);
    lv_test_wait(20);
    click_obj(s_below);
    CHECK_EQ(s_below_clicks, 2); // the view is tappable again
    stop();
}

static void test_approval_prompt(void)
{
    CHECK(start());
    const uint64_t session = 0x1122334455667788u;
    ui_approval_view_t a = approval_state("STUDIO-PC", session);
    ui_remote_view_set_approval(&s_v, &a);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_APPROVAL);
    CHECK(ui_remote_view_card(&s_v, UI_OVERLAY_APPROVAL) != NULL);
    CHECK_EQ(s_v.c.appr_opens, 1u);
    CHECK(strcmp(txt(ui_remote_view_heading_text(&s_v, UI_OVERLAY_APPROVAL)),
                 "STUDIO-PC requests control of the HOST") == 0);
    CHECK(strcmp(txt(ui_remote_view_timer_text(&s_v, UI_OVERLAY_APPROVAL)),
                 "Denied automatically in 30 s") == 0);
    CHECK(strstr(txt(ui_remote_view_body_text(&s_v, UI_OVERLAY_APPROVAL)), "takes control back") != NULL);
    lv_area_t allow, deny;
    area_of(s_v.btn_allow, &allow);
    area_of(s_v.btn_deny, &deny);
    CHECK_EQ(allow.x2 - allow.x1, deny.x2 - deny.x1); // Deny is as easy to hit as Allow
    CHECK(allow.y2 - allow.y1 + 1 >= UI_TOUCH_MIN);

    a.remaining_ms = 9000u;
    ui_remote_view_set_approval(&s_v, &a);
    CHECK(strcmp(txt(ui_remote_view_timer_text(&s_v, UI_OVERLAY_APPROVAL)), "Denied automatically in 9 s") == 0);

    click_obj(s_v.btn_allow);
    CHECK_EQ(count_act(UI_REMOTE_ACT_APPROVE), 1);
    CHECK_EQ(s_rec[0].session, session); // the grant names the session the card was built for
    click_obj(s_v.btn_deny);
    CHECK_EQ(count_act(UI_REMOTE_ACT_DENY), 1);
    CHECK_EQ(s_rec[1].session, session);

    // A second request replaces the first: a later click reports the session that is on the screen now.
    const uint64_t newer = 0x99u;
    a = approval_state("STUDIO-PC", newer);
    ui_remote_view_set_approval(&s_v, &a);
    lv_test_wait(10);
    CHECK_EQ(s_v.c.appr_opens, 1u);
    click_obj(s_v.btn_allow);
    CHECK_EQ(s_rec[2].session, newer);

    const ui_approval_view_t gone = {0};
    ui_remote_view_set_approval(&s_v, &gone);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK_EQ(s_v.c.appr_closes, 1u);
    stop();
}

static void test_pairing_wins_over_approval(void)
{
    CHECK(start());
    ui_pair_view_t p = window_state("111222");
    ui_remote_view_set_pairing(&s_v, &p);
    ui_approval_view_t a = approval_state("LAPTOP", 5u);
    ui_remote_view_set_approval(&s_v, &a);
    lv_test_wait(20);
    // One decision on the screen at a time; the physically opened window keeps it (FR-722 budget).
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_PAIRING);
    CHECK(ui_remote_view_card(&s_v, UI_OVERLAY_APPROVAL) == NULL);
    CHECK_EQ(s_r.depth, 1);

    const ui_pair_view_t closed = {0};
    ui_remote_view_set_pairing(&s_v, &closed);
    lv_test_wait(20);
    // The request is still pending, so it gets the screen now - deferring it never granted anything.
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_APPROVAL);
    CHECK_EQ(s_r.depth, 1);
    CHECK_EQ(count_act(UI_REMOTE_ACT_APPROVE), 0);
    const ui_approval_view_t gone = {0};
    ui_remote_view_set_approval(&s_v, &gone);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    stop();
}

static void test_result_toast_once_per_seq(void)
{
    CHECK(start());
    ui_pair_view_t p = {0};
    p.result = UI_PAIR_RESULT_PAIRED;
    p.result_seq = 1u;
    (void)snprintf(p.result_name, sizeof(p.result_name), "DESKTOP-7F3A");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK_EQ(s_v.c.toasts, 1u);
    CHECK(strcmp(txt(ui_remote_view_toast_text(&s_v)), "Paired: DESKTOP-7F3A") == 0);

    // The same outcome republished with every countdown tick must not stack a second pill.
    for (int i = 0; i < 5; i++) {
        ui_remote_view_set_pairing(&s_v, &p);
        lv_test_wait(10);
    }
    CHECK_EQ(s_v.c.toasts, 1u);

    p.result = UI_PAIR_RESULT_REJECTED;
    p.result_seq = 2u;
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK_EQ(s_v.c.toasts, 2u);
    CHECK(strcmp(txt(ui_remote_view_toast_text(&s_v)), "Pairing rejected: DESKTOP-7F3A") == 0);

    // SEC-104/105: a failed or locked-out attempt is reported without naming an unauthenticated peer.
    p.result = UI_PAIR_RESULT_FAILED;
    p.result_seq = 3u;
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK(strstr(txt(ui_remote_view_toast_text(&s_v)), "failed") != NULL);
    CHECK(strstr(txt(ui_remote_view_toast_text(&s_v)), "DESKTOP") == NULL);
    p.result = UI_PAIR_RESULT_LOCKED;
    p.result_seq = 4u;
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK(strstr(txt(ui_remote_view_toast_text(&s_v)), "locked") != NULL);
    CHECK(strstr(txt(ui_remote_view_toast_text(&s_v)), "DESKTOP") == NULL);
    p.result = UI_PAIR_RESULT_EXPIRED;
    p.result_seq = 5u;
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK(strstr(txt(ui_remote_view_toast_text(&s_v)), "expired") != NULL);
    CHECK_EQ(s_v.c.toasts, 5u);

    // A result with no sequence of its own is reported once and never again.
    p.result = UI_PAIR_RESULT_NONE;
    p.result_seq = 5u;
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK_EQ(s_v.c.toasts, 5u);
    ui_remote_view_close(&s_v);
    lv_test_wait(20);
    CHECK(ui_remote_view_toast_text(&s_v) == NULL);
    stop();
}

static void test_toast_does_not_block_the_view(void)
{
    CHECK(start());
    ui_pair_view_t p = {0};
    p.result = UI_PAIR_RESULT_PAIRED;
    p.result_seq = 1u;
    (void)snprintf(p.result_name, sizeof(p.result_name), "PC");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE); // an outcome is news, not a decision
    click_obj(s_below);
    CHECK_EQ(s_below_clicks, 1);
    stop();
}

static void test_network_name_is_sanitized(void)
{
    CHECK(start());
    // A crafted host name may not break the line, smuggle control characters or overrun the field.
    ui_pair_view_t p = accept_state("");
    p.client_name[0] = 'A';
    p.client_name[1] = '\n';
    p.client_name[2] = '\t';
    p.client_name[3] = (char)0x1Bu;
    p.client_name[4] = 'B';
    p.client_name[5] = '\0';
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK(strcmp(txt(ui_remote_view_heading_text(&s_v, UI_OVERLAY_PAIRING)), "A???B wants to pair") == 0);

    // A full field with no terminator: the copy stops at the capacity.
    memset(p.client_name, 'X', sizeof(p.client_name));
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    const char *heading = txt(ui_remote_view_heading_text(&s_v, UI_OVERLAY_PAIRING));
    CHECK_EQ(strlen(heading), (UI_PAIR_NAME_CAP - 1u) + strlen(" wants to pair"));
    CHECK(strstr(heading, "wants to pair") != NULL);

    // An empty name is called out, never rendered as if the peer had identified itself.
    memset(p.client_name, 0, sizeof(p.client_name));
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(10);
    CHECK(strcmp(txt(ui_remote_view_heading_text(&s_v, UI_OVERLAY_PAIRING)), "Unknown client wants to pair") == 0);

    const ui_pair_view_t closed = {0};
    ui_remote_view_set_pairing(&s_v, &closed);
    lv_test_wait(20);
    stop();
}

static void test_sink_closes_the_window_from_the_handler(void)
{
    s_close_on_cancel = true;
    CHECK(start());
    ui_pair_view_t p = window_state("654321");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(20);
    click_obj(s_v.btn_cancel); // the domain answers synchronously: the card is dismissed inside the click
    lv_test_wait(30);
    CHECK_EQ(count_act(UI_REMOTE_ACT_PAIR_CANCEL), 1);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK(ui_remote_view_card(&s_v, UI_OVERLAY_PAIRING) == NULL);
    click_obj(s_below);
    CHECK_EQ(s_below_clicks, 1); // and the view below is live again
    stop();
    s_close_on_cancel = false;
}

static void test_without_a_sink_nothing_is_reported(void)
{
    s_no_sink = true;
    CHECK(start());
    ui_pair_view_t p = accept_state("PC");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(20);
    click_obj(s_v.btn_accept);
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_v.c.taps[UI_REMOTE_ACT_PAIR_ACCEPT], 1u);
    CHECK_EQ(s_v.c.refused, 1u); // counted, never silently treated as accepted
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_PAIRING);
    const ui_pair_view_t closed = {0};
    ui_remote_view_set_pairing(&s_v, &closed);
    lv_test_wait(20);
    stop();
    s_no_sink = false;
}

static void test_lifecycle(void)
{
    CHECK(start());
    ui_pair_view_t p = window_state("012345");
    const ui_pair_view_t closed = {0};
    ui_approval_view_t a = approval_state("WARM-UP", 1u);
    const ui_approval_view_t gone = {0};
    // Warm-up: the first card of each kind fills LVGL's one-time caches (glyph draw data, style arrays).
    for (int i = 0; i < 4; i++) {
        ui_remote_view_set_pairing(&s_v, &p);
        p.accept_pending = !p.accept_pending;
        ui_remote_view_set_pairing(&s_v, &p);
        ui_remote_view_set_pairing(&s_v, &closed);
        ui_remote_view_set_approval(&s_v, &a);
        ui_remote_view_set_approval(&s_v, &gone);
        lv_test_wait(10);
    }
    p.accept_pending = false;
    const size_t base = host_lv_live_count();
    for (int i = 0; i < 500; i++) {
        p.code_remaining_ms = (uint32_t)(120000 - i);
        ui_remote_view_set_pairing(&s_v, &p);
        if (i % 3 == 0) {
            p.accept_pending = true;
            (void)snprintf(p.client_name, sizeof(p.client_name), "PC-%03d", i);
            ui_remote_view_set_pairing(&s_v, &p);
            p.accept_pending = false;
        }
        ui_remote_view_set_pairing(&s_v, &closed);
        a.session_id = (uint64_t)i;
        ui_remote_view_set_approval(&s_v, &a);
        ui_remote_view_set_approval(&s_v, &gone);
        lv_test_wait(10);
    }
    printf("    500 prompt cycles: live allocations %zu -> %zu\n", base, host_lv_live_count());
    CHECK_EQ(host_lv_live_count(), base);
    CHECK_EQ(s_v.c.pair_opens, s_v.c.pair_closes);
    CHECK_EQ(s_v.c.appr_opens, s_v.c.appr_closes);
    stop();
}

static void test_store_deleted_first(void)
{
    CHECK(start());
    ui_pair_view_t p = window_state("777888");
    ui_remote_view_set_pairing(&s_v, &p);
    lv_test_wait(20);
    ui_store_deinit(&s_store); // the prompt binds no subject, but the shell may still tear down in any order
    lv_test_wait(10);
    ui_remote_view_close(&s_v);
    ui_router_deinit(&s_r);
    lv_screen_load(s_home);
    lv_test_wait(5);
    lv_obj_delete(s_scr);
    lv_test_wait(20);
    CHECK(ui_remote_view_card(&s_v, UI_OVERLAY_PAIRING) == NULL);
}

static void test_refuses_bad_arguments(void)
{
    ui_remote_view_t v;
    const ui_pair_view_t p = {0};
    const ui_approval_view_t a = {0};
    ui_remote_view_init(NULL, &s_r, sink, NULL);
    ui_remote_view_init(&v, NULL, sink, NULL); // no router: every publish is a no-op
    ui_remote_view_set_pairing(&v, &p);
    ui_remote_view_set_approval(&v, &a);
    ui_remote_view_set_pairing(&v, NULL);
    ui_remote_view_set_approval(&v, NULL);
    ui_remote_view_set_pairing(NULL, &p);
    ui_remote_view_set_approval(NULL, &a);
    ui_remote_view_close(&v);
    ui_remote_view_close(NULL);
    CHECK(ui_remote_view_card(&v, UI_OVERLAY_PAIRING) == NULL);
    CHECK(ui_remote_view_card(&v, 0x1234u) == NULL);
    CHECK(ui_remote_view_card(NULL, UI_OVERLAY_PAIRING) == NULL);
    CHECK(ui_remote_view_code_text(NULL) == NULL);
    CHECK(ui_remote_view_heading_text(NULL, UI_OVERLAY_PAIRING) == NULL);
    CHECK(ui_remote_view_heading_text(&v, 0x1234u) == NULL);
    CHECK(ui_remote_view_body_text(&v, 0x1234u) == NULL);
    CHECK(ui_remote_view_timer_text(&v, 0x1234u) == NULL);
    CHECK(ui_remote_view_window_text(NULL) == NULL);
    CHECK(ui_remote_view_toast_text(&v) == NULL);
    CHECK(!ui_remote_view_accept_mode(&v));
    CHECK(!ui_remote_view_accept_mode(NULL));
    uint8_t n = 0;
    CHECK(ui_remote_view_overlays(NULL, &n) == NULL);
    CHECK(ui_remote_view_overlays(&v, NULL) != NULL);
}

#define RUN_MEM(t)                               \
    do {                                         \
        size_t before_ = host_lv_live_count();   \
        RUN(t);                                  \
        CHECK_EQ(host_lv_live_count(), before_); \
    } while (0)

int main(void)
{
    printf("test_ui_remote_view (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    lv_test_indev_create_all();
    CHECK(ui_theme_init());
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);

    // Warm-up: the first text render, the first card of each kind, the first toast and the first press.
    CHECK(start());
    ui_pair_view_t warm = window_state("678901");
    ui_remote_view_set_pairing(&s_v, &warm);
    warm.accept_pending = true;
    (void)snprintf(warm.client_name, sizeof(warm.client_name), "WARM-UP");
    ui_remote_view_set_pairing(&s_v, &warm);
    const ui_pair_view_t warm_closed = {.result = UI_PAIR_RESULT_LOCKED, .result_seq = 1u};
    ui_remote_view_set_pairing(&s_v, &warm_closed);
    const ui_approval_view_t warm_appr = {.pending = true, .client_name = "WARM-UP", .remaining_ms = 1000u};
    ui_remote_view_set_approval(&s_v, &warm_appr);
    lv_test_wait(20);
    click_obj(s_v.btn_allow);
    const ui_approval_view_t warm_gone = {0};
    ui_remote_view_set_approval(&s_v, &warm_gone);
    lv_test_wait(WARN_TOAST_WAIT_MS);
    click_obj(s_below);
    stop();

    RUN_MEM(test_window_shows_the_code);
    RUN_MEM(test_bad_code_fails_closed);
    RUN_MEM(test_accept_question);
    RUN_MEM(test_prompt_blocks_everything_below);
    RUN_MEM(test_approval_prompt);
    RUN_MEM(test_pairing_wins_over_approval);
    RUN_MEM(test_result_toast_once_per_seq);
    RUN_MEM(test_toast_does_not_block_the_view);
    RUN_MEM(test_network_name_is_sanitized);
    RUN_MEM(test_sink_closes_the_window_from_the_handler);
    RUN_MEM(test_without_a_sink_nothing_is_reported);
    RUN_MEM(test_lifecycle);
    RUN_MEM(test_store_deleted_first);
    RUN(test_refuses_bad_arguments);

    lv_test_indev_delete_all();
    ui_theme_deinit();
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_remote_view");
}
