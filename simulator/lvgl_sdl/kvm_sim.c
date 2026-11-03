// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// kvm_sim - desktop simulator of the KVMpad UI (TASK-070D, 07 §10 level L1).
//
// It builds the real product shell - the same ui_app_init() composition as main/ui_shell.c - on the real
// LVGL 9.6, drives it from fixture files through the real domain publishers and the real LVGL hit test, renders
// every fixture headless at 1024x600 (the ADR-026 panel geometry, RGB565 like the panel), compares the result
// with the committed golden PNG and runs the AT-UIFW acceptance checks of 07 §10.
//
// It is software evidence only: there is no panel, no GT911 and no USB HOST here, so no hardware claim may be
// derived from a PASS (AGENTS-UI §12). Nothing in this program can send a HID report: the domain sink is a
// recorder and no TinyUSB header is reachable from the UI layers it links.
//
//   kvm_sim                         render, compare, check; exit 1 on the first failure class
//   kvm_sim --update-golden         regenerate the golden set instead of comparing
//   kvm_sim --only keyboard_fn      one fixture
//   kvm_sim --hanzi <blob>          the hanzi font blob of ime_data, so the ZH fixtures compare their golden
//   kvm_sim --kanji <blob>          the kanji font blob of ime_data, the same for the JA fixtures
//   kvm_sim --list                  print the fixtures and exit
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>

#include "host_lv_mem.h"
#include "sim_checks.h"
#include "sim_fixture.h"
#include "sim_image.h"
#include "sim_platform.h"
#include "ui_font_blob.h"
#include "ui_modal.h"
#include "ui_theme.h"
#include "ui_tokens.h"
#include "ui_topbar.h"
#include "ui_view_settings.h"
#include "ui_view_status.h"

#ifndef KVM_SIM_FIXTURE_DIR
#define KVM_SIM_FIXTURE_DIR "."
#endif
#ifndef KVM_SIM_GOLDEN_DIR
#define KVM_SIM_GOLDEN_DIR "."
#endif
#ifndef KVM_SIM_OUT_DIR
#define KVM_SIM_OUT_DIR "."
#endif

#define FIXTURES_MAX 64

typedef enum {
    GOLD_MATCH = 0,
    GOLD_MISMATCH,
    GOLD_MISSING,
    GOLD_WRITTEN,
    GOLD_SKIPPED,
} gold_status_t;

typedef struct {
    char name[SIM_FIX_NAME_MAX];
    char title[SIM_FIX_TITLE_MAX];
    bool built;
    bool geometry_ok;
    bool hidden_ok;
    gold_status_t gold;
    sim_diff_t diff;
    sim_check_result_t checks;
    size_t alloc_delta_nonzero;
} row_t;

typedef struct {
    const char *fixture_dir;
    const char *golden_dir;
    const char *out_dir;
    const char *only;
    const char *hanzi; // KVMFONT1 blob of the hanzi font (ime_data), for the ZH fixtures
    const char *kanji; // KVMFONT1 blob of the kanji font (ime_data), for the JA fixtures
    bool update_golden;
    bool list_only;
    uint8_t tolerance;
    uint32_t budget;
    uint32_t switches;
    uint32_t modal_cycles;
} opts_t;

static char s_names[FIXTURES_MAX][SIM_FIX_NAME_MAX];
static int s_name_count;
static row_t s_rows[FIXTURES_MAX];
static int s_row_count;
typedef struct {
    const char *what;
    ui_font_blob_t font;
    void *blob;
    bool ok;
} cjk_font_t;

static cjk_font_t s_hanzi = {.what = "hanzi"};
static cjk_font_t s_kanji = {.what = "kanji"};

// ---- fixture discovery -------------------------------------------------------------------------------------------

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/** Every `*.fx` of `dir`, in byte order, so a run is reproducible whatever the file system returns. */
static bool collect_fixtures(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        printf("cannot open fixture directory %s\n", dir);
        return false;
    }
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n < 4u || strcmp(e->d_name + n - 3, ".fx") != 0) {
            continue;
        }
        if (s_name_count >= FIXTURES_MAX) {
            printf("more than %d fixtures in %s\n", FIXTURES_MAX, dir);
            closedir(d);
            return false;
        }
        snprintf(s_names[s_name_count], SIM_FIX_NAME_MAX, "%.*s", (int)(n - 3), e->d_name);
        s_name_count++;
    }
    closedir(d);
    qsort(s_names, (size_t)s_name_count, SIM_FIX_NAME_MAX, cmp_name);
    return s_name_count > 0;
}

