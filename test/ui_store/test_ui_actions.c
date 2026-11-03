// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-060B: typed UI action gateway — value ranges, local/domain routing, refusals, request ids,
// and the status wording (unknown never upgraded).
#include <string.h>

#include "kvm_test.h"
#include "ui_actions.h"
#include "ui_present.h"

typedef struct {
    int calls;
    bool accept;
    ui_action_t last;
} sink_t;

static bool sink(void *ctx, const ui_action_t *a)
{
    sink_t *s = ctx;
    s->calls++;
    s->last = *a;
    return s->accept;
}

static void test_ranges(void)
{
    CHECK(ui_action_valid(UI_ACT_NAVIGATE, UI_VIEW_MOUSE));
    CHECK(ui_action_valid(UI_ACT_NAVIGATE, UI_VIEW_SETTINGS));
    CHECK(!ui_action_valid(UI_ACT_NAVIGATE, UI_VIEW_COUNT));
    CHECK(ui_action_valid(UI_ACT_DISMISS_OVERLAY, 0));
    CHECK(!ui_action_valid(UI_ACT_OPEN_SETTINGS, 1));
    CHECK(!ui_action_valid(UI_ACT_KEY_DOWN, 0x00));
    CHECK(!ui_action_valid(UI_ACT_KEY_DOWN, 0x03));
    CHECK(ui_action_valid(UI_ACT_KEY_DOWN, 0x04));
    CHECK(ui_action_valid(UI_ACT_KEY_UP, 0xE7));
    CHECK(!ui_action_valid(UI_ACT_KEY_UP, 0xE8));
    CHECK(!ui_action_valid(UI_ACT_KEY_DOWN, 0x1FF));
    CHECK(!ui_action_valid(UI_ACT_SHORTCUT_REQUEST, KVM_CHORD_NONE));
    CHECK(ui_action_valid(UI_ACT_SHORTCUT_REQUEST, KVM_CHORD_CTRL_ALT_DEL));
    CHECK(ui_action_valid(UI_ACT_SHORTCUT_REQUEST, KVM_CHORD_COUNT - 1));
    CHECK(!ui_action_valid(UI_ACT_SHORTCUT_REQUEST, KVM_CHORD_COUNT));
    CHECK(!ui_action_valid(UI_ACT_MOUSE_BUTTON_DOWN, 0));
    CHECK(ui_action_valid(UI_ACT_MOUSE_BUTTON_DOWN, KVM_MOUSE_BTN_LEFT));
    CHECK(ui_action_valid(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_MIDDLE));
    CHECK(!ui_action_valid(UI_ACT_MOUSE_BUTTON_UP, 4));
    CHECK(!ui_action_valid(UI_ACT_MODIFIER_TOGGLE, 0xDF));
    CHECK(ui_action_valid(UI_ACT_MODIFIER_TOGGLE, 0xE0));
    CHECK(ui_action_valid(UI_ACT_MODIFIER_TOGGLE, 0xE7));
    CHECK(!ui_action_valid(UI_ACT_MODIFIER_TOGGLE, 0xE8));
    CHECK(ui_action_valid(UI_ACT_DRAG_LOCK_REQUEST, 0));
    CHECK(ui_action_valid(UI_ACT_DRAG_LOCK_REQUEST, 1));
    CHECK(!ui_action_valid(UI_ACT_DRAG_LOCK_REQUEST, 2));
    CHECK(!ui_action_valid(UI_ACT_COUNT, 0));
    // ADR-024 text actions
    CHECK(ui_action_valid(UI_ACT_TEXT_CHAR, 0x20));
    CHECK(ui_action_valid(UI_ACT_TEXT_CHAR, 0xD55C));
    CHECK(ui_action_valid(UI_ACT_TEXT_CHAR, 0xFFFF));
    CHECK(!ui_action_valid(UI_ACT_TEXT_CHAR, 0x1F));
    CHECK(!ui_action_valid(UI_ACT_TEXT_CHAR, 0x7F));
    CHECK(!ui_action_valid(UI_ACT_TEXT_CHAR, 0x85));
    CHECK(!ui_action_valid(UI_ACT_TEXT_CHAR, 0xD800));
    CHECK(!ui_action_valid(UI_ACT_TEXT_CHAR, 0xDFFF));
    CHECK(ui_action_valid(UI_ACT_TEXT_KEY, 0x2A));
    CHECK(ui_action_valid(UI_ACT_TEXT_KEY, 0x1E | UI_TEXT_KEY_SHIFT));
    CHECK(!ui_action_valid(UI_ACT_TEXT_KEY, 0x03));
    CHECK(!ui_action_valid(UI_ACT_TEXT_KEY, 0xE1)); // a modifier is never a text key
    CHECK(!ui_action_valid(UI_ACT_TEXT_KEY, 0x2A | 0x0200));
    CHECK(ui_action_valid(UI_ACT_TEXT_CANCEL, 0));
    CHECK(!ui_action_valid(UI_ACT_TEXT_CANCEL, 1));
    CHECK(!ui_action_valid((ui_action_type_t)99, 0));
}

