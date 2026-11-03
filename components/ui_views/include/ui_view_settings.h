// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Settings of the DEVICE (TASK-070C settings part; FR-731..FR-736, FR-711/FR-712, FR-741, FR-747, FR-751,
// SEC-107, TECH-084, ADR-020, ADR-022, ADR-023, ADR-024, ADR-025).
//
// Shape (07 §5): Settings is one blocking overlay, not a fourth primary view - the router rejects
// UI_VIEW_SETTINGS as a primary view, and 07 §5 keeps "at most three main views". The card is almost
// full-screen and is split into a section list on the left and one pane per section on the right, so a
// section change moves no touch target off the panel and allocates nothing (every pane is built once and
// hidden). Opening it is a navigation: the overlay stack cancels the pending touch and asks for the HID
// release before the card appears, takes the pad away while it is up and gives it back on dismiss.
//
// The UI is not the source of truth (AGENTS-UI MUST 5). Every value rendered here comes from
// `ui_settings_view_t`, which the domain adapter publishes through ui_app_set_settings(); a tap is reported
// as one typed change through `ui_app_hooks_t::settings_change` and nothing else happens. In particular a
// toggle does NOT move when it is tapped (MUST 6): it keeps showing the published value until the domain
// republishes the record it actually wrote. A refused or clamped write therefore shows up as "the switch did
// not move", never as a GUI state the flash does not hold.
//
// Physical presence is the trust anchor (ADR-023 SEC-107): these controls exist only here, the CLIENT has no
// `settings.*` command, and relaxing an approval policy - `pair.require_accept` to OFF, or `control.approval`
// to a less strict value - needs a second, explicit tap in a confirmation card. Forgetting a paired client is
// confirmed the same way. The hard invariants of ADR-023 (TLS + pinning, lease/epoch, RELEASE_ALL, the
// 180 s pairing window, LOCAL > REMOTE, remote off on first boot, no remote settings change, eFuses) get no
// control at all: they are not in `ui_setting_id_t`, so this screen cannot express them.
//
// Secrets (SEC-105 / 07 §7): no token, token digest, session id, SSID, IP address or pairing code reaches
// this view - the snapshot carries none. A paired client's name arrives from the network, so it is copied
// through a bounded, printable-only copy before it is rendered, and it is never logged.
//
// Not here: HID (no tusb.h, no report), pairing policy, the pairing window itself (ui_remote_view owns the
// prompts; "Pair a client" only reports UI_REMOTE_ACT_PAIR_OPEN) and NVS access (config_store owns the record).
//
// The HOST layout (FR-741) is chosen here, from the generated `kbd_layouts` tables: the row opens a two-level
// picker - layout, then variant - inside the Keyboard pane, and a pick is reported as the index of the entry in
// those tables. A build whose tables are absent, or a domain that reports no layout database, shows the stored
// id read-only with a note that says why, and never an invented id.
//
// LVGL owner context only; ui_theme_init() must have run.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_actions.h"
#include "ui_overlay.h"
#include "ui_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Overlay ids. Both are blocking and they nest exactly two deep, which is UI_OVERLAY_STACK_MAX: the
 * Settings card, plus one confirmation over it. Nothing deeper is possible, so a confirmation can never be
 * hidden behind another card.
 */
#define UI_OVERLAY_SETTINGS 0x0401u
#define UI_OVERLAY_SETTINGS_CONFIRM 0x0402u

