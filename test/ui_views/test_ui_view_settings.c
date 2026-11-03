// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-070C Settings with the real LVGL 9.6 (headless 1024x600, LVGL test pointer, ASan + UBSan), the real
// router, overlay stack and action gateway:
// - 07 §12 item 2: before the domain publishes, every value reads "Unknown" and every control is disabled;
// - AGENTS-UI MUST 5/6: a tap reports one typed change and the control keeps the PUBLISHED value - the only
//   thing that moves a control on screen is ui_view_settings_set();
// - ADR-023 FR-732/FR-733: relaxing a policy (require_accept OFF, approval down the strictness scale) opens
//   the confirmation card and writes nothing until Yes; Keep as it is writes nothing at all;
// - ADR-023 FR-726: Forget is confirmed, and marking a client trusted is confirmed too;
// - ADR-020: the keep-awake interval steps by 10 s and clamps at 10 and 300 s without reporting;
// - FR-741/FR-747: layout and Unicode mode are read-only and say so; no layout id is invented;
// - FR-751 / TECH-084: About carries the firmware version and the Required Notice of the generated strings;
// - SEC-105 / 07 §7: a client name is copied printable-only, and no token, code or session id is rendered;
// - lifecycle: 200 open/close cycles and 400 section changes without live-allocation growth (AT-UIFW-09).
#include <string.h>

#include "host_lv_mem.h"
#include "kbd_layouts.h"
#include "kvm_config.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_actions.h"
#include "ui_notices_generated.h"
#include "ui_overlay.h"
#include "ui_router.h"
#include "ui_theme.h"
#include "ui_tokens.h"
#include "ui_view_settings.h"

#define SCR_W 1024
#define SCR_H 600

// ui_views must not depend on config_store (07 §2), so ui_view_settings.h mirrors four of its constants and
// the strictness order of kvm_control_approval_t. The mirrors are pinned here, where both headers are
// visible: a change on either side breaks the build instead of quietly desynchronizing the screen from NVS.
_Static_assert(UI_SETTINGS_CLIENTS_MAX == KVM_CONFIG_MAX_PAIRED, "paired client capacity");
_Static_assert(UI_SETTINGS_NAME_CAP == KVM_CONFIG_PAIR_NAME_CAP, "client name capacity");
_Static_assert(UI_SETTINGS_LAYOUT_CAP == KVM_CONFIG_LAYOUT_ID_CAP, "layout id capacity");
_Static_assert((int)UI_CONTROL_APPROVAL_AUTO == (int)KVM_CONTROL_APPROVAL_AUTO, "approval value");
_Static_assert((int)UI_CONTROL_APPROVAL_TRUSTED == (int)KVM_CONTROL_APPROVAL_TRUSTED, "approval value");
_Static_assert((int)UI_CONTROL_APPROVAL_ALWAYS == (int)KVM_CONTROL_APPROVAL_ALWAYS, "approval value");
_Static_assert((int)UI_CONTROL_APPROVAL_COUNT == (int)KVM_CONTROL_APPROVAL_COUNT, "approval count");
_Static_assert((int)UI_UNICODE_MODE_COUNT == (int)KVM_UNICODE_MODE_COUNT, "unicode mode count");
_Static_assert((int)UI_UNICODE_MODE_WINDOWS_HEX == (int)KVM_UNICODE_MODE_WINDOWS_HEX, "unicode mode value");

// ---- fixture -------------------------------------------------------------------------------

static ui_actions_t s_act;
static ui_view_settings_t s_set;
static ui_router_t s_r;
static lv_obj_t *s_scr;
static lv_obj_t *s_home;
static uint64_t s_clock;
static bool s_sink_on = true;
static bool s_pair_on = true;
static int s_pair_calls;

typedef struct {
    ui_setting_id_t id;
    int32_t value;
    uint8_t index;
} change_t;

#define CHANGES_MAX 32
static change_t s_changes[CHANGES_MAX];
static int s_change_n;

static uint64_t fake_now(void)
{
    return s_clock += 1000;
}

static void sink(void *ctx, ui_setting_id_t id, int32_t value, uint8_t client_index)
{
    (void)ctx;
    if (s_change_n < CHANGES_MAX) {
        s_changes[s_change_n++] = (change_t){.id = id, .value = value, .index = client_index};
    }
}

static void pair_action(void *ctx)
{
    (void)ctx;
    s_pair_calls++;
}

static bool local_sink(void *ctx, const ui_action_t *a)
{
    (void)ctx;
    switch (a->type) {
    case UI_ACT_OPEN_SETTINGS:
        return ui_overlay_show(&s_r, UI_OVERLAY_SETTINGS);
    case UI_ACT_DISMISS_OVERLAY:
        ui_overlay_dismiss(&s_r);
        return true;
    default:
        return false;
    }
}

static lv_obj_t *view_create(void *ctx, lv_obj_t *slot)
{
    (void)ctx;
    lv_obj_t *root = lv_obj_create(slot);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    return root;
}

static ui_view_descriptor_t s_view;

static bool start(void)
{
    s_change_n = 0;
    s_pair_calls = 0;
    ui_actions_init(&s_act);
    ui_actions_set_local_sink(&s_act, local_sink, NULL);
    ui_view_settings_init(&s_set, &s_act, &s_r, fake_now, s_sink_on ? sink : NULL, NULL);
    if (s_pair_on) {
        ui_view_settings_set_pair_action(&s_set, pair_action, NULL);
    }

    s_scr = lv_obj_create(NULL);
    CHECK(s_scr != NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, SCR_W, SCR_H);
    s_view = (ui_view_descriptor_t){.id = UI_VIEW_MOUSE, .create = view_create};
    const ui_shell_layout_t layout = {.width = SCR_W,
                                      .height = SCR_H,
                                      .topbar_h = UI_TOPBAR_H,
                                      .scrim_color = UI_COLOR_INK,
                                      .scrim_opa = UI_SCRIM_OPA};
    uint8_t n = 0;
    const ui_overlay_descriptor_t *ov = ui_view_settings_overlays(&s_set, &n);
    CHECK_EQ(n, UI_SETTINGS_OVERLAYS);
    CHECK(ui_router_init(&s_r, s_scr, &layout, NULL, &s_view, 1, ov, n, NULL));
    lv_screen_load(s_scr);
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    lv_test_wait(10);
    return true;
}

static void stop(void)
{
    ui_router_deinit(&s_r);
    lv_screen_load(s_home);
    lv_test_wait(5);
    lv_obj_delete(s_scr);
    lv_test_wait(10);
}

static void click_obj(lv_obj_t *o)
{
    CHECK(o != NULL);
    if (o == NULL) {
        return;
    }
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    lv_test_mouse_move_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_mouse_press();
    lv_test_wait(50);
    lv_test_mouse_release();
    lv_test_wait(50);
}

/** The record the domain publishes in most tests: everything at its ADR-023 safe default. */
static ui_settings_view_t defaults(uint32_t seq)
{
    ui_settings_view_t v = {0};
    v.seq = seq;
    v.remote_enabled = false;
    v.pair_require_accept = true;                   // FR-732 default ON
    v.control_approval = UI_CONTROL_APPROVAL_ALWAYS; // FR-733 default
    v.confirm_alt_f4 = false;                       // FR-734 default OFF
    v.keepawake_enabled = false;                    // FR-711 default OFF
    v.keepawake_interval_s = 30u;                   // ADR-020 default
    v.unicode_mode = UI_UNICODE_MODE_HOST_IME;
    (void)snprintf(v.host_layout, sizeof(v.host_layout), "us");
    (void)snprintf(v.firmware_version, sizeof(v.firmware_version), "1.0.0+43a49f0");
    return v;
}

static bool open_card(void)
{
    const uint32_t rid = ui_actions_submit(&s_act, UI_ACT_OPEN_SETTINGS, UI_CTRL_SETTINGS_OPEN, 0, fake_now(),
                                           NULL);
    lv_test_wait(10);
    return rid != 0u && ui_view_settings_card(&s_set, UI_OVERLAY_SETTINGS) != NULL;
}