static void test_routing(void)
{
    ui_actions_t a;
    ui_actions_init(&a);
    sink_t local = {.accept = true};
    sink_t domain = {.accept = true};
    ui_actions_set_local_sink(&a, sink, &local);
    ui_actions_set_domain_sink(&a, sink, &domain);
    for (int t = 0; t < UI_ACT_COUNT; t++) {
        uint16_t v = 0;
        switch ((ui_action_type_t)t) {
        case UI_ACT_KEY_DOWN: case UI_ACT_KEY_UP: v = 0x04; break;
        case UI_ACT_SHORTCUT_REQUEST: v = KVM_CHORD_ALT_TAB; break;
        case UI_ACT_MOUSE_BUTTON_DOWN: case UI_ACT_MOUSE_BUTTON_UP: v = KVM_MOUSE_BTN_RIGHT; break;
        case UI_ACT_MODIFIER_TOGGLE: v = 0xE1; break;
        case UI_ACT_TEXT_CHAR: v = 0xD55C; break;
        case UI_ACT_TEXT_KEY: v = 0x2C; break;
        default: break;
        }
        int lb = local.calls;
        int db = domain.calls;
        uint32_t rid = ui_actions_submit(&a, (ui_action_type_t)t, (uint16_t)(100 + t), v, 1000u + (uint64_t)t, NULL);
        CHECK(rid != 0);
        if (ui_action_is_local((ui_action_type_t)t)) {
            CHECK_EQ(local.calls, lb + 1);
            CHECK_EQ(domain.calls, db);
            CHECK_EQ(local.last.request_id, rid);
        } else {
            CHECK_EQ(domain.calls, db + 1);
            CHECK_EQ(local.calls, lb);
            CHECK_EQ(domain.last.type, t);
            CHECK_EQ(domain.last.value, v);
            CHECK_EQ(domain.last.control_id, 100 + t);
            CHECK_EQ(domain.last.monotonic_us, 1000u + (uint64_t)t);
        }
    }
    CHECK_EQ(local.calls, 5);
    CHECK_EQ(domain.calls, UI_ACT_COUNT - 5);
    CHECK_EQ(a.c.local, 5);
    CHECK_EQ(a.c.domain, UI_ACT_COUNT - 5);
}

