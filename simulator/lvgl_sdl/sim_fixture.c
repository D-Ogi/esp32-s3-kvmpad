// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// See sim_fixture.h.
#include "sim_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_core.h"
#include "hid_keymap.h"
#include "sim_ja.h"
#include "sim_zh.h"
#include "ui_segmented.h"
#include "ui_topbar.h"

typedef struct {
    const char *name;
    int32_t value;
} name_value_t;

static const name_value_t k_hosts[] = {
    {"uninitialized", KVM_HOST_USB_UNINITIALIZED}, {"detached", KVM_HOST_USB_DETACHED},
    {"enumerating", KVM_HOST_USB_ENUMERATING},     {"mounted", KVM_HOST_USB_MOUNTED},
    {"suspended", KVM_HOST_USB_SUSPENDED},         {"error", KVM_HOST_USB_ERROR},
};

static const name_value_t k_clients[] = {
    {"disabled", UI_CLIENT_DISABLED},       {"network_down", UI_CLIENT_NETWORK_DOWN},
    {"listening", UI_CLIENT_LISTENING},     {"authenticating", UI_CLIENT_AUTHENTICATING},
    {"connected", UI_CLIENT_CONNECTED},     {"control_granted", UI_CLIENT_CONTROL_GRANTED},
    {"stale", UI_CLIENT_STALE},
};

static const name_value_t k_owners[] = {
    {"none", OWNER_NONE},
    {"local", OWNER_LOCAL},
    {"remote", OWNER_REMOTE},
};

static const name_value_t k_views[] = {
    {"mouse", UI_VIEW_MOUSE},
    {"keyboard", UI_VIEW_KEYBOARD},
    {"shortcuts", UI_VIEW_SHORTCUTS},
};

/** Split a space-separated line of layout ids into `slots`, at most `cap` of them. Returns how many. */
static uint8_t sim_id_list(const char *line, char slots[][UI_SETTINGS_LAYOUT_CAP], uint8_t cap)
{
    uint8_t n = 0;
    const char *p = line;
    while (*p != '\0' && n < cap) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t len = 0;
        while (p[len] != '\0' && p[len] != ' ' && p[len] != '\t') {
            len++;
        }
        if (len > 0u && len < UI_SETTINGS_LAYOUT_CAP) {
            memcpy(slots[n], p, len);
            slots[n][len] = '\0';
            n++;
        }
        p += len;
    }
    return n;
}

static const name_value_t k_layouts[] = {
    {"win", KVM_CHORD_LAYOUT_WIN},
    {"ctrl", KVM_CHORD_LAYOUT_CTRL},
};

static const name_value_t k_mouse_btns[] = {
    {"left", 0},
    {"right", 1},
    {"drag", 2},
    {"help", 3},
};

static const name_value_t k_sections[] = {
    {"remote", UI_SETTINGS_SEC_REMOTE},       {"clients", UI_SETTINGS_SEC_CLIENTS},
    {"shortcuts", UI_SETTINGS_SEC_SHORTCUTS}, {"input", UI_SETTINGS_SEC_INPUT},
    {"keyboard", UI_SETTINGS_SEC_KEYBOARD},   {"about", UI_SETTINGS_SEC_ABOUT},
};

static const name_value_t k_approvals[] = {
    {"auto", UI_CONTROL_APPROVAL_AUTO},
    {"trusted", UI_CONTROL_APPROVAL_TRUSTED},
    {"always", UI_CONTROL_APPROVAL_ALWAYS},
};

static const name_value_t k_unicode_modes[] = {
    {"host_ime", UI_UNICODE_MODE_HOST_IME},         {"linux", UI_UNICODE_MODE_LINUX},
    {"macos", UI_UNICODE_MODE_MACOS},               {"windows_hex", UI_UNICODE_MODE_WINDOWS_HEX},
    {"wincompose", UI_UNICODE_MODE_WINCOMPOSE},
};

static const name_value_t k_langs[] = {
    {"en", UI_KBD_LANG_EN},
    {"ko", UI_KBD_LANG_KO},
    {"zh", UI_KBD_LANG_ZH},
    {"ja", UI_KBD_LANG_JA},
};