static bool contains(const char *haystack, const char *needle)
{
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

// ---- tests ---------------------------------------------------------------------------------

/** 07 §12 item 2: nothing published yet - every value is Unknown and no control accepts a tap. */
static void test_unknown_until_published(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK_EQ(s_set.c.opens, 1u);

    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_REMOTE_ENABLED), "Unknown"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT), "Unknown"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_CONFIRM_ALT_F4), "Unknown"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_KEEPAWAKE_ENABLED), "Unknown"));
    CHECK(contains(ui_view_settings_interval_text(&s_set), "Unknown"));
    CHECK(contains(ui_view_settings_layout_text(&s_set), "Unknown"));
    CHECK(contains(ui_view_settings_unicode_text(&s_set), "Unknown"));

    // No policy button is marked: an unknown policy is never rendered as one of the three values.
    for (int i = 0; i < UI_CONTROL_APPROVAL_COUNT; i++) {
        lv_obj_t *btn = ui_view_settings_approval_button(&s_set, (ui_control_approval_t)i);
        CHECK(btn != NULL);
        CHECK(!lv_obj_has_state(btn, LV_STATE_CHECKED));
        CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
    }
    CHECK(lv_obj_has_state(ui_view_settings_toggle(&s_set, UI_SETTING_REMOTE_ENABLED), LV_STATE_DISABLED));

    // A tap on a disabled control reports nothing and opens no confirmation.
    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_REMOTE_ENABLED));
    click_obj(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_AUTO));
    CHECK_EQ(s_change_n, 0);
    CHECK(!ui_view_settings_confirm_open(&s_set));
    stop();
}

/** A published record is what the card renders, in every pane. */
static void test_renders_the_published_record(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    v.remote_enabled = true;
    v.confirm_alt_f4 = true;
    v.keepawake_enabled = true;
    v.keepawake_interval_s = 120u;
    v.control_approval = UI_CONTROL_APPROVAL_TRUSTED;
    v.unicode_mode = UI_UNICODE_MODE_WINDOWS_HEX;
    (void)snprintf(v.host_layout, sizeof(v.host_layout), "de(neo)");
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);

    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_REMOTE_ENABLED), "On"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT), "On"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_CONFIRM_ALT_F4), "On"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_KEEPAWAKE_ENABLED), "On"));
    CHECK(contains(ui_view_settings_interval_text(&s_set), "120"));
    CHECK(contains(ui_view_settings_layout_text(&s_set), "de(neo)"));
    CHECK(contains(ui_view_settings_unicode_text(&s_set), "Windows"));
    CHECK(contains(ui_view_settings_version_text(&s_set), "1.0.0+43a49f0"));

    CHECK(lv_obj_has_state(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_TRUSTED),
                           LV_STATE_CHECKED));
    CHECK(!lv_obj_has_state(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_ALWAYS),
                            LV_STATE_CHECKED));
    CHECK(!lv_obj_has_state(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_AUTO),
                            LV_STATE_CHECKED));

    // FR-741 / FR-747: both values are read-only, and the pane says why.
    CHECK(ui_view_settings_toggle(&s_set, UI_SETTING_KEEPAWAKE_INTERVAL_S) == NULL);
    stop();
}

/**
 * AGENTS-UI MUST 6, the central invariant: a tap reports the change and the control does NOT move. It moves
 * only when the domain republishes - and if the domain refuses, the screen still shows the stored value.
 */
static void test_no_optimistic_toggle(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_REMOTE_ENABLED), "Off"));

    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_REMOTE_ENABLED));
    CHECK_EQ(s_change_n, 1);
    CHECK_EQ(s_changes[0].id, UI_SETTING_REMOTE_ENABLED);
    CHECK_EQ(s_changes[0].value, 1);
    // The domain has not answered: the legend is still Off and the button is not checked.
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_REMOTE_ENABLED), "Off"));
    CHECK(!lv_obj_has_state(ui_view_settings_toggle(&s_set, UI_SETTING_REMOTE_ENABLED), LV_STATE_CHECKED));

    // A refusal: the domain republishes the unchanged record. The screen never claimed otherwise.
    v.seq = 2u;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_REMOTE_ENABLED), "Off"));

    // The write went through: now - and only now - the control moves.
    v.seq = 3u;
    v.remote_enabled = true;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_REMOTE_ENABLED), "On"));
    CHECK(lv_obj_has_state(ui_view_settings_toggle(&s_set, UI_SETTING_REMOTE_ENABLED), LV_STATE_CHECKED));

    // Tightening a policy needs no confirmation and is reported straight away (FR-734 OFF -> ON). The
    // control lives in another section, so the section has to be shown first - a hidden control takes no tap.
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_SHORTCUTS));
    lv_test_wait(10);
    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_CONFIRM_ALT_F4));
    CHECK_EQ(s_change_n, 2);
    CHECK_EQ(s_changes[1].id, UI_SETTING_CONFIRM_ALT_F4);
    CHECK_EQ(s_changes[1].value, 1);
    CHECK(!ui_view_settings_confirm_open(&s_set));
    stop();
}

/** ADR-023: relaxing `pair.require_accept` needs the second tap; Keep as it is writes nothing. */
static void test_relax_require_accept_needs_confirmation(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);

    CHECK(ui_view_settings_relaxes_policy(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT, 0));
    CHECK(!ui_view_settings_relaxes_policy(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT, 1));

    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT));
    CHECK(ui_view_settings_confirm_open(&s_set));
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_SETTINGS_CONFIRM);
    CHECK_EQ(s_set.c.confirms_shown, 1u);
    CHECK_EQ(s_change_n, 0); // nothing written yet
    CHECK(contains(ui_view_settings_confirm_heading(&s_set), "Relax"));
    CHECK(contains(ui_view_settings_confirm_body(&s_set), "Accept"));

    click_obj(ui_view_settings_confirm_button(&s_set, false)); // Keep as it is
    lv_test_wait(30);
    CHECK(!ui_view_settings_confirm_open(&s_set));
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_SETTINGS); // the Settings card is still open underneath
    CHECK_EQ(s_change_n, 0);
    CHECK_EQ(s_set.c.confirms_no, 1u);
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT), "On"));

    // The same tap, confirmed this time.
    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT));
    CHECK(ui_view_settings_confirm_open(&s_set));
    click_obj(ui_view_settings_confirm_button(&s_set, true));
    lv_test_wait(30);
    CHECK(!ui_view_settings_confirm_open(&s_set));
    CHECK_EQ(s_change_n, 1);
    CHECK_EQ(s_changes[0].id, UI_SETTING_PAIR_REQUIRE_ACCEPT);
    CHECK_EQ(s_changes[0].value, 0);
    CHECK_EQ(s_set.c.confirms_yes, 1u);
    // Still On: the record has not been republished (MUST 6 holds across the confirmation too).
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT), "On"));
    stop();
}

/** FR-733: every step down the strictness scale is a relaxation; a step up is not. */
static void test_relax_control_approval(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v); // ALWAYS
    lv_test_wait(10);

    CHECK(ui_view_settings_relaxes_policy(&s_set, UI_SETTING_CONTROL_APPROVAL, UI_CONTROL_APPROVAL_AUTO));
    CHECK(ui_view_settings_relaxes_policy(&s_set, UI_SETTING_CONTROL_APPROVAL, UI_CONTROL_APPROVAL_TRUSTED));
    CHECK(!ui_view_settings_relaxes_policy(&s_set, UI_SETTING_CONTROL_APPROVAL, UI_CONTROL_APPROVAL_ALWAYS));

    click_obj(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_AUTO));
    CHECK(ui_view_settings_confirm_open(&s_set));
    CHECK_EQ(s_change_n, 0);
    click_obj(ui_view_settings_confirm_button(&s_set, true));
    lv_test_wait(30);
    CHECK_EQ(s_change_n, 1);
    CHECK_EQ(s_changes[0].id, UI_SETTING_CONTROL_APPROVAL);
    CHECK_EQ(s_changes[0].value, UI_CONTROL_APPROVAL_AUTO);
    // ALWAYS is still the marked value until the record comes back.
    CHECK(lv_obj_has_state(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_ALWAYS),
                           LV_STATE_CHECKED));

    v.seq = 2u;
    v.control_approval = UI_CONTROL_APPROVAL_AUTO;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(lv_obj_has_state(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_AUTO),
                           LV_STATE_CHECKED));

    // Going back up is strictly safer: reported immediately, no confirmation.
    click_obj(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_ALWAYS));
    CHECK(!ui_view_settings_confirm_open(&s_set));
    CHECK_EQ(s_change_n, 2);
    CHECK_EQ(s_changes[1].value, UI_CONTROL_APPROVAL_ALWAYS);

    // Tapping the policy that is already active writes nothing and asks nothing.
    const int before = s_change_n;
    click_obj(ui_view_settings_approval_button(&s_set, UI_CONTROL_APPROVAL_AUTO));
    lv_test_wait(10);
    CHECK(!ui_view_settings_confirm_open(&s_set));
    CHECK_EQ(s_change_n, before);
    stop();
}