static void test_refusals(void)
{
    ui_actions_t a;
    ui_actions_init(&a);
    sink_t domain = {.accept = true};
    // no sink: refused, never "assumed delivered"
    ui_action_t out;
    CHECK_EQ(ui_actions_submit(&a, UI_ACT_KEY_DOWN, 1, 0x04, 0, &out), 0);
    CHECK_EQ(a.c.rejected_no_sink, 1);
    ui_actions_set_domain_sink(&a, sink, &domain);
    // invalid: the sink is never called
    CHECK_EQ(ui_actions_submit(&a, UI_ACT_KEY_DOWN, 1, 0xF0, 0, &out), 0);
    CHECK_EQ(ui_actions_submit(&a, UI_ACT_SHORTCUT_REQUEST, 1, 0, 0, &out), 0);
    CHECK_EQ(ui_actions_submit(&a, (ui_action_type_t)200, 1, 0, 0, &out), 0);
    CHECK_EQ(domain.calls, 0);
    CHECK_EQ(a.c.rejected_invalid, 3);
    CHECK_EQ(out.request_id, 0); // invalid actions get no id
    // sink refuses (queue full)
    domain.accept = false;
    CHECK_EQ(ui_actions_submit(&a, UI_ACT_KEY_UP, 1, 0x04, 0, &out), 0);
    CHECK_EQ(domain.calls, 1);
    CHECK_EQ(a.c.rejected_by_sink, 1);
    CHECK(out.request_id != 0); // the id was used for the attempt, so a later result cannot alias it
    CHECK_EQ(a.c.submitted, 5);
}

static void test_request_ids(void)
{
    ui_actions_t a;
    ui_actions_init(&a);
    sink_t s = {.accept = true};
    ui_actions_set_domain_sink(&a, sink, &s);
    uint32_t prev = ui_actions_submit(&a, UI_ACT_KEY_DOWN, 1, 0x04, 0, NULL);
    for (int i = 0; i < 1000; i++) {
        uint32_t id = ui_actions_submit(&a, UI_ACT_KEY_UP, 1, 0x04, 0, NULL);
        CHECK(id == prev + 1);
        prev = id;
    }
    a.next_request_id = 0xFFFFFFFFu; // wrap never yields 0
    CHECK_EQ(ui_actions_submit(&a, UI_ACT_KEY_DOWN, 1, 0x04, 0, NULL), 0xFFFFFFFFu);
    CHECK_EQ(ui_actions_submit(&a, UI_ACT_KEY_DOWN, 1, 0x04, 0, NULL), 1);
}

static void test_names_and_wording(void)
{
    CHECK(strcmp(ui_action_name(UI_ACT_NAVIGATE), "NAVIGATE") == 0);
    CHECK(strcmp(ui_action_name(UI_ACT_DRAG_LOCK_REQUEST), "DRAG_LOCK_REQUEST") == 0);
    CHECK(strcmp(ui_action_name(UI_ACT_COUNT), "?") == 0);
    // 01 §6.1 / §6.2 view column; unknown / out of range never upgraded
    CHECK(strcmp(ui_present_host(KVM_HOST_USB_MOUNTED), "Active") == 0);
    CHECK(strcmp(ui_present_host(KVM_HOST_USB_DETACHED), "Disconnected") == 0);
    CHECK(strcmp(ui_present_host(KVM_HOST_USB_UNINITIALIZED), "Initializing") == 0);
    CHECK(strcmp(ui_present_host(-1), "Unknown") == 0);
    CHECK(strcmp(ui_present_host(6), "Unknown") == 0);
    CHECK(strcmp(ui_present_client(UI_CLIENT_CONNECTED), "Connected") == 0);
    CHECK(strcmp(ui_present_client(UI_CLIENT_CONTROL_GRANTED), "Controlling") == 0);
    CHECK(strcmp(ui_present_client(UI_CLIENT_DISABLED), "Off") == 0);
    CHECK(strcmp(ui_present_client(-1), "Unknown") == 0);
    CHECK(strcmp(ui_present_client(UI_CLIENT_COUNT), "Unknown") == 0);
    CHECK(strcmp(ui_present_owner(OWNER_REMOTE), "Remote") == 0);
    CHECK(strcmp(ui_present_owner(-1), "Unknown") == 0);
}

int main(void)
{
    printf("test_ui_actions\n");
    RUN(test_ranges);
    RUN(test_routing);
    RUN(test_refusals);
    RUN(test_request_ids);
    RUN(test_names_and_wording);
    TEST_MAIN_END("test_ui_actions");
}