// ---- warm-up -----------------------------------------------------------------------------------------------------

/**
 * One shell that visits every state the fixtures use, so the long-lived renderer state exists before the
 * baseline is taken. LVGL caches glyph draw data and style property arrays on first use and keeps them: without
 * this tour the first fixture that renders a new symbol would look like a leak. After it, every fixture must
 * come back to the same live allocation count (AT-UIFW-09).
 */
static bool warm_up(void)
{
    sim_app_t sim;
    memset(&sim, 0, sizeof(sim));
    sim.confirm_alt_f4 = true;
    sim.layout_chord = 7u; // KVM_CHORD_LAYOUT_WIN
    if (!sim_app_start(&sim)) {
        return false;
    }
    sim_state_t st = {
        .host_usb_state = 3u,
        .client_state = 2u,
        .owner_kind = 1u,
        .owner_epoch = 1u,
        .caps_led_known = true,
        .wifi_connected = true,
        .touch_healthy = true,
    };
    for (uint8_t host = 0; host < 6u; host++) { // every HOST and CLIENT status string of 01 §6
        st.host_usb_state = host;
        for (uint8_t client = 0; client < 7u; client++) {
            st.client_state = client;
            sim_publish(&sim, &st);
        }
    }
    st.host_usb_state = 3u;
    st.client_state = 2u;
    st.wifi_connected = false;
    sim_publish(&sim, &st);
    st.wifi_connected = true;

    for (uint16_t err = 0; err < 12u; err++) { // the details card renders the error text
        st.error_code = err;
        st.maintenance = (err % 2u) == 0u;
        sim_publish(&sim, &st);
        sim_click_obj(ui_topbar_status_item(sim.app.bar, UI_TOPBAR_HOST));
        sim_submit(&sim, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0);
        lv_test_wait(10);
    }
    st.error_code = 0;
    st.maintenance = false;
    sim_publish(&sim, &st);
    sim_click_obj(ui_topbar_status_item(sim.app.bar, UI_TOPBAR_CLIENT));
    sim_submit(&sim, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0);
    lv_test_wait(10);

    sim_submit(&sim, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_KEYBOARD);
    lv_test_wait(20);
    lv_obj_t *fn = sim_keyboard_key(&sim, "F1-F12");
    if (fn) {
        sim_click_obj(fn); // the F1-F12 layer legends
        sim_click_obj(fn);
    }
    const bool caps[3] = {false, true, true};
    const bool active[3] = {false, false, true};
    for (int i = 0; i < 3; i++) {
        st.caps_led_known = caps[i];
        st.caps_led_active = active[i];
        sim_publish(&sim, &st);
    }
    st.caps_led_known = true;
    st.caps_led_active = false;
    st.mods_armed = 0x02u;
    st.mods_locked = 0x01u;
    sim_publish(&sim, &st);
    st.mods_armed = 0u;
    st.mods_locked = 0u;
    sim_publish(&sim, &st);
    lv_obj_t *key_a = sim_keyboard_key(&sim, "A");
    if (key_a) {
        sim_click_obj(key_a);
    }

    sim_submit(&sim, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_SHORTCUTS);
    lv_test_wait(20);
    for (int i = 0; i < UI_SHORTCUT_MEDIA_COUNT; i++) {
        sim_click_obj(sim.app.shortcuts.media[i]); // the Consumer Control glyphs of ADR-018
    }
    sim_click_obj(sim.app.shortcuts.card[UI_SHORTCUT_DESKTOP]); // the feedback toast
    sim_click_obj(sim.app.shortcuts.card[UI_SHORTCUT_CLOSE_WINDOW]);
    if (sim.app.shortcuts.modal) {
        ui_modal_close(sim.app.shortcuts.modal); // the FR-734 confirmation text
        lv_test_wait(30);
    }
    if (sim.app.shortcuts.layout_key) {
        sim_click_obj(sim.app.shortcuts.layout_key);
    }
    const uint8_t owners[3] = {0u, 2u, 1u};
    for (int i = 0; i < 3; i++) {
        st.owner_kind = owners[i];
        sim_publish(&sim, &st);
    }
    st.maintenance = true;
    sim_publish(&sim, &st);
    st.maintenance = false;
    sim_publish(&sim, &st);

    sim_submit(&sim, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_MOUSE);
    lv_test_wait(20);
    st.drag_lock_active = true;
    sim_publish(&sim, &st);
    st.drag_lock_active = false;
    sim_publish(&sim, &st);
    st.host_usb_state = 1u; // "Host disconnected" on the touch surface
    sim_publish(&sim, &st);
    st.host_usb_state = 3u;
    sim_publish(&sim, &st);
    sim_click_obj(sim.app.mouse.btn_left);
    sim_click_obj(sim.app.mouse.btn_right);
    sim_click_obj(sim.app.mouse.btn_drag);

    // The pairing and control-approval prompts (TASK-100): the CLIENT card with its entry point, both faces
    // of the pairing card, the approval card and all five outcome toasts.
    st.client_state = 2u; // UI_CLIENT_LISTENING, with Wi-Fi up: the pairing entry point is enabled
    sim_publish(&sim, &st);
    sim_click_obj(ui_topbar_status_item(sim.app.bar, UI_TOPBAR_CLIENT));
    sim_submit(&sim, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0);
    lv_test_wait(10);
    ui_pair_view_t pv = {.window_open = true, .code_remaining_ms = 120000u, .window_remaining_ms = 180000u};
    const ui_pair_view_t pv_closed = {0};
    (void)snprintf(pv.code, sizeof(pv.code), "012345");
    ui_app_set_pairing(&sim.app, &pv);
    (void)snprintf(pv.code, sizeof(pv.code), "678901"); // every digit at the title size
    ui_app_set_pairing(&sim.app, &pv);
    pv.accept_pending = true;
    pv.accept_remaining_ms = 30000u;
    (void)snprintf(pv.client_name, sizeof(pv.client_name), "WARM-UP CLIENT");
    ui_app_set_pairing(&sim.app, &pv);
    lv_test_wait(20);
    ui_app_set_pairing(&sim.app, &pv_closed);
    ui_approval_view_t av = {.pending = true, .remaining_ms = 30000u, .session_id = 1u};
    (void)snprintf(av.client_name, sizeof(av.client_name), "WARM-UP CLIENT");
    ui_app_set_approval(&sim.app, &av);
    lv_test_wait(20);
    sim_click_obj(sim.app.remote.btn_deny);
    const ui_approval_view_t av_gone = {0};
    ui_app_set_approval(&sim.app, &av_gone);
    for (uint32_t r = UI_PAIR_RESULT_PAIRED; r <= UI_PAIR_RESULT_EXPIRED; r++) {
        ui_pair_view_t done = {.result = (ui_pair_result_t)r, .result_seq = r};
        (void)snprintf(done.result_name, sizeof(done.result_name), "WARM-UP CLIENT");
        ui_app_set_pairing(&sim.app, &done);
        lv_test_wait(20);
    }

    // Settings (TASK-070C): the card, every section with a full client register, and the confirmation card,
    // so every legend and every symbol of that screen is rasterized before the baseline is taken.
    ui_settings_view_t sv = {0};
    sv.seq = 1u;
    sv.remote_enabled = true;
    sv.pair_require_accept = true;
    sv.control_approval = 2u; // UI_CONTROL_APPROVAL_ALWAYS
    sv.keepawake_interval_s = 30u;
    sv.client_count = UI_SETTINGS_CLIENTS_MAX;
    for (uint8_t i = 0; i < UI_SETTINGS_CLIENTS_MAX; i++) {
        (void)snprintf(sv.client[i].name, sizeof(sv.client[i].name), "Warm client %u", (unsigned)i);
        sv.client[i].trusted = (i % 2u) == 0u;
    }
    sv.lang_ko = true; // the input language list of ADR-037, every row with its On face
    sv.lang_zh = true;
    sv.lang_ja = true;
    (void)snprintf(sv.host_layout, sizeof(sv.host_layout), "de(neo)");
    (void)snprintf(sv.firmware_version, sizeof(sv.firmware_version), "1.1.0 (simulator)");
    ui_app_set_settings(&sim.app, &sv);
    sim_click_obj(sim.app.settings_btn);
    lv_test_wait(20);
    for (uint8_t sec = 0; sec < UI_SETTINGS_SEC_COUNT; sec++) {
        sim_click_obj(sim.app.settings.tab[sec]);
        lv_test_wait(10);
    }
    sim_click_obj(sim.app.settings.tab[UI_SETTINGS_SEC_REMOTE]);
    sim_click_obj(ui_view_settings_approval_button(&sim.app.settings, UI_CONTROL_APPROVAL_AUTO));
    lv_test_wait(20);
    sim_click_obj(ui_view_settings_confirm_button(&sim.app.settings, false));
    lv_test_wait(20);
    sim_click_obj(sim.app.settings.tab[UI_SETTINGS_SEC_CLIENTS]);
    sim_click_obj(ui_view_settings_client_forget_button(&sim.app.settings, 0));
    lv_test_wait(20);
    sim_click_obj(ui_view_settings_confirm_button(&sim.app.settings, true));
    lv_test_wait(20);
    const ui_settings_view_t sv_empty = {0}; // the Unknown face of every pane
    ui_app_set_settings(&sim.app, &sv_empty);
    lv_test_wait(10);
    sim_submit(&sim, UI_ACT_DISMISS_OVERLAY, UI_CTRL_SETTINGS_CLOSE, 0);
    lv_test_wait(20);

    // The language key (ADR-037): the toast an English-only device shows, then the picker with the current
    // language, the "Current" tail and both reasons a language can be unavailable.
    sim_submit(&sim, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_KEYBOARD);
    lv_test_wait(20);
    lv_obj_t *lang = ui_view_keyboard_lang_key(&sim.app.keyboard);
    if (lang) {
        sim_click_obj(lang); // no language turned on: only the toast
        lv_test_wait(20);
        ui_app_set_settings(&sim.app, &sv);
        lv_test_wait(10);
        sim_click_obj(lang); // EN -> KO, so the cap carries another code
        sim_long_press_obj(lang);
        lv_test_wait(20);
        sim_submit(&sim, UI_ACT_DISMISS_OVERLAY, UI_CTRL_KBD_LANG_CLOSE, 0);
        lv_test_wait(20);
        sim_click_obj(lang); // back round the cycle to EN
        ui_app_set_settings(&sim.app, &sv_empty);
        lv_test_wait(10);
    }
    sim_submit(&sim, UI_ACT_NAVIGATE, UI_CTRL_NAV_BASE, UI_VIEW_MOUSE);
    lv_test_wait(20);

    lv_test_wait(7000); // outlive the longest toast so no transient object is left behind
    sim_app_stop(&sim);
    return true;
}