/** ADR-022 FR-726 / FR-733: the register, the trusted flag and Forget behind a confirmation. */
static void test_paired_clients(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    v.remote_enabled = true;
    v.client_count = 2u;
    (void)snprintf(v.client[0].name, sizeof(v.client[0].name), "Studio PC");
    v.client[0].trusted = false;
    // SEC-105 / 07 §7: a name arrives from the network. Control bytes must not reach the label.
    (void)snprintf(v.client[1].name, sizeof(v.client[1].name), "bad\tname\x01");
    v.client[1].trusted = true;
    ui_view_settings_set(&s_set, &v);
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_CLIENTS));
    lv_test_wait(10);

    CHECK(contains(ui_view_settings_client_name_text(&s_set, 0), "Studio PC"));
    CHECK(contains(ui_view_settings_client_name_text(&s_set, 1), "bad.name."));
    CHECK(strchr(ui_view_settings_client_name_text(&s_set, 1), '\t') == NULL);
    CHECK(ui_view_settings_client_trusted_button(&s_set, 2) == NULL); // unused rows are not reachable

    // Clearing the trusted flag is stricter: reported straight away.
    click_obj(ui_view_settings_client_trusted_button(&s_set, 1));
    CHECK(!ui_view_settings_confirm_open(&s_set));
    CHECK_EQ(s_change_n, 1);
    CHECK_EQ(s_changes[0].id, UI_SETTING_CLIENT_TRUSTED);
    CHECK_EQ(s_changes[0].value, 0);
    CHECK_EQ(s_changes[0].index, 1);

    // Setting it lets that client take control without asking: confirmed.
    click_obj(ui_view_settings_client_trusted_button(&s_set, 0));
    CHECK(ui_view_settings_confirm_open(&s_set));
    CHECK(contains(ui_view_settings_confirm_body(&s_set), "Studio PC"));
    click_obj(ui_view_settings_confirm_button(&s_set, true));
    lv_test_wait(30);
    CHECK_EQ(s_change_n, 2);
    CHECK_EQ(s_changes[1].id, UI_SETTING_CLIENT_TRUSTED);
    CHECK_EQ(s_changes[1].value, 1);
    CHECK_EQ(s_changes[1].index, 0);

    // Forget always asks, and the question names the client.
    click_obj(ui_view_settings_client_forget_button(&s_set, 0));
    CHECK(ui_view_settings_confirm_open(&s_set));
    CHECK(contains(ui_view_settings_confirm_heading(&s_set), "Forget"));
    CHECK(contains(ui_view_settings_confirm_body(&s_set), "Studio PC"));
    CHECK_EQ(s_change_n, 2);
    click_obj(ui_view_settings_confirm_button(&s_set, false));
    lv_test_wait(30);
    CHECK_EQ(s_change_n, 2); // declined: nothing was forgotten

    click_obj(ui_view_settings_client_forget_button(&s_set, 1));
    click_obj(ui_view_settings_confirm_button(&s_set, true));
    lv_test_wait(30);
    CHECK_EQ(s_change_n, 3);
    CHECK_EQ(s_changes[2].id, UI_SETTING_CLIENT_FORGET);
    CHECK_EQ(s_changes[2].index, 1);

    // The register shrinks only when the domain says so, and the row goes away then.
    v.seq = 2u;
    v.client_count = 1u;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(ui_view_settings_client_forget_button(&s_set, 1) == NULL);
    CHECK(ui_view_settings_client_forget_button(&s_set, 0) != NULL);

    // A malformed publish cannot show more rows than the register holds.
    v.seq = 3u;
    v.client_count = 99u;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK_EQ(ui_view_settings_snapshot(&s_set)->client_count, UI_SETTINGS_CLIENTS_MAX);
    stop();
}

/** ADR-020: 10 s steps, clamped at both ends; a clamped tap reports nothing. */
static void test_keepawake_interval(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_INPUT));
    lv_test_wait(10);
    CHECK(contains(ui_view_settings_interval_text(&s_set), "30 s"));

    click_obj(ui_view_settings_interval_button(&s_set, true));
    CHECK_EQ(s_change_n, 1);
    CHECK_EQ(s_changes[0].id, UI_SETTING_KEEPAWAKE_INTERVAL_S);
    CHECK_EQ(s_changes[0].value, 40);
    CHECK(contains(ui_view_settings_interval_text(&s_set), "30 s")); // not optimistic either

    click_obj(ui_view_settings_interval_button(&s_set, false));
    CHECK_EQ(s_change_n, 2);
    CHECK_EQ(s_changes[1].value, 20);

    v.seq = 2u;
    v.keepawake_interval_s = (uint16_t)UI_SETTINGS_INTERVAL_MAX;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(lv_obj_has_state(ui_view_settings_interval_button(&s_set, true), LV_STATE_DISABLED));
    click_obj(ui_view_settings_interval_button(&s_set, true));
    CHECK_EQ(s_change_n, 2); // clamped: nothing out of range is ever reported

    v.seq = 3u;
    v.keepawake_interval_s = (uint16_t)UI_SETTINGS_INTERVAL_MIN;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(lv_obj_has_state(ui_view_settings_interval_button(&s_set, false), LV_STATE_DISABLED));
    click_obj(ui_view_settings_interval_button(&s_set, false));
    CHECK_EQ(s_change_n, 2);
    stop();
}

/** FR-747: the Unicode delivery stepper wraps, reports one change per tap, is never optimistic, shows the hint. */
static void test_unicode_mode_stepper(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    lv_test_wait(10);
    // unpublished: both buttons disabled, nothing reported
    CHECK(lv_obj_has_state(ui_view_settings_unicode_button(&s_set, true), LV_STATE_DISABLED));
    click_obj(ui_view_settings_unicode_button(&s_set, true));
    CHECK_EQ(s_change_n, 0);

    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(contains(ui_view_settings_unicode_text(&s_set), "Host IME"));
    CHECK(contains(ui_view_settings_unicode_hint(&s_set), "input method"));
    CHECK(!lv_obj_has_state(ui_view_settings_unicode_button(&s_set, true), LV_STATE_DISABLED));

    click_obj(ui_view_settings_unicode_button(&s_set, true));
    CHECK_EQ(s_change_n, 1);
    CHECK_EQ(s_changes[0].id, UI_SETTING_UNICODE_MODE);
    CHECK_EQ(s_changes[0].value, UI_UNICODE_MODE_LINUX);
    CHECK(contains(ui_view_settings_unicode_text(&s_set), "Host IME")); // not optimistic

    click_obj(ui_view_settings_unicode_button(&s_set, false)); // wraps below the first mode
    CHECK_EQ(s_change_n, 2);
    CHECK_EQ(s_changes[1].value, UI_UNICODE_MODE_WINCOMPOSE);

    v.seq = 2u;
    v.unicode_mode = UI_UNICODE_MODE_WINDOWS_HEX;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(contains(ui_view_settings_unicode_text(&s_set), "Windows"));
    CHECK(contains(ui_view_settings_unicode_hint(&s_set), "EnableHexNumpad"));
    click_obj(ui_view_settings_unicode_button(&s_set, true));
    CHECK_EQ(s_change_n, 3);
    CHECK_EQ(s_changes[2].value, UI_UNICODE_MODE_WINCOMPOSE);

    v.seq = 3u;
    v.unicode_mode = UI_UNICODE_MODE_WINCOMPOSE;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    click_obj(ui_view_settings_unicode_button(&s_set, true)); // wraps past the last mode
    CHECK_EQ(s_change_n, 4);
    CHECK_EQ(s_changes[3].value, UI_UNICODE_MODE_HOST_IME);
    stop();
}