static const name_value_t k_results[] = {
    {"none", UI_PAIR_RESULT_NONE},         {"paired", UI_PAIR_RESULT_PAIRED},
    {"rejected", UI_PAIR_RESULT_REJECTED}, {"failed", UI_PAIR_RESULT_FAILED},
    {"locked", UI_PAIR_RESULT_LOCKED},     {"expired", UI_PAIR_RESULT_EXPIRED},
};

static bool lookup(const name_value_t *table, size_t n, const char *word, int32_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, word) == 0) {
            *out = table[i].value;
            return true;
        }
    }
    return false;
}

#define LOOKUP(tbl, word, out) lookup((tbl), sizeof(tbl) / sizeof((tbl)[0]), (word), (out))

static bool parse_u32(const char *word, uint32_t *out)
{
    char *end = NULL;
    const int base = (word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) ? 16 : 10;
    const unsigned long v = strtoul(word, &end, base);
    if (!end || *end != '\0') {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_bool(const char *word, bool *out)
{
    uint32_t v = 0;
    if (!parse_u32(word, &v) || v > 1) {
        return false;
    }
    *out = v == 1;
    return true;
}

static void stem_of(const char *path, char *out, size_t cap)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        n = (size_t)(dot - base);
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
}

// ---- step parsing -----------------------------------------------------------------------------------------------

static bool parse_step(const char *rest, sim_step_t *step, const char *path, int line)
{
    char word[64] = {0};
    char arg[SIM_FIX_TEXT_MAX] = {0};
    const int got = sscanf(rest, "%63s %31s", word, arg);
    if (got < 1) {
        printf("  %s:%d: empty step\n", path, line);
        return false;
    }
    memset(step, 0, sizeof(*step));
    int32_t v = 0;
    uint32_t u = 0;
    unsigned fq = 0;
    if (strcmp(word, "tap_nav") == 0 && got == 2 && parse_u32(arg, &u) && u < 3u) {
        step->kind = SIM_STEP_TAP_NAV;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_host") == 0) {
        step->kind = SIM_STEP_TAP_HOST;
    } else if (strcmp(word, "tap_client") == 0) {
        step->kind = SIM_STEP_TAP_CLIENT;
    } else if (strcmp(word, "tap_key") == 0 && got == 2) {
        step->kind = SIM_STEP_TAP_KEY;
        snprintf(step->text, sizeof(step->text), "%s", arg);
    } else if (strcmp(word, "hold_key") == 0 && got == 2) {
        step->kind = SIM_STEP_HOLD_KEY;
        snprintf(step->text, sizeof(step->text), "%s", arg);
    } else if (strcmp(word, "tap_mouse") == 0 && got == 2 && LOOKUP(k_mouse_btns, arg, &v)) {
        step->kind = SIM_STEP_TAP_MOUSE;
        step->arg = v;
    } else if (strcmp(word, "tap_shortcut") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SHORTCUT_COMMAND_COUNT) {
        step->kind = SIM_STEP_TAP_SHORTCUT;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_media") == 0 && got == 2 && parse_u32(arg, &u) && u < UI_SHORTCUT_MEDIA_COUNT) {
        step->kind = SIM_STEP_TAP_MEDIA;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_layout") == 0) {
        step->kind = SIM_STEP_TAP_LAYOUT;
    } else if (strcmp(word, "tap_pair") == 0) {
        step->kind = SIM_STEP_TAP_PAIR;
    } else if (strcmp(word, "tap_settings") == 0) {
        step->kind = SIM_STEP_TAP_SETTINGS;
    } else if (strcmp(word, "tap_section") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_SEC_COUNT) {
        step->kind = SIM_STEP_TAP_SECTION;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_toggle") == 0 && got == 2 && parse_u32(arg, &u) && u < UI_SETTING_COUNT) {
        step->kind = SIM_STEP_TAP_TOGGLE;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_approval") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_CONTROL_APPROVAL_COUNT) {
        step->kind = SIM_STEP_TAP_APPROVAL;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_trusted") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_CLIENTS_MAX) {
        step->kind = SIM_STEP_TAP_TRUSTED;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_forget") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_CLIENTS_MAX) {
        step->kind = SIM_STEP_TAP_FORGET;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_confirm") == 0 && got == 2 && parse_u32(arg, &u) && u <= 1u) {
        step->kind = SIM_STEP_TAP_CONFIRM;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_lang") == 0) {
        step->kind = SIM_STEP_TAP_LANG;
    } else if (strcmp(word, "hold_lang") == 0) {
        step->kind = SIM_STEP_HOLD_LANG;
    } else if (strcmp(word, "pick_lang") == 0 && got == 2 && LOOKUP(k_langs, arg, &v)) {
        step->kind = SIM_STEP_PICK_LANG;
        step->arg = v;
    } else if (strcmp(word, "layout_open") == 0) {
        step->kind = SIM_STEP_TAP_LAYOUT_OPEN;
    } else if (strcmp(word, "layout_back") == 0) {
        step->kind = SIM_STEP_TAP_LAYOUT_BACK;
    } else if (strcmp(word, "layout_page") == 0 && got == 2 &&
               (strcmp(arg, "prev") == 0 || strcmp(arg, "next") == 0)) {
        step->kind = SIM_STEP_TAP_LAYOUT_STEP;
        step->arg = strcmp(arg, "next") == 0 ? 1 : 0;
    } else if (strcmp(word, "layout_all") == 0) {
        step->kind = SIM_STEP_TAP_LAYOUT_ALL;
    } else if (strcmp(word, "layout_search") == 0) {
        step->kind = SIM_STEP_TAP_LAYOUT_SEARCH;
    } else if (strcmp(word, "layout_type") == 0 && got == 2 && strlen(arg) < sizeof(step->text)) {
        step->kind = SIM_STEP_LAYOUT_TYPE;
        snprintf(step->text, sizeof(step->text), "%s", arg);
    } else if (strcmp(word, "hold_quick") == 0) {
        step->kind = SIM_STEP_HOLD_QUICK;
    } else if (strcmp(word, "quick_pick") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_VIEW_KBD_QUICK_MAX) {
        step->kind = SIM_STEP_TAP_QUICK;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "layout_star") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_PICK_ROWS) {
        step->kind = SIM_STEP_TAP_LAYOUT_STAR;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "layout_pick_fav") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_FAV_MAX) {
        step->kind = SIM_STEP_TAP_LAYOUT_FAV;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "layout_pick_recent") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_RECENT_MAX) {
        step->kind = SIM_STEP_TAP_LAYOUT_RECENT;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "layout_index") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_PICK_INDEX) {
        step->kind = SIM_STEP_TAP_LAYOUT_INDEX;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "layout_row") == 0 && got == 2 && parse_u32(arg, &u) &&
               u < UI_SETTINGS_PICK_ROWS) {
        step->kind = SIM_STEP_TAP_LAYOUT_ROW;
        step->arg = (int32_t)u;
    } else if (strcmp(word, "tap_flick_mode") == 0) {
        step->kind = SIM_STEP_TAP_FLICK_MODE;
    } else if (strcmp(word, "flick") == 0 && got == 2 && sscanf(arg, "%u:%u", &u, &fq) == 2 && u < 12u && fq <= 4u) {
        step->kind = SIM_STEP_FLICK;
        step->arg = (int32_t)(u * 8u + fq);
    } else if (strcmp(word, "wait") == 0 && got == 2 && parse_u32(arg, &u) && u <= 2000u) {
        step->kind = SIM_STEP_WAIT;
        step->arg = (int32_t)u;
    } else {
        printf("  %s:%d: bad step '%s'\n", path, line, rest);
        return false;
    }
    return true;
}