// ---- the CJK candidate fonts (TASK-072) --------------------------------------------------------------------------
// The blobs are derived from Noto Sans SC / JP by tools/fonts/gen_hanzi_font_blob.py and are not in the repository;
// without one the ZH / JA fixtures still build and run their checks, only their golden comparison is skipped.

static bool load_cjk(cjk_font_t *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  --%s %s: cannot open\n", c->what, path);
        return false;
    }
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        len = ftell(f);
    }
    rewind(f);
    c->blob = len > 0 ? malloc((size_t)len) : NULL;
    const bool read_ok = c->blob != NULL && fread(c->blob, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!read_ok) {
        printf("  --%s %s: read failed\n", c->what, path);
        return false;
    }
    const ui_font_blob_status_t st = ui_font_blob_load(&c->font, c->blob, (size_t)len, &lv_font_montserrat_24);
    printf("  %s    %s: %s, %u glyphs in %u part(s)\n", c->what, path, ui_font_blob_status_name(st),
           (unsigned)c->font.glyphs, (unsigned)c->font.parts);
    return st == UI_FONT_BLOB_OK;
}

// ---- one fixture -------------------------------------------------------------------------------------------------

static bool run_fixture(const opts_t *o, const char *name, row_t *row)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.fx", o->fixture_dir, name);
    sim_fixture_t fx;
    if (!sim_fixture_load(path, &fx)) {
        return false;
    }
    snprintf(row->name, sizeof(row->name), "%s", fx.name);
    snprintf(row->title, sizeof(row->title), "%s", fx.title);

    sim_app_t sim;
    memset(&sim, 0, sizeof(sim));
    sim.confirm_alt_f4 = fx.confirm_alt_f4;
    sim.layout_chord = fx.layout_chord;

    const size_t base_alloc = host_lv_live_count();
    if (!sim_app_start(&sim)) {
        printf("  %s: the shell refused to build\n", fx.name);
        return false;
    }
    bool ok = sim_fixture_apply(&sim, &fx);
    row->built = ok;

    if (ok) {
        row->geometry_ok = sim_check_geometry(&sim, fx.name, &row->checks);
        ok = row->geometry_ok;

        sim_image_t shot;
        if (!sim_image_render(sim.app.screen, &shot)) {
            ok = false;
        } else {
            char out_png[512];
            char gold_png[512];
            snprintf(out_png, sizeof(out_png), "%s/%s.png", o->out_dir, fx.name);
            snprintf(gold_png, sizeof(gold_png), "%s/%s.png", o->golden_dir, fx.name);
            if (shot.w != (uint32_t)UI_SCREEN_W || shot.h != (uint32_t)UI_SCREEN_H) {
                printf("  %s: rendered %ux%u, expected %dx%d\n", fx.name, shot.w, shot.h, UI_SCREEN_W,
                       UI_SCREEN_H);
                ok = false;
            }
            if (!sim_image_write_png(out_png, &shot)) {
                ok = false;
            }
            if (fx.zh_engine && !s_hanzi.ok) {
                row->gold = GOLD_SKIPPED; // the hanzi glyphs would render as the fallback font
                printf("  %s: golden skipped, no hanzi font (kvm_sim --hanzi <font_hanzi_24.bin>)\n", fx.name);
            } else if (fx.ja_engine && !s_kanji.ok) {
                row->gold = GOLD_SKIPPED;
                printf("  %s: golden skipped, no kanji font (kvm_sim --kanji <font_kanji_24.bin>)\n", fx.name);
            } else if (o->update_golden) {
                row->gold = sim_image_write_png(gold_png, &shot) ? GOLD_WRITTEN : GOLD_MISSING;
                ok = ok && row->gold == GOLD_WRITTEN;
            } else {
                sim_image_t gold;
                if (!sim_image_read_png(gold_png, &gold)) {
                    row->gold = GOLD_MISSING;
                    printf("  %s: no golden at %s (run with --update-golden)\n", fx.name, gold_png);
                    ok = false;
                } else {
                    const bool same = sim_image_compare(&shot, &gold, o->tolerance, o->budget, &row->diff);
                    row->gold = same ? GOLD_MATCH : GOLD_MISMATCH;
                    if (!same) {
                        printf("  %s: %u of %u pixels differ beyond tolerance %u (max channel delta %u, "
                               "first at %d,%d); rendered image kept at %s\n",
                               fx.name, row->diff.pixels_differing, row->diff.pixels_compared, o->tolerance,
                               row->diff.max_channel_delta, (int)row->diff.first_x, (int)row->diff.first_y,
                               out_png);
                        ok = false;
                    }
                    sim_image_free(&gold);
                }
            }
            sim_image_free(&shot);
        }

        sim_release(); // drop a held press from a `hold_key` step before the probing starts
        row->hidden_ok = sim_check_hidden(&sim, fx.name, &row->checks);
        ok = ok && row->hidden_ok;
    }

    sim_app_stop(&sim);
    const size_t alloc = host_lv_live_count();
    if (alloc != base_alloc) {
        printf("  %s: live LVGL allocations %zu -> %zu after the shell was destroyed (AT-UIFW-09)\n", fx.name,
               base_alloc, alloc);
        row->alloc_delta_nonzero = 1;
        ok = false;
    }
    return ok;
}