/** ADR-037: the three input languages are ordinary 0/1 settings, reported once per tap and never optimistic. */
static void test_input_languages(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    lv_test_wait(10);

    static const ui_setting_id_t ids[3] = {UI_SETTING_LANG_KO, UI_SETTING_LANG_ZH, UI_SETTING_LANG_JA};
    // unpublished: Unknown, disabled, and a tap reports nothing
    for (uint8_t i = 0; i < 3u; i++) {
        CHECK(contains(ui_view_settings_toggle_text(&s_set, ids[i]), "Unknown"));
        CHECK(lv_obj_has_state(ui_view_settings_toggle(&s_set, ids[i]), LV_STATE_DISABLED));
        click_obj(ui_view_settings_toggle(&s_set, ids[i]));
    }
    CHECK_EQ(s_change_n, 0);

    // the record's default: all three off (English alone)
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    for (uint8_t i = 0; i < 3u; i++) {
        CHECK(contains(ui_view_settings_toggle_text(&s_set, ids[i]), "Off"));
        CHECK(!lv_obj_has_state(ui_view_settings_toggle(&s_set, ids[i]), LV_STATE_CHECKED));
        CHECK(!lv_obj_has_state(ui_view_settings_toggle(&s_set, ids[i]), LV_STATE_DISABLED));
    }

    // one tap per row, each reporting its own id; MUST 6: no switch moves before the domain answers
    for (uint8_t i = 0; i < 3u; i++) {
        click_obj(ui_view_settings_toggle(&s_set, ids[i]));
        CHECK_EQ(s_change_n, (int)i + 1);
        CHECK_EQ(s_changes[i].id, ids[i]);
        CHECK_EQ(s_changes[i].value, 1);
        CHECK(contains(ui_view_settings_toggle_text(&s_set, ids[i]), "Off"));
    }
    CHECK(!ui_view_settings_confirm_open(&s_set)); // a language is not an approval policy
    CHECK(!ui_view_settings_relaxes_policy(&s_set, UI_SETTING_LANG_KO, 0));

    // the domain wrote them: now the rows move, and a second tap asks for 0
    v.seq = 2u;
    v.lang_ko = true;
    v.lang_zh = true;
    v.lang_ja = true;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    for (uint8_t i = 0; i < 3u; i++) {
        CHECK(contains(ui_view_settings_toggle_text(&s_set, ids[i]), "On"));
        CHECK(lv_obj_has_state(ui_view_settings_toggle(&s_set, ids[i]), LV_STATE_CHECKED));
    }
    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_LANG_ZH));
    CHECK_EQ(s_change_n, 4);
    CHECK_EQ(s_changes[3].id, UI_SETTING_LANG_ZH);
    CHECK_EQ(s_changes[3].value, 0);

    // one language on, the others off: each row shows its own value, never a neighbor's
    v.seq = 3u;
    v.lang_ko = false;
    v.lang_zh = true;
    v.lang_ja = false;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_LANG_KO), "Off"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_LANG_ZH), "On"));
    CHECK(contains(ui_view_settings_toggle_text(&s_set, UI_SETTING_LANG_JA), "Off"));
    // the keys the diagnostics dump uses
    CHECK(contains(ui_setting_key(UI_SETTING_LANG_KO), "kbd.lang_ko"));
    CHECK(contains(ui_setting_key(UI_SETTING_LANG_ZH), "kbd.lang_zh"));
    CHECK(contains(ui_setting_key(UI_SETTING_LANG_JA), "kbd.lang_ja"));
    stop();
}

/** ADR-022 step 1 / SEC-106: the entry point closes the card and reports, and is off while remote is off. */
// ---- FR-741 HOST layout picker --------------------------------------------------------------
static bool layouts_present(void)
{
    if (kbd_layouts_count_get() == 0u) {
        printf("    no generated layout tables in this build: the row stays read-only (FR-741)\n");
        return false;
    }
    return true;
}

/** Press the on-screen keys of the picker's own search keyboard, one per character of `q`. */
static void type_query(const char *q)
{
    for (const char *c = q; *c != '\0'; c++) {
        bool found = false;
        for (uint8_t i = 0; i < UI_SETTINGS_QUERY_KEYS && !found; i++) {
            lv_obj_t *key = ui_view_settings_picker_key(&s_set, i);
            if (key != NULL && ui_view_settings_picker_key_char(&s_set, i) == *c) {
                click_obj(key);
                found = true;
            }
        }
        CHECK(found);
    }
}

/** Publish a record with the layout database reported as available and `id` stored. */
static void publish_layout(const char *id, bool db)
{
    ui_settings_view_t v = defaults(7u);
    (void)snprintf(v.host_layout, sizeof(v.host_layout), "%s", id);
    v.layout_db_available = db;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(5);
}

/**
 * ADR-039: the control opens the picker on its home screen (current layout, favorites, recents);
 * the flat list of layouts is one tap further, behind "All layouts".
 */
static bool open_picker_list(void)
{
    click_obj(ui_view_settings_layout_button(&s_set));
    CHECK(ui_view_settings_picker_open(&s_set));
    CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_HOME);
    CHECK(contains(ui_view_settings_picker_title(&s_set), "Host layout"));
    lv_obj_t *all = ui_view_settings_picker_all_button(&s_set);
    CHECK(all != NULL && !lv_obj_is_disabled(all));
    click_obj(all);
    CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_LIST);
    return true;
}

static void test_layout_row_is_read_only_without_tables(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    // The domain says it has no database: no control, nothing offered, and the stored id still readable.
    publish_layout("us", false);
    CHECK(ui_view_settings_layout_button(&s_set) == NULL ||
          lv_obj_is_disabled(ui_view_settings_layout_button(&s_set)));
    CHECK(contains(ui_view_settings_layout_text(&s_set), "us"));
    CHECK(!ui_view_settings_picker_open(&s_set));
    if (ui_view_settings_layout_button(&s_set) != NULL) {
        const uint32_t before = s_set.c.reported;
        click_obj(ui_view_settings_layout_button(&s_set));
        CHECK_EQ(s_set.c.reported, before); // a disabled control writes nothing
        CHECK(!ui_view_settings_picker_open(&s_set));
    }
    stop();
}

static void test_layout_picker_lists_and_pages(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        publish_layout("us", true);
        // The row names the stored layout, not just its id.
        CHECK(contains(ui_view_settings_layout_text(&s_set), "us"));
        CHECK(contains(ui_view_settings_layout_text(&s_set), "English"));
        CHECK(ui_view_settings_layout_button(&s_set) != NULL);
        CHECK(open_picker_list());
        CHECK(contains(ui_view_settings_picker_title(&s_set), "All layouts"));
        printf("    picker: %s, page %s, first row \"%s\"\n", ui_view_settings_picker_title(&s_set),
               ui_view_settings_picker_page_text(&s_set), ui_view_settings_picker_row_text(&s_set, 0));
        // It opens on the page that holds the stored layout, and that row is marked.
        int marked = -1;
        for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
            if (contains(ui_view_settings_picker_row_text(&s_set, r), "us  English (US)")) {
                marked = r;
            }
        }
        CHECK(marked >= 0);
        CHECK(contains(ui_view_settings_picker_row_text(&s_set, (uint8_t)marked), LV_SYMBOL_OK));
        // Paging stays within the list and allocates nothing (the rows are the same widgets).
        lv_obj_t *first_row = ui_view_settings_picker_row(&s_set, 0);
        const char *page_one = ui_view_settings_picker_page_text(&s_set);
        char before[24];
        (void)snprintf(before, sizeof(before), "%s", page_one);
        // "us" sits near the end of the list, so the page it opens on may be the last one: step whichever way
        // is available and back again.
        const bool forward = !lv_obj_is_disabled(ui_view_settings_picker_step(&s_set, true));
        CHECK(forward || !lv_obj_is_disabled(ui_view_settings_picker_step(&s_set, false)));
        click_obj(ui_view_settings_picker_step(&s_set, forward));
        CHECK(!contains(ui_view_settings_picker_page_text(&s_set), before));
        CHECK(ui_view_settings_picker_row(&s_set, 0) == first_row); // the same widgets, new text
        click_obj(ui_view_settings_picker_step(&s_set, !forward));
        CHECK(contains(ui_view_settings_picker_page_text(&s_set), before));
        // Back walks the screens out, one level at a time, without reporting anything.
        const uint32_t reported = s_set.c.reported;
        click_obj(ui_view_settings_picker_back(&s_set));
        CHECK(ui_view_settings_picker_open(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_HOME);
        click_obj(ui_view_settings_picker_back(&s_set));
        CHECK(!ui_view_settings_picker_open(&s_set));
        CHECK_EQ(s_set.c.reported, reported);
    }
    stop();
}