/** Control ids (07 §3: UI-local stable ids, never keycodes). */
#define UI_CTRL_SETTINGS_OPEN 0x0410u          // the gear in the top bar
#define UI_CTRL_SETTINGS_CLOSE 0x0411u         // Close: submits UI_ACT_DISMISS_OVERLAY
#define UI_CTRL_SETTINGS_PAIR 0x0412u          // "Pair a client" (ADR-022 step 1)
#define UI_CTRL_SETTINGS_INTERVAL_DOWN 0x0413u // keep-awake interval -10 s
#define UI_CTRL_SETTINGS_INTERVAL_UP 0x0414u   // keep-awake interval +10 s
#define UI_CTRL_SETTINGS_CONFIRM_YES 0x0415u
#define UI_CTRL_SETTINGS_CONFIRM_NO 0x0416u
#define UI_CTRL_SETTINGS_UNICODE_PREV 0x0417u  // Unicode delivery: previous mode
#define UI_CTRL_SETTINGS_UNICODE_NEXT 0x0418u  // Unicode delivery: next mode
#define UI_CTRL_SETTINGS_LAYOUT_OPEN 0x0419u   // FR-741: open the HOST layout picker inside the Keyboard pane
#define UI_CTRL_SETTINGS_LAYOUT_BACK 0x041Au   // one level up, or back to the pane
#define UI_CTRL_SETTINGS_LAYOUT_PREV 0x041Bu   // previous page of the list
#define UI_CTRL_SETTINGS_LAYOUT_NEXT 0x041Cu   // next page
#define UI_CTRL_SETTINGS_LAYOUT_SEARCH 0x041Du // ADR-039: open the search screen
#define UI_CTRL_SETTINGS_LAYOUT_ALL 0x041Eu    // ADR-039: open the full list
#define UI_CTRL_SETTINGS_LAYOUT_ERASE 0x041Fu  // ADR-039: backspace of the search keyboard
#define UI_CTRL_SETTINGS_LAYOUT_CLEAR 0x0460u  // ADR-039: clear the whole query
#define UI_CTRL_SETTINGS_LAYOUT_ROW_BASE 0x0470u // + row on the page (0..UI_SETTINGS_PICK_ROWS-1)
#define UI_CTRL_SETTINGS_LAYOUT_STAR_BASE 0x0480u  // ADR-039: + row; pins or unpins that row's layout
#define UI_CTRL_SETTINGS_LAYOUT_FAV_BASE 0x0490u   // ADR-039: + tile (0..UI_SETTINGS_FAV_MAX-1)
#define UI_CTRL_SETTINGS_LAYOUT_RECENT_BASE 0x04A0u // ADR-039: + chip (0..UI_SETTINGS_RECENT_MAX-1)
#define UI_CTRL_SETTINGS_LAYOUT_INDEX_BASE 0x04B0u  // ADR-039: + bucket (0..UI_SETTINGS_PICK_INDEX-1)
#define UI_CTRL_SETTINGS_LAYOUT_KEY_BASE 0x04C0u    // ADR-039: + key of the search keyboard
#define UI_CTRL_SETTINGS_SECTION_BASE 0x0420u  // + ui_settings_section_t
#define UI_CTRL_SETTINGS_TOGGLE_BASE 0x0430u   // + ui_setting_id_t
#define UI_CTRL_SETTINGS_APPROVAL_BASE 0x0440u // + ui_control_approval_t
#define UI_CTRL_SETTINGS_TRUSTED_BASE 0x0450u  // + client index
#define UI_CTRL_SETTINGS_FORGET_BASE 0x0460u   // + client index

/** Sections of the card, in the order the list shows them. */
typedef enum {
    UI_SETTINGS_SEC_REMOTE = 0, // FR-506, FR-732, FR-733 and the pairing entry point
    UI_SETTINGS_SEC_CLIENTS,    // FR-726 register: trusted flag and Forget
    UI_SETTINGS_SEC_SHORTCUTS,  // FR-734
    UI_SETTINGS_SEC_INPUT,      // ADR-020 keep awake
    UI_SETTINGS_SEC_KEYBOARD,   // ADR-037 input languages, FR-747 stepper, FR-741 (read-only until TASK-071)
    UI_SETTINGS_SEC_ABOUT,      // FR-751, TECH-084
    UI_SETTINGS_SEC_COUNT,
} ui_settings_section_t;

/**
 * FR-733 control approval policy. The values are the on-flash values of `kvm_control_approval_t`
 * (config_store), ordered so that a HIGHER value is STRICTER - that ordering is what decides whether a tap
 * relaxes a policy and therefore needs a confirmation (ADR-023 FR-736). ui_views does not depend on
 * config_store, so the three values are mirrored here; a test pins them.
 */
typedef enum {
    UI_CONTROL_APPROVAL_AUTO = 0,
    UI_CONTROL_APPROVAL_TRUSTED = 1,
    UI_CONTROL_APPROVAL_ALWAYS = 2,
    UI_CONTROL_APPROVAL_COUNT,
} ui_control_approval_t;