// ---- global checks -----------------------------------------------------------------------------------------------

static bool run_global_checks(const opts_t *o)
{
    sim_app_t sim;
    memset(&sim, 0, sizeof(sim));
    sim.confirm_alt_f4 = false;
    sim.layout_chord = 7u; // KVM_CHORD_LAYOUT_WIN; the value is irrelevant for these checks
    const size_t base_alloc = host_lv_live_count();
    if (!sim_app_start(&sim)) {
        printf("  global checks: the shell refused to build\n");
        return false;
    }
    const sim_state_t ready = {
        .host_usb_state = 3u, // KVM_HOST_USB_MOUNTED
        .client_state = 2u,   // UI_CLIENT_LISTENING
        .owner_kind = 1u,     // OWNER_LOCAL
        .owner_epoch = 1u,
        .caps_led_known = true,
        .wifi_connected = true,
        .touch_healthy = true,
    };
    sim_publish(&sim, &ready);
    sim_settle();

    bool ok = true;
    ok = sim_check_at04(&sim) && ok;
    ok = sim_check_at08(&sim) && ok;
    ok = sim_check_at10(&sim, o->modal_cycles) && ok;
    ok = sim_check_at02(&sim, o->switches) && ok;

    sim_app_stop(&sim);
    const size_t alloc = host_lv_live_count();
    if (alloc != base_alloc) {
        printf("  global checks: live LVGL allocations %zu -> %zu after teardown (AT-UIFW-09)\n", base_alloc,
               alloc);
        ok = false;
    }
    return ok;
}