static void test_layout_picker_two_levels_and_pick(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        publish_layout("us", true);
        CHECK(open_picker_list());
        // Find the row of a layout that has variants and open its second level.
        int row = -1;
        for (int guard = 0; guard < 200 && row < 0; guard++) {
            for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
                if (contains(ui_view_settings_picker_row_text(&s_set, r), "variants")) {
                    row = r;
                    break;
                }
            }
            if (row < 0) {
                lv_obj_t *next = ui_view_settings_picker_step(&s_set, true);
                CHECK(next != NULL);
                if (lv_obj_is_disabled(next)) {
                    break;
                }
                click_obj(next);
            }
        }
        CHECK(row >= 0);
        char first_level[UI_SETTINGS_PICK_TEXT];
        (void)snprintf(first_level, sizeof(first_level), "%s",
                       ui_view_settings_picker_row_text(&s_set, (uint8_t)row));
        const uint32_t reported = s_set.c.reported;
        click_obj(ui_view_settings_picker_row(&s_set, (uint8_t)row));
        CHECK(ui_view_settings_picker_open(&s_set));
        CHECK_EQ(s_set.c.reported, reported); // opening a level is not a pick
        CHECK(contains(ui_view_settings_picker_title(&s_set), "variant"));
        printf("    level 1 row \"%s\" -> level 2 \"%s\", first variant \"%s\"\n", first_level,
               ui_view_settings_picker_title(&s_set), ui_view_settings_picker_row_text(&s_set, 0));
        // Back goes one level up, not out.
        click_obj(ui_view_settings_picker_back(&s_set));
        CHECK(ui_view_settings_picker_open(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_LIST);
        // Down again and pick the second variant: one change, reported as a table index, and the list closes.
        click_obj(ui_view_settings_picker_row(&s_set, (uint8_t)row));
        const char *txt = ui_view_settings_picker_row_text(&s_set, 1);
        char picked[UI_SETTINGS_PICK_TEXT];
        (void)snprintf(picked, sizeof(picked), "%s", txt);
        CHECK(picked[0] != '\0');
        click_obj(ui_view_settings_picker_row(&s_set, 1));
        CHECK(!ui_view_settings_picker_open(&s_set));
        CHECK_EQ(s_set.c.reported, reported + 1u);
        CHECK_EQ(s_change_n, 1);
        CHECK_EQ((int)s_changes[0].id, (int)UI_SETTING_HOST_LAYOUT);
        const kbd_layout_t *lay = kbd_layouts_at((size_t)s_changes[0].value);
        CHECK(lay != NULL);
        CHECK(contains(picked, lay->id));
        printf("    picked \"%s\" -> index %d (%s)\n", picked, (int)s_changes[0].value, lay->id);
        // MUST 6: the row still shows the stored layout, because the domain has not republished.
        CHECK(contains(ui_view_settings_layout_text(&s_set), "us"));
        // ... and when it does, the row follows it.
        publish_layout(lay->id, true);
        CHECK(contains(ui_view_settings_layout_text(&s_set), lay->id));
    }
    stop();
}

static void test_layout_picker_closes_when_the_database_goes_away(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        publish_layout("us", true);
        click_obj(ui_view_settings_layout_button(&s_set));
        CHECK(ui_view_settings_picker_open(&s_set));
        publish_layout("us", false);
        CHECK(!ui_view_settings_picker_open(&s_set)); // never a stale list over a record that cannot change
        // An unpublished record (seq 0) is the same story: fail closed.
        click_obj(ui_view_settings_layout_button(&s_set));
        CHECK(!ui_view_settings_picker_open(&s_set));
    }
    stop();
}

static void test_layout_id_the_tables_do_not_have(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    // An id that is not in the tables (or a build without them) is shown as it came, with no name invented and
    // no row marked.
    publish_layout("zz(nope)", true);
    CHECK(contains(ui_view_settings_layout_text(&s_set), "zz(nope)"));
    if (layouts_present()) {
        CHECK(open_picker_list());
        for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
            CHECK(!contains(ui_view_settings_picker_row_text(&s_set, r), LV_SYMBOL_OK));
        }
        CHECK(contains(ui_view_settings_picker_page_text(&s_set), "1 /")); // from the top of the list
    }
    stop();
}

/** Held past LVGL's long-press time, so LONG_PRESSED fires and the short click is suppressed. */
static void long_press_obj(lv_obj_t *o)
{
    CHECK(o != NULL);
    if (o == NULL) {
        return;
    }
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    lv_test_mouse_move_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_mouse_press();
    lv_test_wait(700);
    lv_test_mouse_release();
    lv_test_wait(50);
}

/** The table index of `id`, or -1 when this build has no such layout. */
static int index_of(const char *id)
{
    const kbd_layout_t *lay = kbd_layouts_find(id);
    return lay == NULL ? -1 : (int)(lay - kbd_layouts_at(0));
}

/** Publish a record with the layout database available, `id` stored, and the two ADR-039 lists filled. */
static void publish_lists(const char *id, const char *const *fav, uint8_t fav_n,
                          const char *const *recent, uint8_t recent_n)
{
    ui_settings_view_t v = defaults(9u);
    (void)snprintf(v.host_layout, sizeof(v.host_layout), "%s", id);
    v.layout_db_available = true;
    for (uint8_t i = 0; i < fav_n && i < UI_SETTINGS_FAV_MAX; i++) {
        (void)snprintf(v.host_fav[i], sizeof(v.host_fav[i]), "%s", fav[i]);
    }
    v.host_fav_count = fav_n;
    for (uint8_t i = 0; i < recent_n && i < UI_SETTINGS_RECENT_MAX; i++) {
        (void)snprintf(v.host_recent[i], sizeof(v.host_recent[i]), "%s", recent[i]);
    }
    v.host_recent_count = recent_n;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(5);
}

/**
 * ADR-039 sections 2-4: the home screen shows the current layout with a home-row preview, the favorites in
 * the order the record stores them, and the recents newest first with the pinned ones left out. A tap on
 * either applies that layout.
 */
static void test_layout_home_favorites_and_recents(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        static const char *const fav[] = {"us", "de", "fr"};
        // "de" and "us" are pinned, so only "pl" is left to show as a recent, and it is the newest one.
        static const char *const recent[] = {"pl", "de", "us"};
        publish_lists("us", fav, 3u, recent, 3u);
        click_obj(ui_view_settings_layout_button(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_HOME);
        // 1: the current layout, named, with a preview of its home row.
        CHECK(contains(ui_view_settings_picker_current_text(&s_set), "us"));
        CHECK(contains(ui_view_settings_picker_current_text(&s_set), "English"));
        int legends = 0;
        for (uint8_t k = 0; k < UI_SETTINGS_PREVIEW_KEYS; k++) {
            if (ui_view_settings_picker_preview_text(&s_set, k)[0] != '\0') {
                legends++;
            }
        }
        printf("    home: \"%s\", %d preview legends\n", ui_view_settings_picker_current_text(&s_set), legends);
        CHECK(legends >= 8);

        // 2: three tiles, in the record's order, and no fourth one.
        CHECK(contains(ui_view_settings_picker_fav_text(&s_set, 0), "us"));
        CHECK(contains(ui_view_settings_picker_fav_text(&s_set, 1), "de"));
        CHECK(contains(ui_view_settings_picker_fav_text(&s_set, 2), "fr"));
        CHECK(ui_view_settings_picker_fav(&s_set, 3) == NULL);

        // 3: one chip, because the other two recents are pinned above.
        CHECK(contains(ui_view_settings_picker_recent_text(&s_set, 0), "pl"));
        CHECK(ui_view_settings_picker_recent(&s_set, 1) == NULL);

        // A tap on a chip applies that layout and closes the picker; the row waits for the domain (MUST 5).
        const uint32_t reported = s_set.c.reported;
        click_obj(ui_view_settings_picker_recent(&s_set, 0));
        CHECK(!ui_view_settings_picker_open(&s_set));
        CHECK_EQ(s_set.c.reported, reported + 1u);
        CHECK_EQ(s_change_n, 1);
        CHECK_EQ((int)s_changes[0].id, (int)UI_SETTING_HOST_LAYOUT);
        CHECK_EQ((int)s_changes[0].value, index_of("pl"));
        CHECK(contains(ui_view_settings_layout_text(&s_set), "us"));

        // A tap on a tile does the same.
        s_change_n = 0;
        click_obj(ui_view_settings_layout_button(&s_set));
        click_obj(ui_view_settings_picker_fav(&s_set, 1));
        CHECK(!ui_view_settings_picker_open(&s_set));
        CHECK_EQ(s_change_n, 1);
        CHECK_EQ((int)s_changes[0].id, (int)UI_SETTING_HOST_LAYOUT);
        CHECK_EQ((int)s_changes[0].value, index_of("de"));
    }
    stop();
}