/**
 * FR-747 Unicode delivery mode, mirroring `kvm_unicode_mode_t`. Chosen with the < > stepper of the Keyboard pane
 * (UI_SETTING_UNICODE_MODE); the pane shows the one-screen setup hint of the selected mode.
 */
typedef enum {
    UI_UNICODE_MODE_HOST_IME = 0,
    UI_UNICODE_MODE_LINUX = 1,
    UI_UNICODE_MODE_MACOS = 2,
    UI_UNICODE_MODE_WINDOWS_HEX = 3,
    UI_UNICODE_MODE_WINCOMPOSE = 4,
    UI_UNICODE_MODE_COUNT,
} ui_unicode_mode_t;

/** What a tap asks the domain to write. Every id maps to one field of the config_store record. */
typedef enum {
    UI_SETTING_REMOTE_ENABLED = 0,   // 0/1, FR-506 / TECH-040
    UI_SETTING_PAIR_REQUIRE_ACCEPT,  // 0/1, FR-732 (default ON)
    UI_SETTING_CONTROL_APPROVAL,     // ui_control_approval_t, FR-733 (default ALWAYS)
    UI_SETTING_CONFIRM_ALT_F4,       // 0/1, FR-734 (default OFF)
    UI_SETTING_KEEPAWAKE_ENABLED,    // 0/1, FR-711 (default OFF)
    UI_SETTING_KEEPAWAKE_INTERVAL_S, // 10..300 s, ADR-020 (default 30)
    UI_SETTING_CLIENT_TRUSTED,       // 0/1 for `client_index`, FR-733 trusted flag
    UI_SETTING_CLIENT_FORGET,        // `client_index`; `value` is unused
    UI_SETTING_UNICODE_MODE,         // ui_unicode_mode_t, FR-747 (default HOST_IME)
    UI_SETTING_LANG_KO,              // 0/1, FR-746 / ADR-037 (default OFF)
    UI_SETTING_LANG_ZH,              // 0/1, FR-746 / ADR-037 (default OFF)
    UI_SETTING_LANG_JA,              // 0/1, FR-746 / ADR-037 (default OFF)
    /**
     * FR-741 HOST layout. `value` is an index into the generated layout tables (kbd_layouts_at()), not a
     * keycode and not a string: the id itself is `kbd_layouts_at(value)->id`, which is what the domain writes to
     * the record. An index is what a list of 581 entries can report through one int32_t, and a value the tables
     * do not have is refused by the domain rather than guessed.
     */
    UI_SETTING_HOST_LAYOUT,
    /**
     * ADR-039: pin or unpin one layout. `value` is a table index, exactly as UI_SETTING_HOST_LAYOUT; the shell
     * decides whether that is a pin or an unpin from the list it holds, keeps the order, and refuses a ninth
     * favorite. This screen never edits the list itself - it redraws the one that comes back.
     */
    UI_SETTING_HOST_FAV_TOGGLE,
    UI_SETTING_COUNT,
} ui_setting_id_t;

/** Reported once per accepted tap. The UI never writes anything itself and learns the outcome as a publish. */
typedef void (*ui_settings_sink_fn)(void *ctx, ui_setting_id_t id, int32_t value, uint8_t client_index);

/** ADR-020: the keep-awake interval is 10..300 s and this screen steps it by 10 s. */
#define UI_SETTINGS_INTERVAL_MIN 10u
#define UI_SETTINGS_INTERVAL_MAX 300u
#define UI_SETTINGS_INTERVAL_STEP 10u

/** KVM_CONFIG_MAX_PAIRED (FR-726) and KVM_CONFIG_PAIR_NAME_CAP, mirrored; a test pins both. */
#define UI_SETTINGS_CLIENTS_MAX 8
#define UI_SETTINGS_NAME_CAP 24

/** Capacity of the firmware version string the domain publishes (TECH-084 / FR-751). */
#define UI_SETTINGS_VERSION_CAP 32

/** Capacity of a layout id, KVM_CONFIG_LAYOUT_ID_CAP ("us", "de(neo)", ...). */
#define UI_SETTINGS_LAYOUT_CAP 16

#define UI_SETTINGS_OVERLAYS 2