// ---- file parsing -----------------------------------------------------------------------------------------------

bool sim_fixture_load(const char *path, sim_fixture_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  cannot open fixture %s\n", path);
        return false;
    }
    memset(out, 0, sizeof(*out));
    stem_of(path, out->name, sizeof(out->name));
    snprintf(out->title, sizeof(out->title), "%s", out->name);
    out->view = UI_VIEW_MOUSE;
    out->layout_chord = KVM_CHORD_LAYOUT_WIN;
    // FR-722 defaults: the code is good for 120 s inside a 180 s window, a decision for 30 s.
    out->pair.code_remaining_ms = 120000u;
    out->pair.window_remaining_ms = 180000u;
    out->pair.accept_remaining_ms = 30000u;
    out->approval.remaining_ms = 30000u;
    // ADR-023 safe defaults, as config_store hands them to the adapter on a factory-fresh device. The
    // version string is a constant, so a golden never depends on the build.
    out->settings.seq = 1u;
    out->settings.pair_require_accept = true;
    out->settings.control_approval = UI_CONTROL_APPROVAL_ALWAYS;
    out->settings.keepawake_interval_s = 30u;
    out->settings.unicode_mode = UI_UNICODE_MODE_HOST_IME;
    snprintf(out->settings.host_layout, sizeof(out->settings.host_layout), "us");
    out->settings.layout_db_available = true; // this build links the generated tables; `layout_db 0` drops them
    snprintf(out->settings.firmware_version, sizeof(out->settings.firmware_version), "1.1.0 (simulator)");
    out->st = (sim_state_t){
        .host_usb_state = KVM_HOST_USB_MOUNTED,
        .client_state = UI_CLIENT_LISTENING,
        .owner_kind = OWNER_LOCAL,
        .owner_epoch = 1u,
        .caps_led_known = true,
        .wifi_connected = true,
        .touch_healthy = true,
    };

    char buf[512];
    int line = 0;
    bool ok = true;
    while (ok && fgets(buf, sizeof(buf), f)) {
        line++;
        char *hash = strchr(buf, '#');
        if (hash) {
            *hash = '\0';
        }
        char key[32] = {0};
        char rest[256] = {0};
        const int got = sscanf(buf, "%31s %255[^\n\r]", key, rest);
        if (got < 1) {
            continue;
        }
        // trim the trailing blanks of `rest`
        for (size_t i = strlen(rest); i > 0 && (rest[i - 1] == ' ' || rest[i - 1] == '\t'); i--) {
            rest[i - 1] = '\0';
        }
        int32_t v = 0;
        uint32_t u = 0;
        bool b = false;
        if (strcmp(key, "title") == 0 && got == 2) {
            snprintf(out->title, sizeof(out->title), "%s", rest);
        } else if (strcmp(key, "view") == 0 && LOOKUP(k_views, rest, &v)) {
            out->view = (ui_view_id_t)v;
        } else if (strcmp(key, "host") == 0 && LOOKUP(k_hosts, rest, &v)) {
            out->st.host_usb_state = (uint8_t)v;
        } else if (strcmp(key, "client") == 0 && LOOKUP(k_clients, rest, &v)) {
            out->st.client_state = (uint8_t)v;
        } else if (strcmp(key, "owner") == 0 && LOOKUP(k_owners, rest, &v)) {
            out->st.owner_kind = (uint8_t)v;
        } else if (strcmp(key, "epoch") == 0 && parse_u32(rest, &u)) {
            out->st.owner_epoch = u;
        } else if (strcmp(key, "wifi") == 0 && parse_bool(rest, &b)) {
            out->st.wifi_connected = b;
        } else if (strcmp(key, "touch") == 0 && parse_bool(rest, &b)) {
            out->st.touch_healthy = b;
        } else if (strcmp(key, "drag") == 0 && parse_bool(rest, &b)) {
            out->st.drag_lock_active = b;
        } else if (strcmp(key, "mods") == 0 && parse_u32(rest, &u) && u <= 0xFFu) {
            out->st.modifiers_down = (uint8_t)u;
        } else if (strcmp(key, "armed") == 0 && parse_u32(rest, &u) && u <= 0xFFu) {
            out->st.mods_armed = (uint8_t)u;
        } else if (strcmp(key, "locked") == 0 && parse_u32(rest, &u) && u <= 0xFFu) {
            out->st.mods_locked = (uint8_t)u;
        } else if (strcmp(key, "caps") == 0 && strcmp(rest, "unknown") == 0) {
            out->st.caps_led_known = false;
            out->st.caps_led_active = false;
        } else if (strcmp(key, "caps") == 0 && (strcmp(rest, "on") == 0 || strcmp(rest, "off") == 0)) {
            out->st.caps_led_known = true;
            out->st.caps_led_active = strcmp(rest, "on") == 0;
        } else if (strcmp(key, "maint") == 0 && parse_bool(rest, &b)) {
            out->st.maintenance = b;
        } else if (strcmp(key, "error") == 0 && parse_u32(rest, &u) && u <= 0xFFFFu) {
            out->st.error_code = (uint16_t)u;
        } else if (strcmp(key, "confirm") == 0 && parse_bool(rest, &b)) {
            out->confirm_alt_f4 = b;
        } else if (strcmp(key, "layout") == 0 && LOOKUP(k_layouts, rest, &v)) {
            out->layout_chord = (uint16_t)v;
        } else if (strcmp(key, "pair") == 0 && parse_bool(rest, &b)) {
            out->pair.window_open = b;
        } else if (strcmp(key, "code") == 0 && got == 2 && strlen(rest) < sizeof(out->pair.code)) {
            snprintf(out->pair.code, sizeof(out->pair.code), "%s", rest);
        } else if (strcmp(key, "code_ms") == 0 && parse_u32(rest, &u)) {
            out->pair.code_remaining_ms = u;
        } else if (strcmp(key, "win_ms") == 0 && parse_u32(rest, &u)) {
            out->pair.window_remaining_ms = u;
        } else if (strcmp(key, "accept") == 0 && parse_bool(rest, &b)) {
            out->pair.accept_pending = b;
        } else if (strcmp(key, "accept_ms") == 0 && parse_u32(rest, &u)) {
            out->pair.accept_remaining_ms = u;
        } else if (strcmp(key, "peer") == 0 && got == 2) {
            // The one name of the run: the prompts and the outcome toast all speak about the same client.
            snprintf(out->pair.client_name, sizeof(out->pair.client_name), "%s", rest);
            snprintf(out->pair.result_name, sizeof(out->pair.result_name), "%s", rest);
            snprintf(out->approval.client_name, sizeof(out->approval.client_name), "%s", rest);
        } else if (strcmp(key, "result") == 0 && LOOKUP(k_results, rest, &v)) {
            out->pair.result = (ui_pair_result_t)v;
            out->pair.result_seq = (v == UI_PAIR_RESULT_NONE) ? 0u : 1u;
        } else if (strcmp(key, "approval") == 0 && parse_bool(rest, &b)) {
            out->approval.pending = b;
        } else if (strcmp(key, "appr_ms") == 0 && parse_u32(rest, &u)) {
            out->approval.remaining_ms = u;
        } else if (strcmp(key, "session") == 0 && parse_u32(rest, &u)) {
            out->approval.session_id = u;
        } else if (strcmp(key, "settings") == 0 && parse_bool(rest, &b)) {
            out->settings_open = b;
        } else if (strcmp(key, "zh") == 0 && parse_bool(rest, &b)) {
            out->zh_engine = b;
        } else if (strcmp(key, "ja") == 0 && parse_bool(rest, &b)) {
            out->ja_engine = b;
        } else if (strcmp(key, "section") == 0 && LOOKUP(k_sections, rest, &v)) {
            out->section = (ui_settings_section_t)v;
        } else if (strcmp(key, "cfg_seq") == 0 && parse_u32(rest, &u)) {
            out->settings.seq = u;
        } else if (strcmp(key, "remote") == 0 && parse_bool(rest, &b)) {
            out->settings.remote_enabled = b;
        } else if (strcmp(key, "req_accept") == 0 && parse_bool(rest, &b)) {
            out->settings.pair_require_accept = b;
        } else if (strcmp(key, "approval_policy") == 0 && LOOKUP(k_approvals, rest, &v)) {
            out->settings.control_approval = (uint8_t)v;
        } else if (strcmp(key, "altf4") == 0 && parse_bool(rest, &b)) {
            out->settings.confirm_alt_f4 = b;
        } else if (strcmp(key, "awake") == 0 && parse_bool(rest, &b)) {
            out->settings.keepawake_enabled = b;
        } else if (strcmp(key, "awake_s") == 0 && parse_u32(rest, &u) && u <= 0xFFFFu) {
            out->settings.keepawake_interval_s = (uint16_t)u;
        } else if (strcmp(key, "layout_id") == 0 && got == 2 &&
                   strlen(rest) < sizeof(out->settings.host_layout)) {
            snprintf(out->settings.host_layout, sizeof(out->settings.host_layout), "%s", rest);
        } else if (strcmp(key, "layout_fav") == 0 && got == 2) {
            out->settings.host_fav_count = sim_id_list(rest, out->settings.host_fav,
                                                       UI_SETTINGS_FAV_MAX);
        } else if (strcmp(key, "layout_recent") == 0 && got == 2) {
            out->settings.host_recent_count = sim_id_list(rest, out->settings.host_recent,
                                                          UI_SETTINGS_RECENT_MAX);
        } else if (strcmp(key, "layout_db") == 0 && parse_bool(rest, &b)) {
            out->settings.layout_db_available = b; // FR-741: the domain reports whether the tables are there
        } else if (strcmp(key, "unicode") == 0 && LOOKUP(k_unicode_modes, rest, &v)) {
            out->settings.unicode_mode = (uint8_t)v;
        } else if (strcmp(key, "lang_ko") == 0 && parse_bool(rest, &b)) {
            out->settings.lang_ko = b;
        } else if (strcmp(key, "lang_zh") == 0 && parse_bool(rest, &b)) {
            out->settings.lang_zh = b;
        } else if (strcmp(key, "lang_ja") == 0 && parse_bool(rest, &b)) {
            out->settings.lang_ja = b;
        } else if (strcmp(key, "fw") == 0 && got == 2) {
            snprintf(out->settings.firmware_version, sizeof(out->settings.firmware_version), "%s", rest);
        } else if ((strcmp(key, "client") == 0 || strcmp(key, "tclient") == 0) && got == 2) {
            if (out->settings.client_count >= UI_SETTINGS_CLIENTS_MAX) {
                printf("  %s:%d: more than %d paired clients\n", path, line, UI_SETTINGS_CLIENTS_MAX);
                ok = false;
            } else {
                ui_settings_client_t *c = &out->settings.client[out->settings.client_count++];
                snprintf(c->name, sizeof(c->name), "%s", rest);
                c->trusted = strcmp(key, "tclient") == 0;
            }
        } else if (strcmp(key, "step") == 0 && got == 2) {
            if (out->n_steps >= SIM_FIX_STEPS_MAX) {
                printf("  %s:%d: more than %d steps\n", path, line, SIM_FIX_STEPS_MAX);
                ok = false;
            } else {
                ok = parse_step(rest, &out->steps[out->n_steps], path, line);
                out->n_steps = (uint8_t)(out->n_steps + (ok ? 1 : 0));
            }
        } else {
            printf("  %s:%d: unknown directive '%s %s'\n", path, line, key, rest);
            ok = false;
        }
    }
    fclose(f);
    return ok;
}