/**
 * ADR-039 section 3: the star pins and unpins, a long press on a tile unpins it, and a ninth favorite is
 * refused instead of pushing one of the eight out. Every one of those is a request, never a local edit.
 */
static void test_layout_favorites_pin_unpin_and_limit(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        static const char *const one[] = {"us"};
        publish_lists("us", one, 1u, NULL, 0u);
        CHECK(open_picker_list());
        // The star of the first row asks for a pin, as a typed change with that layout's table index.
        s_change_n = 0;
        char row0[UI_SETTINGS_PICK_TEXT];
        (void)snprintf(row0, sizeof(row0), "%s", ui_view_settings_picker_row_text(&s_set, 0));
        click_obj(ui_view_settings_picker_star(&s_set, 0));
        CHECK_EQ(s_change_n, 1);
        CHECK_EQ((int)s_changes[0].id, (int)UI_SETTING_HOST_FAV_TOGGLE);
        // The index it reports is the entry that row names - the base of the group, not whatever page it is on.
        const kbd_layout_t *starred = kbd_layouts_at((size_t)s_changes[0].value);
        CHECK(starred != NULL);
        CHECK(contains(row0, starred->id));
        CHECK(ui_view_settings_picker_open(&s_set)); // pinning is not picking: the picker stays
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_LIST);

        // A long press on a pinned tile asks for the unpin and does not apply the layout.
        click_obj(ui_view_settings_picker_back(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_HOME);
        s_change_n = 0;
        long_press_obj(ui_view_settings_picker_fav(&s_set, 0));
        CHECK_EQ(s_change_n, 1);
        CHECK_EQ((int)s_changes[0].id, (int)UI_SETTING_HOST_FAV_TOGGLE);
        CHECK_EQ((int)s_changes[0].value, index_of("us"));
        CHECK(ui_view_settings_picker_open(&s_set));

        // Eight pinned: the star of an unpinned row is refused, with a note and nothing reported.
        static const char *const eight[] = {"us", "de", "fr", "pl", "es", "it", "cz", "se"};
        publish_lists("us", eight, 8u, NULL, 0u);
        CHECK(ui_view_settings_picker_open(&s_set));
        CHECK(open_picker_list() || true); // already open: this re-enters the list from home
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_LIST);
        s_change_n = 0;
        const uint32_t refused = s_set.c.refused;
        click_obj(ui_view_settings_picker_star(&s_set, 0));
        CHECK_EQ(s_change_n, 0);
        CHECK_EQ(s_set.c.refused, refused + 1u);
        CHECK(contains(ui_view_settings_picker_note(&s_set), "8 favorites"));
        printf("    the ninth favorite: \"%s\"\n", ui_view_settings_picker_note(&s_set));
        // Unpinning one of the eight is still allowed, because it frees a slot instead of taking one.
        click_obj(ui_view_settings_picker_back(&s_set));
        s_change_n = 0;
        long_press_obj(ui_view_settings_picker_fav(&s_set, 7));
        CHECK_EQ(s_change_n, 1);
        CHECK_EQ((int)s_changes[0].id, (int)UI_SETTING_HOST_FAV_TOGGLE);
        CHECK_EQ((int)s_changes[0].value, index_of("se"));
    }
    stop();
}

/** ADR-039 section 5: the search field matches the name and the id, case aside, and its keyboard is its own. */
static void test_layout_search_matches_name_and_id(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        publish_layout("us", true);
        click_obj(ui_view_settings_layout_button(&s_set));
        click_obj(ui_view_settings_picker_search_button(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_SEARCH);
        CHECK(ui_view_settings_picker_key(&s_set, 0) != NULL);

        // By name: "pol" is nowhere in an id, so this is the name column matching.
        type_query("pol");
        CHECK(contains(ui_view_settings_picker_query(&s_set), "pol"));
        int hit = -1;
        for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
            if (contains(ui_view_settings_picker_row_text(&s_set, r), "Polish")) {
                hit = r;
            }
        }
        printf("    search \"pol\": title \"%s\", first row \"%s\"\n", ui_view_settings_picker_title(&s_set),
               ui_view_settings_picker_row_text(&s_set, 0));
        CHECK(hit >= 0);

        // By id, and the id prefix ranks first: "de" leads with the German layouts, not with "Swedish".
        click_obj(ui_view_settings_picker_clear(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_query(&s_set)[0], 0);
        type_query("de");
        CHECK(contains(ui_view_settings_picker_row_text(&s_set, 0), "de"));
        printf("    search \"de\": first row \"%s\"\n", ui_view_settings_picker_row_text(&s_set, 0));

        // The erase key takes the last character back, and the result set follows.
        click_obj(ui_view_settings_picker_erase(&s_set));
        CHECK(contains(ui_view_settings_picker_query(&s_set), "d"));
        CHECK_EQ((int)ui_view_settings_picker_query(&s_set)[1], 0);

        // A query nothing matches says so and offers no row to tap.
        type_query("zzz");
        CHECK(contains(ui_view_settings_picker_title(&s_set), "0 found"));
        CHECK(ui_view_settings_picker_row(&s_set, 0) == NULL);

        // Picking a search result applies it like any other row.
        click_obj(ui_view_settings_picker_clear(&s_set));
        type_query("pol");
        s_change_n = 0;
        click_obj(ui_view_settings_picker_row(&s_set, 0));
        CHECK(!ui_view_settings_picker_open(&s_set));
        CHECK_EQ(s_change_n, 1);
        CHECK_EQ((int)s_changes[0].id, (int)UI_SETTING_HOST_LAYOUT);
        const kbd_layout_t *lay = kbd_layouts_at((size_t)s_changes[0].value);
        CHECK(lay != NULL && contains(lay->name, "Polish"));
    }
    stop();
}

/**
 * A stored id the tables no longer have (a regenerated xkeyboard-config) must not take the screen with it:
 * it is skipped in both lists, and everything around it still works.
 */
static void test_layout_lists_skip_ids_the_tables_lost(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        static const char *const fav[] = {"zz(nope)", "de"};
        static const char *const recent[] = {"zz(gone)", "pl"};
        publish_lists("us", fav, 2u, recent, 2u);
        click_obj(ui_view_settings_layout_button(&s_set));
        CHECK(ui_view_settings_picker_open(&s_set));
        // One tile and one chip: the unknown ids are not drawn, and the known ones keep their places.
        CHECK(contains(ui_view_settings_picker_fav_text(&s_set, 0), "de"));
        CHECK(ui_view_settings_picker_fav(&s_set, 1) == NULL);
        CHECK(contains(ui_view_settings_picker_recent_text(&s_set, 0), "pl"));
        CHECK(ui_view_settings_picker_recent(&s_set, 1) == NULL);
        // The list below is unharmed.
        click_obj(ui_view_settings_picker_all_button(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_LIST);
        CHECK(ui_view_settings_picker_row_text(&s_set, 0)[0] != '\0');
    }
    stop();
}

/** ADR-039 section 7: the A-Z strip jumps the list, and an empty bucket is disabled, not a trap. */
static void test_layout_index_strip_jumps(void)
{
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
    if (layouts_present()) {
        publish_layout("us", true);
        CHECK(open_picker_list());
        CHECK(ui_view_settings_picker_index(&s_set, 0) != NULL);
        // The list opens where the stored layout is, so take the top of the list as the reference.
        click_obj(ui_view_settings_picker_index(&s_set, 0));
        CHECK(contains(ui_view_settings_picker_page_text(&s_set), "1 /"));
        char first[UI_SETTINGS_PICK_TEXT];
        (void)snprintf(first, sizeof(first), "%s", ui_view_settings_picker_row_text(&s_set, 0));
        // The last bucket the tables fill is further down the list than the first one; an empty bucket is
        // disabled, and clicking it would be a no-op, so find the last enabled one.
        uint8_t last = 0u;
        for (uint8_t b = 0; b < UI_SETTINGS_PICK_INDEX; b++) {
            if (!lv_obj_is_disabled(ui_view_settings_picker_index(&s_set, b))) {
                last = b;
            }
        }
        CHECK(last > 0u);
        click_obj(ui_view_settings_picker_index(&s_set, last));
        printf("    index Y-Z: page %s, first row \"%s\"\n", ui_view_settings_picker_page_text(&s_set),
               ui_view_settings_picker_row_text(&s_set, 0));
        CHECK(!contains(ui_view_settings_picker_row_text(&s_set, 0), first));
        click_obj(ui_view_settings_picker_index(&s_set, 0));
        CHECK(contains(ui_view_settings_picker_page_text(&s_set), "1 /"));
        CHECK(contains(ui_view_settings_picker_row_text(&s_set, 0), first));
    }
    stop();
}