/** ADR-039 section 1: the four screens of the HOST layout picker. `Back` walks them from right to left. */
typedef enum {
    UI_PICK_HOME = 0,  // the current card, the favorites tiles, the recents row, Search and All layouts
    UI_PICK_LIST,      // the 96 layout groups, six rows a page, with the index strip
    UI_PICK_VARIANTS,  // the variants of one group
    UI_PICK_SEARCH,    // the query field, the on-screen keyboard and the matching rows
    UI_PICK_SCREEN_COUNT,
} ui_pick_screen_t;

/**
 * FR-741: rows of the HOST layout picker on one page. The picker is drawn inside the Keyboard pane and not as a
 * third overlay - the stack is two deep and the confirmation card owns the second level (ADR-023), so a card over
 * the card would either break that rule or hide a confirmation. 581 layouts are therefore reached as a page of
 * fixed rows: the widgets are built once with the pane and only their text changes, so paging through the whole
 * list costs no allocation and no widget (AT-UIFW-09/10), and the two levels - layout, then variant - keep each
 * page short enough to read on the 1024x600 screen.
 */
#define UI_SETTINGS_PICK_ROWS 6

/**
 * ADR-039: the picker's favorites (pinned by hand) and recents (the last applied layouts, newest first).
 * They mirror KVM_CONFIG_HOST_FAV_MAX and KVM_CONFIG_HOST_RECENT_MAX; a static assertion in the source pins
 * both to the record. The device never unpins a favorite on its own, so pinning a ninth one is refused.
 */
#define UI_SETTINGS_FAV_MAX 8
#define UI_SETTINGS_RECENT_MAX 5

/**
 * ADR-039 section 7: buckets of the alphabetical index strip. Nine, not 26: a 26-row strip over the pane gives
 * each letter 17 px, far under the UI_TOUCH_MIN of 44 px that AT-UIFW-01 requires.
 */
#define UI_SETTINGS_PICK_INDEX 9

/** ADR-039 section 5: the search query and the keys of the picker's own on-screen keyboard. */
#define UI_SETTINGS_QUERY_CAP 24
#define UI_SETTINGS_QUERY_KEYS 32

/** ADR-039 section 2: slots of the home row previewed on the current-layout card (AC01..AC11). */
#define UI_SETTINGS_PREVIEW_KEYS 11

/** Room for one row of the picker ("de(neo)  German (Neo 2)") and for its heading. */
#define UI_SETTINGS_PICK_TEXT 64

/** One paired client as the register holds it (ADR-022 FR-726). No token, no digest, no address. */
typedef struct {
    char name[UI_SETTINGS_NAME_CAP];
    bool trusted; // FR-733
} ui_settings_client_t;

/**
 * The settings snapshot the domain adapter publishes (main/, never this component). `seq` is 0 until the
 * first publish; while it is 0 the panes render "Unknown" and every control is disabled, because a screen
 * that has not been told a policy must not offer to change it (07 §12 item 2, fail closed).
 */
typedef struct {
    uint32_t seq;

    bool remote_enabled;         // FR-506
    bool pair_require_accept;    // FR-732
    uint8_t control_approval;    // ui_control_approval_t, FR-733
    bool confirm_alt_f4;         // FR-734
    bool keepawake_enabled;      // FR-711
    uint16_t keepawake_interval_s;

    uint8_t unicode_mode;                     // ui_unicode_mode_t, FR-747
    /** ADR-037: the composed languages the KEYBOARD view's language key offers. English is always on. */
    bool lang_ko;
    bool lang_zh;
    bool lang_ja;
    char host_layout[UI_SETTINGS_LAYOUT_CAP]; // FR-741 xkeyboard-config id
    /** The domain has a layout database (the generated tables are in this build): without one the row is
     *  read-only and says so, with the id the domain published and nothing invented. */
    bool layout_db_available;
    /**
     * ADR-039: the pinned layouts and the last applied ones, as the record holds them - ids, newest first for
     * the recents. The shell owns the ordering, the deduplication and the limits; this screen only draws them
     * and reports a pin or a pick. An id the tables no longer have stays in the record and is skipped here.
     */
    char host_fav[UI_SETTINGS_FAV_MAX][UI_SETTINGS_LAYOUT_CAP];
    uint8_t host_fav_count;
    char host_recent[UI_SETTINGS_RECENT_MAX][UI_SETTINGS_LAYOUT_CAP];
    uint8_t host_recent_count;

    char firmware_version[UI_SETTINGS_VERSION_CAP]; // FR-751

    uint8_t client_count; // <= UI_SETTINGS_CLIENTS_MAX
    ui_settings_client_t client[UI_SETTINGS_CLIENTS_MAX];
} ui_settings_view_t;