// ---- replay -----------------------------------------------------------------------------------------------------

lv_obj_t *sim_keyboard_key(const sim_app_t *s, const char *legend)
{
    const uint32_t n = ui_view_keyboard_key_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = ui_view_keyboard_key_name(i);
        if (name && strcmp(name, legend) == 0) {
            lv_obj_t *obj = ui_view_keyboard_key_obj(&s->app.keyboard, i);
            if (obj) {
                return obj;
            }
        }
    }
    return NULL;
}

static int32_t nav_index_of(const sim_app_t *s, ui_view_id_t view)
{
    for (uint16_t i = 0; i < s->app.nav_desc.count; i++) {
        if (s->app.segment_view[i] == view) {
            return (int32_t)i;
        }
    }
    return -1;
}

static bool run_step(sim_app_t *s, const sim_step_t *step)
{
    lv_obj_t *target = NULL;
    switch (step->kind) {
    case SIM_STEP_TAP_NAV:
        target = ui_segmented_tab(s->app.nav, (uint16_t)step->arg);
        break;
    case SIM_STEP_TAP_HOST:
        target = ui_topbar_status_item(s->app.bar, UI_TOPBAR_HOST);
        break;
    case SIM_STEP_TAP_CLIENT:
        target = ui_topbar_status_item(s->app.bar, UI_TOPBAR_CLIENT);
        break;
    case SIM_STEP_TAP_KEY:
    case SIM_STEP_HOLD_KEY:
        target = sim_keyboard_key(s, step->text);
        break;
    case SIM_STEP_TAP_MOUSE:
        target = step->arg == 0   ? s->app.mouse.btn_left
                 : step->arg == 1 ? s->app.mouse.btn_right
                 : step->arg == 2 ? s->app.mouse.btn_drag
                                  : s->app.mouse.btn_help;
        break;
    case SIM_STEP_TAP_SHORTCUT:
        target = s->app.shortcuts.card[step->arg];
        break;
    case SIM_STEP_TAP_MEDIA:
        target = s->app.shortcuts.media[step->arg];
        break;
    case SIM_STEP_TAP_LAYOUT:
        target = s->app.shortcuts.layout_key;
        break;
    case SIM_STEP_TAP_PAIR:
        target = ui_view_status_pair_button(&s->app.status);
        break;
    case SIM_STEP_TAP_SETTINGS:
        target = s->app.settings_btn;
        break;
    case SIM_STEP_TAP_SECTION:
        target = s->app.settings.tab[step->arg];
        break;
    case SIM_STEP_TAP_TOGGLE:
        target = ui_view_settings_toggle(&s->app.settings, (ui_setting_id_t)step->arg);
        break;
    case SIM_STEP_TAP_APPROVAL:
        target = ui_view_settings_approval_button(&s->app.settings, (ui_control_approval_t)step->arg);
        break;
    case SIM_STEP_TAP_TRUSTED:
        target = ui_view_settings_client_trusted_button(&s->app.settings, (uint8_t)step->arg);
        break;
    case SIM_STEP_TAP_FORGET:
        target = ui_view_settings_client_forget_button(&s->app.settings, (uint8_t)step->arg);
        break;
    case SIM_STEP_TAP_CONFIRM:
        target = ui_view_settings_confirm_button(&s->app.settings, step->arg == 1);
        break;
    case SIM_STEP_TAP_LANG:
        target = ui_view_keyboard_lang_key(&s->app.keyboard);
        break;
    case SIM_STEP_HOLD_LANG:
        target = ui_view_keyboard_lang_key(&s->app.keyboard);
        if (target == NULL) {
            printf("    step %d: target not available\n", (int)step->kind);
            return false;
        }
        sim_long_press_obj(target);
        return true;
    case SIM_STEP_HOLD_QUICK:
        target = ui_view_keyboard_layout_strip(&s->app.keyboard);
        if (target == NULL) {
            printf("    step %d: target not available\n", (int)step->kind);
            return false;
        }
        sim_long_press_obj(target);
        return true;
    case SIM_STEP_TAP_QUICK:
        target = ui_view_keyboard_layout_pick_button(&s->app.keyboard, (uint8_t)step->arg);
        break;
    case SIM_STEP_PICK_LANG:
        target = ui_view_keyboard_lang_pick_button(&s->app.keyboard, (ui_kbd_lang_t)step->arg);
        break;
    case SIM_STEP_TAP_LAYOUT_OPEN:
        target = ui_view_settings_layout_button(&s->app.settings);
        break;
    case SIM_STEP_TAP_LAYOUT_BACK:
        target = ui_view_settings_picker_back(&s->app.settings);
        break;
    case SIM_STEP_TAP_LAYOUT_STEP:
        target = ui_view_settings_picker_step(&s->app.settings, step->arg == 1);
        break;
    case SIM_STEP_TAP_LAYOUT_ROW:
        target = ui_view_settings_picker_row(&s->app.settings, (uint8_t)step->arg);
        break;
    case SIM_STEP_TAP_LAYOUT_ALL:
        target = ui_view_settings_picker_all_button(&s->app.settings);
        break;
    case SIM_STEP_TAP_LAYOUT_SEARCH:
        target = ui_view_settings_picker_search_button(&s->app.settings);
        break;
    case SIM_STEP_LAYOUT_TYPE:
        for (const char *ch = step->text; *ch != '\0'; ch++) {
            lv_obj_t *key = NULL;
            for (uint8_t i = 0; i < UI_SETTINGS_QUERY_KEYS && key == NULL; i++) {
                if (ui_view_settings_picker_key_char(&s->app.settings, i) == *ch) {
                    key = ui_view_settings_picker_key(&s->app.settings, i);
                }
            }
            if (key == NULL) {
                printf("    layout_type: no key for '%c'\n", *ch);
                return false;
            }
            sim_click_obj(key);
        }
        return true;
    case SIM_STEP_TAP_LAYOUT_STAR:
        target = ui_view_settings_picker_star(&s->app.settings, (uint8_t)step->arg);
        break;
    case SIM_STEP_TAP_LAYOUT_FAV:
        target = ui_view_settings_picker_fav(&s->app.settings, (uint8_t)step->arg);
        break;
    case SIM_STEP_TAP_LAYOUT_RECENT:
        target = ui_view_settings_picker_recent(&s->app.settings, (uint8_t)step->arg);
        break;
    case SIM_STEP_TAP_LAYOUT_INDEX:
        target = ui_view_settings_picker_index(&s->app.settings, (uint8_t)step->arg);
        break;
    case SIM_STEP_TAP_FLICK_MODE:
        target = s->app.keyboard.flick_toggle;
        break;
    case SIM_STEP_FLICK: {
        const uint32_t q = (uint32_t)step->arg / 8u;
        target = s->app.keyboard.flick_key[(q / 3u) * 4u + q % 3u];
        if (target != NULL) {
            sim_flick_obj(target, (uint8_t)(step->arg % 8));
        }
        return target != NULL;
    }
    case SIM_STEP_WAIT:
        lv_test_wait((uint32_t)step->arg);
        return true;
    }
    if (!target) {
        printf("    step %d: target not available\n", (int)step->kind);
        return false;
    }
    if (step->kind == SIM_STEP_HOLD_KEY) {
        sim_press_obj(target);
    } else {
        sim_click_obj(target);
    }
    return true;
}