static void test_pair_entry_point(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v); // remote off
    lv_test_wait(10);
    CHECK(lv_obj_has_state(ui_view_settings_pair_button(&s_set), LV_STATE_DISABLED));
    click_obj(ui_view_settings_pair_button(&s_set));
    CHECK_EQ(s_pair_calls, 0);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_SETTINGS);

    v.seq = 2u;
    v.remote_enabled = true;
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(!lv_obj_has_state(ui_view_settings_pair_button(&s_set), LV_STATE_DISABLED));
    click_obj(ui_view_settings_pair_button(&s_set));
    lv_test_wait(30);
    CHECK_EQ(s_pair_calls, 1);
    CHECK_EQ(s_set.c.pair_taps, 1u);
    // The pairing prompt is a blocking overlay of its own: the Settings card is gone first.
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK(ui_view_settings_card(&s_set, UI_OVERLAY_SETTINGS) == NULL);
    stop();

    // No entry point installed: no button at all.
    s_pair_on = false;
    CHECK(start());
    CHECK(open_card());
    CHECK(ui_view_settings_pair_button(&s_set) == NULL);
    stop();
    s_pair_on = true;
}

/** Without a change hook every control is disabled, but About and the published values still read. */
static void test_without_a_sink_nothing_is_offered(void)
{
    s_sink_on = false;
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);
    CHECK(lv_obj_has_state(ui_view_settings_toggle(&s_set, UI_SETTING_REMOTE_ENABLED), LV_STATE_DISABLED));
    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_REMOTE_ENABLED));
    CHECK_EQ(s_change_n, 0);
    CHECK(!ui_view_settings_confirm_open(&s_set));
    CHECK(contains(ui_view_settings_version_text(&s_set), "1.0.0"));
    stop();
    s_sink_on = true;
}

/** FR-751 / TECH-084: About carries the Required Notice and the generated third-party text. */
static void test_about_carries_the_required_notice(void)
{
    CHECK(contains(ui_notices_license(), "SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0"));
    CHECK(contains(ui_notices_license(), "Required Notice: Copyright 2026 D-Ogi"));
    CHECK(contains(ui_notices_license(), "https://github.com/D-Ogi/esp32-s3-kvmpad"));
    CHECK(contains(ui_notices_third_party(), "LVGL"));
    CHECK(contains(ui_notices_third_party(), "PolyForm Noncommercial 1.0.0"));
    // The generated text is plain ASCII: the About pane renders it with the built-in Montserrat subsets.
    for (const char *p = ui_notices_third_party(); *p != '\0'; p++) {
        CHECK((unsigned char)*p < 0x80u);
    }

    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_ABOUT));
    lv_test_wait(10);
    CHECK_EQ(ui_view_settings_section(&s_set), UI_SETTINGS_SEC_ABOUT);
    CHECK(contains(ui_view_settings_version_text(&s_set), "1.0.0+43a49f0"));
    stop();
}

/** Close, the section list, and a card that is dismissed while the confirmation is up. */
static void test_navigation_and_close(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    lv_test_wait(10);

    for (uint8_t i = 0; i < UI_SETTINGS_SEC_COUNT; i++) {
        click_obj(s_set.tab[i]);
        CHECK_EQ(ui_view_settings_section(&s_set), (ui_settings_section_t)i);
        CHECK(lv_obj_has_state(s_set.tab[i], LV_STATE_CHECKED));
    }
    CHECK_EQ(s_set.c.section_taps, UI_SETTINGS_SEC_COUNT);
    CHECK(!ui_view_settings_show_section(&s_set, (ui_settings_section_t)UI_SETTINGS_SEC_COUNT));

    // A pending confirmation that is torn down writes nothing (fail closed).
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_REMOTE));
    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT));
    CHECK(ui_view_settings_confirm_open(&s_set));
    ui_overlay_dismiss_all(&s_r);
    lv_test_wait(30);
    CHECK_EQ(s_change_n, 0);
    CHECK(!ui_view_settings_confirm_open(&s_set));
    CHECK(ui_view_settings_card(&s_set, UI_OVERLAY_SETTINGS) == NULL);

    // Close submits UI_ACT_DISMISS_OVERLAY through the gateway.
    CHECK(open_card());
    lv_obj_t *card = ui_view_settings_card(&s_set, UI_OVERLAY_SETTINGS);
    lv_obj_t *close = NULL;
    lv_obj_t *header = lv_obj_get_child(card, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(header); i++) {
        lv_obj_t *child = lv_obj_get_child(header, i);
        if (lv_obj_check_type(child, &lv_button_class)) {
            close = child;
        }
    }
    click_obj(close);
    lv_test_wait(30);
    CHECK_EQ(s_set.c.close_taps, 1u);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK_EQ(s_set.c.closes, 2u);
    stop();
}

/** Every tappable box of every section is at least UI_TOUCH_MIN and stays inside the panel (AT-UIFW-01). */
static const char *s_probe_tag = "?";

static void probe_geometry(void)
{
    lv_obj_t *card = ui_view_settings_card(&s_set, UI_OVERLAY_SETTINGS);
    CHECK(card != NULL);
    lv_obj_update_layout(card);
    lv_area_t ca;
    lv_obj_get_coords(card, &ca);
    CHECK(ca.x1 >= 0 && ca.y1 >= 0 && ca.x2 < SCR_W && ca.y2 < SCR_H);

    lv_obj_t *stack[256];
    uint32_t n = 0;
    stack[n++] = card;
    while (n > 0) {
        lv_obj_t *obj = stack[--n];
        const uint32_t kids = lv_obj_get_child_count(obj);
        for (uint32_t i = 0; i < kids && n < 256; i++) {
            lv_obj_t *child = lv_obj_get_child(obj, i);
            if (lv_obj_is_hidden(child)) {
                continue;
            }
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            if (lv_obj_is_clickable(child)) {
                CHECK(a.x2 - a.x1 + 1 >= UI_TOUCH_MIN);
                CHECK(a.y2 - a.y1 + 1 >= UI_TOUCH_MIN);
                CHECK(a.x1 >= ca.x1 && a.x2 <= ca.x2 && a.y1 >= ca.y1 && a.y2 <= ca.y2);
                // A control has to be reachable, not merely present: LVGL searches the touch down the tree
                // and never enters a parent the point is outside of, so an overflowing control is dead.
                const lv_point_t mid = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
                for (lv_obj_t *up = lv_obj_get_parent(child); up != NULL; up = lv_obj_get_parent(up)) {
                    lv_area_t ua;
                    lv_obj_get_coords(up, &ua);
                    if (!(mid.x >= ua.x1 && mid.x <= ua.x2 && mid.y >= ua.y1 && mid.y <= ua.y2)) {
                        const char *what = "";
                        if (lv_obj_get_child_count(child) > 0 &&
                            lv_obj_check_type(lv_obj_get_child(child, 0), &lv_label_class)) {
                            what = lv_label_get_text(lv_obj_get_child(child, 0));
                        }
                        printf("    out of reach [%s] \"%s\": child x%d..%d y%d..%d under x%d..%d y%d..%d\n",
                               s_probe_tag, what, a.x1, a.x2, a.y1, a.y2, ua.x1, ua.x2, ua.y1, ua.y2);
                        CHECK(false);
                        break;
                    }
                }
            }
            stack[n++] = child;
        }
    }
}