typedef struct {
    uint32_t opens;          // Settings cards built
    uint32_t closes;
    uint32_t close_taps;
    uint32_t section_taps;
    uint32_t reported;       // changes handed to the sink
    uint32_t refused;        // a tap with no sink, no snapshot, or an out-of-range value
    uint32_t confirms_shown; // confirmation cards opened (policy relaxation, Forget)
    uint32_t confirms_yes;
    uint32_t confirms_no;
    uint32_t pair_taps;      // "Pair a client" reported to the shell
} ui_view_settings_counters_t;

/** One row of a pane: the control plus the two labels that say what it is and what it does. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *value;   // the toggle button's legend ("On" / "Off" / "Unknown")
    lv_obj_t *control; // the tappable control, or NULL for a read-only row
} ui_settings_row_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *trusted_btn;
    lv_obj_t *trusted_label;
    lv_obj_t *forget_btn;
} ui_settings_client_row_t;

typedef struct ui_view_settings_s ui_view_settings_t;

struct ui_view_settings_s {
    // bound at init
    ui_actions_t *actions; // Close submits UI_ACT_DISMISS_OVERLAY here (a local action, 07 §3)
    ui_router_t *router;   // overlay stack; also the fallback when no local sink took the action
    uint64_t (*now_us)(void);
    ui_settings_sink_fn sink;
    void *sink_ctx;
    /** ADR-022 step 1: the local user asked for a pairing window. NULL = no entry point is built. */
    void (*pair_action)(void *ctx);
    void *pair_ctx;

    ui_overlay_descriptor_t ov[UI_SETTINGS_OVERLAYS]; // hand these to ui_router_init()

    ui_settings_view_t v; // the only source of what the card renders
    ui_settings_section_t section;

    // objects of the open card (NULL while it is closed)
    lv_obj_t *card;
    lv_obj_t *tab[UI_SETTINGS_SEC_COUNT];
    lv_obj_t *pane[UI_SETTINGS_SEC_COUNT];
    ui_settings_row_t toggle[UI_SETTING_COUNT];      // only the 0/1 settings use an entry here
    lv_obj_t *approval[UI_CONTROL_APPROVAL_COUNT];
    lv_obj_t *approval_label[UI_CONTROL_APPROVAL_COUNT];
    lv_obj_t *approval_note;
    lv_obj_t *pair_btn;
    lv_obj_t *pair_note;
    lv_obj_t *interval_value;
    lv_obj_t *interval_down;
    lv_obj_t *interval_up;
    lv_obj_t *clients_heading;
    ui_settings_client_row_t client_row[UI_SETTINGS_CLIENTS_MAX];
    lv_obj_t *clients_empty;
    lv_obj_t *layout_value;
    // FR-741 HOST layout picker, inside the Keyboard pane: `kbd_main` carries the ordinary rows and `pick_box`
    // the list, and exactly one of the two is visible.
    lv_obj_t *kbd_main;
    lv_obj_t *layout_btn;   // the row's control; NULL while the domain reports no layout database
    lv_obj_t *pick_box;
    lv_obj_t *pick_title;
    lv_obj_t *pick_back;
    lv_obj_t *pick_prev;
    lv_obj_t *pick_next;
    lv_obj_t *pick_page_label;
    lv_obj_t *pick_row[UI_SETTINGS_PICK_ROWS];
    lv_obj_t *pick_label[UI_SETTINGS_PICK_ROWS];
    lv_obj_t *pick_star_icon[UI_SETTINGS_PICK_ROWS]; /**< the drawn star inside that button */
    lv_obj_t *pick_star[UI_SETTINGS_PICK_ROWS];  // ADR-039: pins or unpins the layout of that row
    lv_obj_t *pick_foot;                         // the paging footer, hidden on the HOME screen
    bool pick_open;         // the picker is up instead of the rows
    ui_pick_screen_t pick_screen;
    bool pick_long_press;   // a long press on a favorite tile unpinned it; the click that follows is ignored
    uint16_t pick_group;    // the layout group whose variants UI_PICK_VARIANTS lists
    uint16_t pick_page;     // index of the first entry on the page
    char pick_text[UI_SETTINGS_PICK_ROWS][UI_SETTINGS_PICK_TEXT];
    char pick_title_text[UI_SETTINGS_PICK_TEXT];
    char pick_page_text[24];
    char layout_btn_text[UI_SETTINGS_PICK_TEXT];

    // ADR-039 HOME: the current-layout card, the favorites tiles and the recents chips. Built once with the
    // pane and only relabelled, so no screen of the picker allocates (AT-UIFW-09/10).
    lv_obj_t *pick_home;
    lv_obj_t *pick_cur_name;
    lv_obj_t *pick_preview[UI_SETTINGS_PREVIEW_KEYS];
    lv_obj_t *pick_fav_box;
    lv_obj_t *pick_fav[UI_SETTINGS_FAV_MAX];
    lv_obj_t *pick_fav_label[UI_SETTINGS_FAV_MAX];
    lv_obj_t *pick_fav_empty;
    lv_obj_t *pick_recent_box;
    lv_obj_t *pick_recent[UI_SETTINGS_RECENT_MAX];
    lv_obj_t *pick_recent_label[UI_SETTINGS_RECENT_MAX];
    lv_obj_t *pick_recent_empty;
    lv_obj_t *pick_search_btn;
    lv_obj_t *pick_all_btn;
    lv_obj_t *pick_note;    // "8 favorites is the limit" and the other one-line answers of the picker
    char pick_cur_text[UI_SETTINGS_PICK_TEXT];
    char pick_preview_text[UI_SETTINGS_PREVIEW_KEYS][8];
    char pick_fav_text[UI_SETTINGS_FAV_MAX][UI_SETTINGS_PICK_TEXT];
    char pick_recent_text[UI_SETTINGS_RECENT_MAX][UI_SETTINGS_PICK_TEXT];
    char pick_note_text[UI_SETTINGS_PICK_TEXT];

    // ADR-039 LIST: the alphabetical index strip.
    uint16_t pick_list_page; /**< the page the list was on when a variant level was entered */
    lv_obj_t *pick_rows_box; /**< the column of rows, whose height decides how many of them are used */
    uint8_t pick_rows_fit;   /**< rows the pane has room for right now: 1..UI_SETTINGS_PICK_ROWS */
    lv_obj_t *pick_list_box; /**< the rows and the index strip: everything the home screen does not use */
    lv_obj_t *pick_index_box;
    lv_obj_t *pick_index[UI_SETTINGS_PICK_INDEX];

    // ADR-039 SEARCH: the query field and the picker's own on-screen keyboard. It types into this device only
    // and shares nothing with the KEYBOARD view, which emits USB HID (AGENTS-UI MUST 4).
    lv_obj_t *pick_search_box;
    lv_obj_t *pick_query_label;
    lv_obj_t *pick_key[UI_SETTINGS_QUERY_KEYS];
    lv_obj_t *pick_erase;   // backspace
    lv_obj_t *pick_clear;   // clears the whole query
    char query[UI_SETTINGS_QUERY_CAP];
    char query_shown[UI_SETTINGS_QUERY_CAP + 16];
    uint8_t query_len;
    lv_obj_t *unicode_value;
    lv_obj_t *unicode_prev;
    lv_obj_t *unicode_next;
    lv_obj_t *unicode_hint;
    lv_obj_t *layout_note;
    lv_obj_t *version_value;

    // objects of the open confirmation card
    lv_obj_t *confirm_card;
    lv_obj_t *confirm_heading;
    lv_obj_t *confirm_body;
    lv_obj_t *confirm_yes;
    lv_obj_t *confirm_no;
    /** The change the confirmation asks about; only applied when Yes is tapped. */
    struct {
        ui_setting_id_t id;
        int32_t value;
        uint8_t client_index;
        bool valid;
    } pending;
    char confirm_text[160];
    char name_text[UI_SETTINGS_NAME_CAP]; // printable-only copy of the name the confirmation renders

    ui_view_settings_counters_t c;
};