bool sim_fixture_apply(sim_app_t *s, const sim_fixture_t *fx)
{
    sim_publish(s, &fx->st);
    if (fx->view != UI_VIEW_MOUSE) {
        const int32_t idx = nav_index_of(s, fx->view);
        if (idx < 0) {
            printf("    view %d has no mode switcher tab\n", (int)fx->view);
            return false;
        }
        // Navigate the way a finger does: through the tab, the action gateway and the router.
        sim_click_obj(ui_segmented_tab(s->app.nav, (uint16_t)idx));
        if (ui_router_active(&s->app.router) != (int)fx->view) {
            printf("    navigation to view %d refused\n", (int)fx->view);
            return false;
        }
    }
    // The pairing and approval state reaches the shell the way secure_remote publishes it, before the taps.
    ui_app_set_pairing(&s->app, &fx->pair);
    ui_app_set_approval(&s->app, &fx->approval);
    // The settings record reaches it the way the config_store adapter publishes it. The card is opened
    // through the gear and the section through its tab, so both go through the real hit test.
    ui_app_set_settings(&s->app, &fx->settings);
    if (fx->zh_engine) {
        ui_app_set_zh_engine(&s->app, sim_zh_engine()); // as ime_glue hands the decoder over on the device
    }
    if (fx->ja_engine) {
        ui_app_set_ja_engine(&s->app, sim_ja_engine());
    }
    lv_test_wait(10);
    if (fx->settings_open) {
        sim_click_obj(s->app.settings_btn);
        lv_test_wait(10);
        if (ui_view_settings_card(&s->app.settings, UI_OVERLAY_SETTINGS) == NULL) {
            printf("    the Settings card refused to open\n");
            return false;
        }
        if (fx->section != UI_SETTINGS_SEC_REMOTE) {
            sim_click_obj(s->app.settings.tab[fx->section]);
            lv_test_wait(10);
            if (ui_view_settings_section(&s->app.settings) != fx->section) {
                printf("    section %d did not open\n", (int)fx->section);
                return false;
            }
        }
    }
    for (uint8_t i = 0; i < fx->n_steps; i++) {
        if (!run_step(s, &fx->steps[i])) {
            return false;
        }
    }
    sim_settle();
    return true;
}