// ---- reporting ---------------------------------------------------------------------------------------------------

static const char *gold_name(gold_status_t g)
{
    switch (g) {
    case GOLD_MATCH:
        return "match";
    case GOLD_MISMATCH:
        return "MISMATCH";
    case GOLD_MISSING:
        return "MISSING";
    case GOLD_WRITTEN:
        return "written";
    default:
        return "skipped";
    }
}

static void print_table(void)
{
    printf("\n  %-26s %-9s %5s %5s %5s %6s %-9s %s\n", "fixture", "built", "hits", "small", "over", "hidden",
           "golden", "diff px");
    printf("  %s\n", "--------------------------------------------------------------------------------------");
    for (int i = 0; i < s_row_count; i++) {
        const row_t *r = &s_rows[i];
        printf("  %-26s %-9s %5u %5u %5u %6u %-9s %u\n", r->name, r->built ? "yes" : "NO",
               r->checks.hit_targets, r->checks.too_small, r->checks.overlapping, r->checks.hidden_probed,
               gold_name(r->gold), r->diff.pixels_differing);
    }
}

// ---- main --------------------------------------------------------------------------------------------------------

static bool arg_u32(int argc, char **argv, int *i, uint32_t *out)
{
    if (*i + 1 >= argc) {
        printf("%s needs a value\n", argv[*i]);
        return false;
    }
    (*i)++;
    *out = (uint32_t)strtoul(argv[*i], NULL, 10);
    return true;
}