/**
 * Fill `s->ov` with the two overlay descriptors. Builds no object. `actions` may be NULL (Close then falls
 * back to `router`); `sink` may be NULL, and every control is then disabled instead of pretending to work.
 */
void ui_view_settings_init(ui_view_settings_t *s, ui_actions_t *actions, ui_router_t *router,
                           uint64_t (*now_us)(void), ui_settings_sink_fn sink, void *sink_ctx);

/** The descriptor array to register with the router; `count` receives UI_SETTINGS_OVERLAYS. */
const ui_overlay_descriptor_t *ui_view_settings_overlays(ui_view_settings_t *s, uint8_t *count);

/**
 * Install the "Pair a client" entry point of the Remote section (ADR-022 step 1). Without it the button is
 * not built at all: a shell that cannot pair shows no path to pairing.
 */
void ui_view_settings_set_pair_action(ui_view_settings_t *s, void (*fn)(void *ctx), void *ctx);

/**
 * Publish the settings snapshot. Call it from the LVGL owner context on every change; an open card is
 * re-rendered from it and nothing else moves. This is the only way a value on screen changes.
 */
void ui_view_settings_set(ui_view_settings_t *s, const ui_settings_view_t *v);

/** The published snapshot the card renders (never NULL for a live view). */
const ui_settings_view_t *ui_view_settings_snapshot(const ui_view_settings_t *s);