static void test_geometry_of_every_section(void)
{
    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    v.remote_enabled = true;
    v.client_count = UI_SETTINGS_CLIENTS_MAX; // the worst case: a full register
    for (uint8_t i = 0; i < UI_SETTINGS_CLIENTS_MAX; i++) {
        (void)snprintf(v.client[i].name, sizeof(v.client[i].name), "Long client name %u", (unsigned)i);
        v.client[i].trusted = (i % 2u) == 0u;
    }
    ui_view_settings_set(&s_set, &v);
    for (uint8_t i = 0; i < UI_SETTINGS_SEC_COUNT; i++) {
        CHECK(ui_view_settings_show_section(&s_set, (ui_settings_section_t)i));
        lv_test_wait(10);
        s_probe_tag = "section";
        probe_geometry();
    }

    // ADR-039: the picker, on each of its four screens, with the favorites filled to the brim - the state
    // whose home screen is tallest. The register goes back to empty first: a card full of long client names
    // squeezes the pane itself, which is a question about the Clients section, not about this picker.
    {
        ui_settings_view_t clean = defaults(2u);
        ui_view_settings_set(&s_set, &clean);
        lv_test_wait(10);
    }
    if (layouts_present()) {
        static const char *const eight[] = {"us", "de", "fr", "pl", "es", "it", "cz", "se"};
        static const char *const recent[] = {"ru", "jp", "br", "gb", "tr"};
        publish_lists("us", eight, 8u, recent, 5u);
        CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_KEYBOARD));
        click_obj(ui_view_settings_layout_button(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_HOME);
        s_probe_tag = "pick home";
        probe_geometry();
        click_obj(ui_view_settings_picker_all_button(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_LIST);
        s_probe_tag = "pick list";
        probe_geometry();
        // The second level, on a row that has variants.
        for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
            if (contains(ui_view_settings_picker_row_text(&s_set, r), "variants")) {
                click_obj(ui_view_settings_picker_row(&s_set, r));
                break;
            }
        }
        if (ui_view_settings_picker_screen(&s_set) == UI_PICK_VARIANTS) {
            s_probe_tag = "pick variants";
            probe_geometry();
            click_obj(ui_view_settings_picker_back(&s_set));
        }
        click_obj(ui_view_settings_picker_back(&s_set));
        click_obj(ui_view_settings_picker_search_button(&s_set));
        CHECK_EQ((int)ui_view_settings_picker_screen(&s_set), (int)UI_PICK_SEARCH);
        type_query("de");
        s_probe_tag = "pick search";
        probe_geometry();
    }
    stop();
}

/** AT-UIFW-09: open/close and section changes return to the same live allocation count. */
static void test_lifecycle(void)
{
    CHECK(start());
    ui_settings_view_t v = defaults(1u);
    v.remote_enabled = true;
    v.client_count = 3u;
    for (uint8_t i = 0; i < 3u; i++) {
        (void)snprintf(v.client[i].name, sizeof(v.client[i].name), "Client %u", (unsigned)i);
    }
    ui_view_settings_set(&s_set, &v);

    CHECK(open_card());
    for (int i = 0; i < 24; i++) {
        CHECK(ui_view_settings_show_section(&s_set, (ui_settings_section_t)(i % UI_SETTINGS_SEC_COUNT)));
    }
    ui_overlay_dismiss(&s_r);
    lv_test_wait(30);

    const size_t base = host_lv_live_count();
    for (int i = 0; i < 200; i++) {
        CHECK(open_card());
        CHECK(ui_view_settings_show_section(&s_set, (ui_settings_section_t)(i % UI_SETTINGS_SEC_COUNT)));
        CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_CLIENTS));
        ui_overlay_dismiss(&s_r);
        lv_test_wait(10);
    }
    const size_t after = host_lv_live_count();
    printf("    200 Settings open/close cycles: live allocations %zu -> %zu\n", base, after);
    CHECK_EQ(after, base);
    CHECK_EQ(s_set.c.opens, 201u);
    CHECK_EQ(s_set.c.closes, 201u);
    stop();
}

/** Bad arguments must be refused, not crash: the public API accepts NULL and out-of-range ids. */
static void test_refuses_bad_arguments(void)
{
    ui_view_settings_init(NULL, NULL, NULL, NULL, NULL, NULL);
    ui_view_settings_set(NULL, NULL);
    ui_view_settings_set_pair_action(NULL, NULL, NULL);
    CHECK(ui_view_settings_overlays(NULL, NULL) == NULL);
    CHECK(ui_view_settings_card(NULL, UI_OVERLAY_SETTINGS) == NULL);
    CHECK(!ui_view_settings_show_section(NULL, UI_SETTINGS_SEC_ABOUT));
    CHECK(!ui_view_settings_confirm_open(NULL));
    CHECK(ui_view_settings_snapshot(NULL) == NULL);
    CHECK(ui_view_settings_toggle(NULL, UI_SETTING_REMOTE_ENABLED) == NULL);
    CHECK(ui_view_settings_toggle_text(NULL, UI_SETTING_COUNT) == NULL);
    CHECK(ui_view_settings_approval_button(NULL, UI_CONTROL_APPROVAL_COUNT) == NULL);
    CHECK(ui_view_settings_client_trusted_button(NULL, 200) == NULL);
    CHECK(!ui_view_settings_relaxes_policy(NULL, UI_SETTING_CONTROL_APPROVAL, 0));

    CHECK(start());
    CHECK(open_card());
    ui_settings_view_t v = defaults(1u);
    ui_view_settings_set(&s_set, &v);
    CHECK(ui_view_settings_card(&s_set, 0xFFFFu) == NULL);
    CHECK(ui_view_settings_toggle(&s_set, UI_SETTING_COUNT) == NULL);
    // The two per-client ids have no 0/1 control of their own.
    CHECK(ui_view_settings_toggle(&s_set, UI_SETTING_CLIENT_FORGET) == NULL);
    CHECK(!ui_view_settings_relaxes_policy(&s_set, UI_SETTING_CONFIRM_ALT_F4, 0));
    stop();

    // The stable ids the diagnostics dump prints.
    CHECK(contains(ui_setting_key(UI_SETTING_CONTROL_APPROVAL), "control.approval"));
    CHECK(contains(ui_setting_key(UI_SETTING_KEEPAWAKE_INTERVAL_S), "keepawake.interval_s"));
    CHECK(contains(ui_setting_key(UI_SETTING_COUNT), "unknown"));
}

#define RUN_MEM(t)                                       \
    do {                                                 \
        size_t before_ = host_lv_live_count();           \
        RUN(t);                                          \
        CHECK_EQ(host_lv_live_count(), before_);         \
    } while (0)

int main(void)
{
    printf("test_ui_view_settings (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    lv_test_indev_create_all();
    CHECK(ui_theme_init());
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);

    CHECK(start()); // warm-up: first card, first confirmation, first press
    CHECK(open_card());
    ui_settings_view_t warm = defaults(1u);
    warm.remote_enabled = true;
    warm.client_count = UI_SETTINGS_CLIENTS_MAX;
    for (uint8_t i = 0; i < UI_SETTINGS_CLIENTS_MAX; i++) {
        (void)snprintf(warm.client[i].name, sizeof(warm.client[i].name), "Warm %u", (unsigned)i);
    }
    ui_view_settings_set(&s_set, &warm);
    for (uint8_t i = 0; i < UI_SETTINGS_SEC_COUNT; i++) {
        CHECK(ui_view_settings_show_section(&s_set, (ui_settings_section_t)i));
        lv_test_wait(10);
    }
    CHECK(ui_view_settings_show_section(&s_set, UI_SETTINGS_SEC_REMOTE));
    click_obj(ui_view_settings_toggle(&s_set, UI_SETTING_PAIR_REQUIRE_ACCEPT));
    click_obj(ui_view_settings_confirm_button(&s_set, false));
    lv_test_wait(30);
    stop();

    RUN_MEM(test_unknown_until_published);
    RUN_MEM(test_renders_the_published_record);
    RUN_MEM(test_no_optimistic_toggle);
    RUN_MEM(test_relax_require_accept_needs_confirmation);
    RUN_MEM(test_relax_control_approval);
    RUN_MEM(test_paired_clients);
    RUN_MEM(test_keepawake_interval);
    RUN_MEM(test_unicode_mode_stepper);
    RUN_MEM(test_input_languages);
    RUN_MEM(test_layout_row_is_read_only_without_tables);
    RUN_MEM(test_layout_picker_lists_and_pages);
    RUN_MEM(test_layout_picker_two_levels_and_pick);
    RUN_MEM(test_layout_picker_closes_when_the_database_goes_away);
    RUN_MEM(test_layout_id_the_tables_do_not_have);
    RUN_MEM(test_layout_home_favorites_and_recents);
    RUN_MEM(test_layout_favorites_pin_unpin_and_limit);
    RUN_MEM(test_layout_search_matches_name_and_id);
    RUN_MEM(test_layout_lists_skip_ids_the_tables_lost);
    RUN_MEM(test_layout_index_strip_jumps);
    RUN_MEM(test_pair_entry_point);
    RUN_MEM(test_without_a_sink_nothing_is_offered);
    RUN_MEM(test_about_carries_the_required_notice);
    RUN_MEM(test_navigation_and_close);
    RUN_MEM(test_geometry_of_every_section);
    RUN_MEM(test_lifecycle);
    RUN_MEM(test_refuses_bad_arguments);

    lv_test_indev_delete_all();
    ui_theme_deinit();
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_view_settings");
}