int main(int argc, char **argv)
{
    opts_t o = {
        .fixture_dir = KVM_SIM_FIXTURE_DIR,
        .golden_dir = KVM_SIM_GOLDEN_DIR,
        .out_dir = KVM_SIM_OUT_DIR,
        .tolerance = 2u,
        .budget = 64u,
        .switches = 2000u,
        .modal_cycles = 200u,
    };
    for (int i = 1; i < argc; i++) {
        uint32_t v = 0;
        if (strcmp(argv[i], "--update-golden") == 0) {
            o.update_golden = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            o.list_only = true;
        } else if (strcmp(argv[i], "--hanzi") == 0 && i + 1 < argc) {
            o.hanzi = argv[++i];
        } else if (strcmp(argv[i], "--kanji") == 0 && i + 1 < argc) {
            o.kanji = argv[++i];
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            o.only = argv[++i];
        } else if (strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) {
            o.fixture_dir = argv[++i];
        } else if (strcmp(argv[i], "--golden") == 0 && i + 1 < argc) {
            o.golden_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            o.out_dir = argv[++i];
        } else if (strcmp(argv[i], "--tolerance") == 0) {
            if (!arg_u32(argc, argv, &i, &v)) {
                return 2;
            }
            o.tolerance = (uint8_t)(v > 255u ? 255u : v);
        } else if (strcmp(argv[i], "--budget") == 0) {
            if (!arg_u32(argc, argv, &i, &o.budget)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--switches") == 0) {
            if (!arg_u32(argc, argv, &i, &o.switches)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--modal-cycles") == 0) {
            if (!arg_u32(argc, argv, &i, &o.modal_cycles)) {
                return 2;
            }
        } else {
            printf("unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    printf("kvm_sim (LVGL %d.%d.%d, %dx%d RGB565 headless)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH, UI_SCREEN_W, UI_SCREEN_H);
    printf("  fixtures %s\n  golden   %s\n  rendered %s\n", o.fixture_dir, o.golden_dir, o.out_dir);

    if (!collect_fixtures(o.fixture_dir)) {
        return 2;
    }
    if (o.list_only) {
        for (int i = 0; i < s_name_count; i++) {
            printf("  %s\n", s_names[i]);
        }
        return 0;
    }

    const size_t env_base = sim_env_start();
    if (!ui_theme_init()) { // the shared styles allocate once and live until ui_theme_deinit()
        printf("  ui_theme_init() failed\n");
        return 2;
    }
#ifdef KVM_HANZI_BLOB
    if (o.hanzi == NULL) {
        o.hanzi = KVM_HANZI_BLOB;
    }
#endif
#ifdef KVM_KANJI_BLOB
    if (o.kanji == NULL) {
        o.kanji = KVM_KANJI_BLOB;
    }
#endif
    if (o.hanzi != NULL && !(s_hanzi.ok = load_cjk(&s_hanzi, o.hanzi))) {
        return 2; // asked for and unusable: never fall back silently
    }
    if (o.kanji != NULL && !(s_kanji.ok = load_cjk(&s_kanji, o.kanji))) {
        return 2;
    }
    if (s_hanzi.ok) {
        ui_theme_set_hanzi_font(ui_font_blob_font(&s_hanzi.font));
    }
    if (s_kanji.ok) {
        ui_theme_set_kanji_font(ui_font_blob_font(&s_kanji.font));
    }
    if (!warm_up()) {
        printf("  the warm-up shell refused to build\n");
        return 2;
    }
    const size_t baseline = host_lv_live_count();
    printf("  live LVGL allocations: %zu after the display warm-up, %zu after the state tour\n\n", env_base,
           baseline);

    bool ok = true;
    for (int i = 0; i < s_name_count; i++) {
        if (o.only && strcmp(o.only, s_names[i]) != 0) {
            continue;
        }
        printf("  [%s]\n", s_names[i]);
        row_t *row = &s_rows[s_row_count++];
        memset(row, 0, sizeof(*row));
        snprintf(row->name, sizeof(row->name), "%s", s_names[i]);
        ok = run_fixture(&o, s_names[i], row) && ok;
    }
    if (s_row_count == 0) {
        printf("  no fixture matched --only %s\n", o.only ? o.only : "");
        ok = false;
    }

    if (!o.only) {
        printf("\n  [acceptance checks]\n");
        ok = run_global_checks(&o) && ok;
    } else {
        printf("\n  acceptance checks skipped (--only)\n");
    }

    print_table();
    const size_t live = host_lv_live_count();
    printf("\n  live LVGL allocations at the end: %zu (baseline %zu)\n", live, baseline);
    if (live != baseline) {
        printf("  AT-UIFW-09: the run did not return to the baseline\n");
        ok = false;
    }
    ui_theme_deinit();
    ui_theme_set_hanzi_font(NULL);
    ui_theme_set_kanji_font(NULL);
    free(s_hanzi.blob);
    free(s_kanji.blob);
    sim_env_stop(baseline);
    printf("  %s\n", ok ? "kvm_sim: PASS" : "kvm_sim: FAIL");
    return ok ? 0 : 1;
}