/** The open card of an overlay id, or NULL when that card is not up. */
lv_obj_t *ui_view_settings_card(const ui_view_settings_t *s, uint16_t overlay_id);

/** Show a section of the open card. False for an out-of-range section or a closed card. */
bool ui_view_settings_show_section(ui_view_settings_t *s, ui_settings_section_t section);

/** The section the open card shows. */
ui_settings_section_t ui_view_settings_section(const ui_view_settings_t *s);

/** The tappable control of a 0/1 setting in the open card, or NULL. For tests and diagnostics. */
lv_obj_t *ui_view_settings_toggle(const ui_view_settings_t *s, ui_setting_id_t id);

/** Rendered legend of a 0/1 setting ("On" / "Off" / "Unknown"), or NULL when its pane is not built. */
const char *ui_view_settings_toggle_text(const ui_view_settings_t *s, ui_setting_id_t id);

/** One of the three FR-733 policy buttons, or NULL. */
lv_obj_t *ui_view_settings_approval_button(const ui_view_settings_t *s, ui_control_approval_t which);

/** "Pair a client", or NULL when the card is closed or the entry point was not installed. */
lv_obj_t *ui_view_settings_pair_button(const ui_view_settings_t *s);

/** Keep-awake interval as the card renders it ("30 s" / "Unknown"), or NULL. */
const char *ui_view_settings_interval_text(const ui_view_settings_t *s);
lv_obj_t *ui_view_settings_interval_button(const ui_view_settings_t *s, bool up);

/** Row widgets of paired client `index`, or NULL when the card is closed or the row is unused. */
lv_obj_t *ui_view_settings_client_trusted_button(const ui_view_settings_t *s, uint8_t index);
lv_obj_t *ui_view_settings_client_forget_button(const ui_view_settings_t *s, uint8_t index);
const char *ui_view_settings_client_name_text(const ui_view_settings_t *s, uint8_t index);

/** FR-741 / FR-747 values as the Keyboard pane renders them, or NULL when the card is closed. */
const char *ui_view_settings_layout_text(const ui_view_settings_t *s);

/** FR-741: the control that opens the HOST layout picker, or NULL without a layout database or a card. */
lv_obj_t *ui_view_settings_layout_button(const ui_view_settings_t *s);

/** Whether the picker is up, and its heading ("Host layout" / "German") - "" when it is down. */
bool ui_view_settings_picker_open(const ui_view_settings_t *s);
const char *ui_view_settings_picker_title(const ui_view_settings_t *s);

/** Row `row` of the open page: its button, its text ("de(neo)  German (Neo 2)") and the page counter. */
lv_obj_t *ui_view_settings_picker_row(const ui_view_settings_t *s, uint8_t row);
const char *ui_view_settings_picker_row_text(const ui_view_settings_t *s, uint8_t row);
const char *ui_view_settings_picker_page_text(const ui_view_settings_t *s);

/** The Back, previous-page and next-page controls of the open picker, or NULL. */
lv_obj_t *ui_view_settings_picker_back(const ui_view_settings_t *s);
lv_obj_t *ui_view_settings_picker_step(const ui_view_settings_t *s, bool next);

/** ADR-039: which of the four screens the open picker shows. UI_PICK_HOME while it is closed. */
ui_pick_screen_t ui_view_settings_picker_screen(const ui_view_settings_t *s);

/** ADR-039 HOME: the current-layout line ("German - de(neo)") and one slot of its home-row preview. */
const char *ui_view_settings_picker_current_text(const ui_view_settings_t *s);
const char *ui_view_settings_picker_preview_text(const ui_view_settings_t *s, uint8_t slot);

/** ADR-039 HOME: favorite tile `i` and recent chip `i` - their control and their text, or NULL / "". */
lv_obj_t *ui_view_settings_picker_fav(const ui_view_settings_t *s, uint8_t i);
const char *ui_view_settings_picker_fav_text(const ui_view_settings_t *s, uint8_t i);
lv_obj_t *ui_view_settings_picker_recent(const ui_view_settings_t *s, uint8_t i);
const char *ui_view_settings_picker_recent_text(const ui_view_settings_t *s, uint8_t i);

/** ADR-039 HOME: the Search and All layouts controls, and the one-line answer under them ("" when silent). */
lv_obj_t *ui_view_settings_picker_search_button(const ui_view_settings_t *s);
lv_obj_t *ui_view_settings_picker_all_button(const ui_view_settings_t *s);
const char *ui_view_settings_picker_note(const ui_view_settings_t *s);

/** ADR-039: the star of row `row`, which pins or unpins that row's layout. */
lv_obj_t *ui_view_settings_picker_star(const ui_view_settings_t *s, uint8_t row);

/** ADR-039 LIST: bucket `i` of the index strip ("A-C" ... "Y-Z"). */
lv_obj_t *ui_view_settings_picker_index(const ui_view_settings_t *s, uint8_t i);

/** ADR-039 SEARCH: key `i` of the picker's own keyboard, the backspace, and the query as the field shows it. */
lv_obj_t *ui_view_settings_picker_key(const ui_view_settings_t *s, uint8_t i);
const char *ui_view_settings_picker_query(const ui_view_settings_t *s);

/** The character search key `i` appends, or 0 when there is no such key. */
char ui_view_settings_picker_key_char(const ui_view_settings_t *s, uint8_t i);

/** The backspace key of the search screen, or NULL when that screen is not up. */
lv_obj_t *ui_view_settings_picker_erase(const ui_view_settings_t *s);

/** The key that empties the search query, or NULL when the search screen is not up. */
lv_obj_t *ui_view_settings_picker_clear(const ui_view_settings_t *s);
const char *ui_view_settings_unicode_text(const ui_view_settings_t *s);
/** The setup hint of the selected Unicode mode, or NULL when the card is closed. */
const char *ui_view_settings_unicode_hint(const ui_view_settings_t *s);
/** The < (false) or > (true) button of the Unicode delivery stepper, or NULL when the card is closed. */
lv_obj_t *ui_view_settings_unicode_button(const ui_view_settings_t *s, bool next);

/** FR-751: the firmware version line as the About pane renders it, or NULL. */
const char *ui_view_settings_version_text(const ui_view_settings_t *s);

/** True while the confirmation card is up (a policy relaxation or a Forget awaiting a second tap). */
bool ui_view_settings_confirm_open(const ui_view_settings_t *s);

/** Heading and question of the open confirmation card, or NULL. */
const char *ui_view_settings_confirm_heading(const ui_view_settings_t *s);
const char *ui_view_settings_confirm_body(const ui_view_settings_t *s);
lv_obj_t *ui_view_settings_confirm_button(const ui_view_settings_t *s, bool yes);

/**
 * True when writing `value` to `id` relaxes an ADR-023 approval policy against the published snapshot, i.e.
 * when the tap needs the confirmation card. Pure function of the snapshot: no LVGL, no side effect.
 */
bool ui_view_settings_relaxes_policy(const ui_view_settings_t *s, ui_setting_id_t id, int32_t value);

/** Stable setting id as the UI and the diagnostics dump name it ("control.approval"). Never NULL. */
const char *ui_setting_key(ui_setting_id_t id);

#ifdef __cplusplus
}
#endif
