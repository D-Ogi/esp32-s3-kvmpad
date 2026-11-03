// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// KEYBOARD view (TASK-070B). See ui_view_keyboard.h for the contract.
//
// Layout. The reference keyboard-view is a column of key rows whose caps share the row height and divide the row
// width by weight; this is that structure scaled from 800x480 to the 1024x600 content slot (ADR-026) with LVGL
// Flex: one column of rows, each row a flex line of UiActionKey caps with `flex_grow` as the reference weight
// x 20 (lv_obj_set_flex_grow takes a uint8_t, so the weights are integers). Every length is a ui_tokens.h token;
// the 44 px touch floor is enforced by UI_STYLE_BTN_BASE (min_width 44 / min_height 48), which Flex honors as
// the minimum of a growing item.
//
// The key field is US ANSI, not the abbreviated reference row set: 01 outranks `reference/` (which is UX only),
// and FR-201 plus AT-KEY-01 require the punctuation and `[ ]` that the 800 px reference had to drop. The
// reference's own additions are kept: the utility row, the shift sub-legends of the number row and the extra
// Backspace at the end of the bottom letter row.
//
// The number row and the F1-F12 row are both built once and one of them is hidden (Flex skips a hidden item),
// so the FR-207 layer switch neither allocates nor re-creates a cap. A press costs no allocation at all: the cap
// descriptors are a static table and the only per-press work is one ui_actions_submit().
#include "ui_view_keyboard.h"

#include <string.h>

#include <stdio.h>

#include "hid_core.h"
#include "hid_keymap.h"
#include "ui_overlay.h"
#include "ui_theme.h"
#include "ui_toast.h"
#include "ui_tokens.h"

// ---- layout table --------------------------------------------------------------------------
typedef enum {
    ROW_UTIL = 0,
    ROW_NUMBER,
    ROW_FN,
    ROW_TOP,
    ROW_MID,
    ROW_LOW,
    ROW_BOTTOM,
    ROW_COUNT,
} kbd_row_t;

enum { LOCAL_NONE = 0, LOCAL_FN, LOCAL_LANG };

typedef struct {
    ui_action_key_desc_t desc; // handed to ui_action_key_create(); the table outlives every widget
    uint8_t usage;             // page 0x07 usage, or 0 for a local key (F1-F12, the language cap)
    uint8_t local;             // LOCAL_FN / LOCAL_LANG for the local keys
    uint8_t grow;              // reference key weight x 20
    const char *sub;           // shift sub-legend (number row) or the CapsLock indicator slot
    uint8_t row;               // kbd_row_t
    // FR-742: a cap that exists only in some geometry families is created hidden and moved in front of the cap
    // with this usage inside its row, so it sits where that family puts it. 0 for every other cap, which keeps
    // the table order - and therefore every control id - exactly as it was before the layouts landed.
    uint8_t anchor;
} kbd_cap_t;

#define KEY(lbl, icn, usg, var, fnt, g, sb, rw)                                                       \
    {                                                                                                 \
        .desc = {.label = (lbl), .icon = (icn), .variant = (var), .font = (fnt)}, .usage = (usg),     \
        .local = LOCAL_NONE, .grow = (g), .sub = (sb), .row = (rw)                                    \
    }
#define LAYER(lbl, kind, g)                                                                                   \
    {                                                                                                         \
        .desc = {.label = (lbl), .icon = NULL, .variant = UI_KEY_MODIFIER, .font = UI_FONT_LABEL}, .usage = 0u, \
        .local = (kind), .grow = (g), .sub = NULL, .row = ROW_UTIL                                            \
    }
// The language cap (ADR-037). The theme fonts are the built-in Montserrat set, whose symbol range has no globe
// (FontAwesome 0xF0AC is not in it, and neither is U+1F310), so the cap wears the keyboard symbol that is in the
// range and carries the language code as a text label. No font asset is added for one glyph.
#define LANG_KEY(g)                                                                                                \
    {                                                                                                              \
        .desc = {.label = "EN", .icon = LV_SYMBOL_KEYBOARD, .variant = UI_KEY_MODIFIER, .font = UI_FONT_LABEL},    \
        .usage = 0u, .local = LOCAL_LANG, .grow = (g), .sub = NULL, .row = ROW_UTIL                                \
    }

// The usages of the keys that only some geometry families have (HID 1.12 Keyboard page, 10 Keyboard/Keypad).
// hid_keymap.h names the keys the US field needs; these five belong to TASK-071 and are checked against the
// generated tables by test_ui_view_keyboard.c, so a disagreement is a failing test and not a wrong HID report.
#define KBD_USAGE_INT1_RO 0x87u        // AB11 on JIS and ABNT2
#define KBD_USAGE_INT2_KANA 0x88u      // HKTG: Hiragana / Katakana
#define KBD_USAGE_INT3_YEN 0x89u       // AE13 on JIS
#define KBD_USAGE_INT4_HENKAN 0x8Au    // HENK: Henkan (convert)
#define KBD_USAGE_INT5_MUHENKAN 0x8Bu  // MUHE: Muhenkan (no convert)
#define KBD_USAGE_LANG1_HANGUL 0x90u   // HNGL: Hangul / English
#define KBD_USAGE_LANG2_HANJA 0x91u    // HJCV: Hanja conversion

// A geometry cap: the white `normal` look of a character key, created hidden, placed by `anchor` (FR-742).
#define GEOM(lbl, usg, g, rw, anch)                                                                               \
    {                                                                                                             \
        .desc = {.label = (lbl), .icon = NULL, .variant = UI_KEY_NORMAL, .font = UI_FONT_BODY},                   \
        .usage = (usg), .local = LOCAL_NONE, .grow = (g), .sub = NULL, .row = (rw), .anchor = (anch)              \
    }

// Caps that are grey on the reference (`.key.special` / `.utility-key`) use the modifier cap style; the letter,
// digit and punctuation caps are the white `normal` style.
#define UTIL(lbl, usg, g) KEY(lbl, NULL, usg, UI_KEY_MODIFIER, UI_FONT_LABEL, g, NULL, ROW_UTIL)
#define NUM(lbl, usg, sb) KEY(lbl, NULL, usg, UI_KEY_NORMAL, UI_FONT_TITLE, 20, sb, ROW_NUMBER)
// F1-F12 send a key like any letter does, so the layer wears the white `normal` cap: the grey `.key.special`
// look belongs to the modifiers and to the utility row, and on a disabled field it reads as "not available".
#define FN_KEY(lbl, usg) KEY(lbl, NULL, usg, UI_KEY_NORMAL, UI_FONT_BODY, 20, NULL, ROW_FN)
#define LETTER(lbl, usg, rw) KEY(lbl, NULL, usg, UI_KEY_NORMAL, UI_FONT_TITLE, 20, NULL, rw)
#define SPECIAL(lbl, usg, g, rw) KEY(lbl, NULL, usg, UI_KEY_MODIFIER, UI_FONT_BODY, g, NULL, rw)
#define BACKSPACE(g, rw) KEY(NULL, LV_SYMBOL_BACKSPACE, KVM_KEY_BACKSPACE, UI_KEY_MODIFIER, UI_FONT_BODY, g, NULL, rw)
#define ARROW(icn, usg) KEY(NULL, icn, usg, UI_KEY_MODIFIER, UI_FONT_BODY, 20, NULL, ROW_BOTTOM)

// Not const: ui_view_keyboard_init() stamps the control ids once (index -> UI_CTRL_KBD_BASE + index).
static kbd_cap_t s_keys[] = {
    // utility row (reference `util`): Esc, the F1-F12 layer key, the language key, Insert, Delete, Home, End,
    // PgUp, PgDn. The three per-language caps of v1 (KO / ZH / JA) are one cap now (ADR-037); the width they
    // freed went back to the row, which Flex still fills exactly.
    UTIL("Esc", KVM_KEY_ESCAPE, 20),
    LAYER("F1-F12", LOCAL_FN, 28),
    LANG_KEY(28), // ADR-037 FR-746: every language composed on this device is reached from this one cap
    UTIL("Insert", KVM_KEY_INSERT, 21),
    UTIL("Delete", KVM_KEY_DELETE, 21),
    UTIL("Home", KVM_KEY_HOME, 20),
    UTIL("End", KVM_KEY_END, 20),
    UTIL("PgUp", KVM_KEY_PAGE_UP, 20),
    UTIL("PgDn", KVM_KEY_PAGE_DOWN, 20),

    // number row with the reference shift sub-legends (FR-202: the legend is a hint, the usage is the contract)
    NUM("`", KVM_KEY_GRAVE, "~"),
    NUM("1", KVM_KEY_1 + 0, "!"),
    NUM("2", KVM_KEY_1 + 1, "@"),
    NUM("3", KVM_KEY_1 + 2, "#"),
    NUM("4", KVM_KEY_1 + 3, "$"),
    NUM("5", KVM_KEY_1 + 4, "%"),
    NUM("6", KVM_KEY_1 + 5, "^"),
    NUM("7", KVM_KEY_1 + 6, "&"),
    NUM("8", KVM_KEY_1 + 7, "*"),
    NUM("9", KVM_KEY_1 + 8, "("),
    NUM("0", KVM_KEY_0, ")"),
    NUM("-", KVM_KEY_MINUS, "_"),
    NUM("=", KVM_KEY_EQUAL, "+"),
    BACKSPACE(36, ROW_NUMBER),

    // F1-F12 layer: replaces the number row only, never covers the main field (FR-207)
    FN_KEY("F1", KVM_KEY_F1 + 0),
    FN_KEY("F2", KVM_KEY_F1 + 1),
    FN_KEY("F3", KVM_KEY_F1 + 2),
    FN_KEY("F4", KVM_KEY_F1 + 3),
    FN_KEY("F5", KVM_KEY_F1 + 4),
    FN_KEY("F6", KVM_KEY_F1 + 5),
    FN_KEY("F7", KVM_KEY_F1 + 6),
    FN_KEY("F8", KVM_KEY_F1 + 7),
    FN_KEY("F9", KVM_KEY_F1 + 8),
    FN_KEY("F10", KVM_KEY_F1 + 9),
    FN_KEY("F11", KVM_KEY_F1 + 10),
    FN_KEY("F12", KVM_KEY_F12),
    BACKSPACE(24, ROW_FN),

    // QWERTY
    SPECIAL("Tab", KVM_KEY_TAB, 26, ROW_TOP),
    LETTER("Q", 0x14, ROW_TOP),
    LETTER("W", 0x1A, ROW_TOP),
    LETTER("E", 0x08, ROW_TOP),
    LETTER("R", 0x15, ROW_TOP),
    LETTER("T", 0x17, ROW_TOP),
    LETTER("Y", 0x1C, ROW_TOP),
    LETTER("U", 0x18, ROW_TOP),
    LETTER("I", 0x0C, ROW_TOP),
    LETTER("O", 0x12, ROW_TOP),
    LETTER("P", 0x13, ROW_TOP),
    LETTER("[", KVM_KEY_LEFT_BRACKET, ROW_TOP),
    LETTER("]", KVM_KEY_RIGHT_BRACKET, ROW_TOP),
    LETTER("\\", KVM_KEY_BACKSLASH, ROW_TOP),

    // CapsLock is an ordinary toggling key on the HOST (FR-206); `sub` is its LED indicator slot
    KEY("Caps", NULL, KVM_KEY_CAPS_LOCK, UI_KEY_MODIFIER, UI_FONT_BODY, 32, "Unknown", ROW_MID),
    LETTER("A", KVM_KEY_A, ROW_MID),
    LETTER("S", 0x16, ROW_MID),
    LETTER("D", 0x07, ROW_MID),
    LETTER("F", 0x09, ROW_MID),
    LETTER("G", 0x0A, ROW_MID),
    LETTER("H", 0x0B, ROW_MID),
    LETTER("J", 0x0D, ROW_MID),
    LETTER("K", 0x0E, ROW_MID),
    LETTER("L", 0x0F, ROW_MID),
    LETTER(";", KVM_KEY_SEMICOLON, ROW_MID),
    LETTER("'", KVM_KEY_APOSTROPHE, ROW_MID),
    SPECIAL("Enter", KVM_KEY_ENTER, 40, ROW_MID),

    SPECIAL("Shift", KVM_KEY_LEFT_SHIFT, 36, ROW_LOW),
    LETTER("Z", KVM_KEY_Z, ROW_LOW),
    LETTER("X", 0x1B, ROW_LOW),
    LETTER("C", 0x06, ROW_LOW),
    LETTER("V", 0x19, ROW_LOW),
    LETTER("B", 0x05, ROW_LOW),
    LETTER("N", 0x11, ROW_LOW),
    LETTER("M", 0x10, ROW_LOW),
    LETTER(",", KVM_KEY_COMMA, ROW_LOW),
    LETTER(".", KVM_KEY_PERIOD, ROW_LOW),
    LETTER("/", KVM_KEY_SLASH, ROW_LOW),
    BACKSPACE(30, ROW_LOW),

    SPECIAL("Ctrl", KVM_KEY_LEFT_CTRL, 22, ROW_BOTTOM),
    SPECIAL("Alt", KVM_KEY_LEFT_ALT, 22, ROW_BOTTOM),
    SPECIAL("Win", KVM_KEY_LEFT_GUI, 22, ROW_BOTTOM), // FR-210: v1 defaults to the PC/Windows legend
    KEY("space", NULL, KVM_KEY_SPACE, UI_KEY_NORMAL, UI_FONT_BODY, 83, NULL, ROW_BOTTOM),
    SPECIAL("AltGr", KVM_KEY_RIGHT_ALT, 23, ROW_BOTTOM), // 03 §3 rule 1: AltGr is RightAlt 0xE6
    ARROW(LV_SYMBOL_LEFT, KVM_KEY_LEFT),
    ARROW(LV_SYMBOL_UP, KVM_KEY_UP),
    ARROW(LV_SYMBOL_DOWN, KVM_KEY_DOWN),
    ARROW(LV_SYMBOL_RIGHT, KVM_KEY_RIGHT),

    // ---- the geometry families (FR-742) ------------------------------------------------------
    // Appended, never inserted: a cap's control id is UI_CTRL_KBD_BASE + its table index, so the US field above
    // keeps the ids it has always had and `anchor` alone decides where these caps appear in their row. Every one
    // of them is hidden until a layout whose family has it is selected, and its legends come from that layout
    // (the IME keys carry none, so they wear the name of the key instead - FR-743: the legend is not the keymap).
    GEOM("<", KVM_KEY_NON_US_BACKSLASH, 20, ROW_LOW, KVM_KEY_Z),  // LSGT: ISO and ABNT2
    GEOM("\\", KBD_USAGE_INT3_YEN, 20, ROW_NUMBER, KVM_KEY_BACKSPACE), // AE13 (Yen): JIS
    GEOM("\\", KBD_USAGE_INT1_RO, 20, ROW_LOW, KVM_KEY_BACKSPACE),     // AB11 (Ro): JIS and ABNT2
    // Wider than the modifier caps next to them: the legends are words, and a clipped "Muhenka" names no key.
    GEOM("Muhenkan", KBD_USAGE_INT5_MUHENKAN, 42, ROW_BOTTOM, KVM_KEY_SPACE),
    GEOM("Henkan", KBD_USAGE_INT4_HENKAN, 30, ROW_BOTTOM, KVM_KEY_RIGHT_ALT),
    GEOM("Kana", KBD_USAGE_INT2_KANA, 26, ROW_BOTTOM, KVM_KEY_RIGHT_ALT),
    GEOM("Han/Eng", KBD_USAGE_LANG1_HANGUL, 38, ROW_BOTTOM, KVM_KEY_RIGHT_ALT), // KR
    GEOM("Hanja", KBD_USAGE_LANG2_HANJA, 30, ROW_BOTTOM, KVM_KEY_SPACE),        // KR
};

#define KEY_COUNT ((uint32_t)(sizeof(s_keys) / sizeof(s_keys[0])))
_Static_assert(sizeof(s_keys) / sizeof(s_keys[0]) <= UI_VIEW_KBD_KEY_CAP, "ui_view_keyboard: cap array too small");
_Static_assert(UI_CTRL_KBD_BASE + sizeof(s_keys) / sizeof(s_keys[0]) <= 0xFFFFu, "ui_view_keyboard: control id range");

#define UTIL_ROW_H UI_TOUCH_PREFERRED // reference `.keyrow.utility` is a fixed, shorter row

// ---- small helpers -------------------------------------------------------------------------
uint32_t ui_view_keyboard_key_count(void)
{
    return KEY_COUNT;
}

uint8_t ui_view_keyboard_key_usage(uint32_t idx)
{
    return idx < KEY_COUNT ? s_keys[idx].usage : 0u;
}

const char *ui_view_keyboard_key_name(uint32_t idx)
{
    if (idx >= KEY_COUNT) {
        return "?";
    }
    if (s_keys[idx].local == LOCAL_LANG) {
        return "Lang"; // its legend follows the language: a lookup by legend would not be reproducible
    }
    if (s_keys[idx].desc.label != NULL) {
        return s_keys[idx].desc.label;
    }
    return s_keys[idx].desc.icon != NULL ? s_keys[idx].desc.icon : "?";
}

lv_obj_t *ui_view_keyboard_key_obj(const ui_view_keyboard_t *k, uint32_t idx)
{
    return (k != NULL && idx < KEY_COUNT) ? k->keys[idx] : NULL;
}

static bool host_mounted(const ui_view_keyboard_t *k)
{
    return ui_store_get(k->store, UI_SUBJ_HOST_USB) == (int32_t)KVM_HOST_USB_MOUNTED;
}

// ---- HOST layout (FR-741/742/743) ----------------------------------------------------------
// A cap is tied to a key slot by its usage, not by its table index: the usages of the US field are unique, so the
// map below survives every reordering of the table and needs no second list to keep in step with it. The usage a
// cap *sends* still comes from the selected layout (`usage_live`), which is the only thing a layout changes about
// behavior; the legends it draws are decoration (FR-743).
static const struct {
    uint8_t usage;
    uint8_t slot;
} k_slot_of_usage[] = {
    {KVM_KEY_GRAVE, KBD_SLOT_TLDE},
    {KVM_KEY_1 + 0, KBD_SLOT_AE01}, {KVM_KEY_1 + 1, KBD_SLOT_AE02}, {KVM_KEY_1 + 2, KBD_SLOT_AE03},
    {KVM_KEY_1 + 3, KBD_SLOT_AE04}, {KVM_KEY_1 + 4, KBD_SLOT_AE05}, {KVM_KEY_1 + 5, KBD_SLOT_AE06},
    {KVM_KEY_1 + 6, KBD_SLOT_AE07}, {KVM_KEY_1 + 7, KBD_SLOT_AE08}, {KVM_KEY_1 + 8, KBD_SLOT_AE09},
    {KVM_KEY_0, KBD_SLOT_AE10}, {KVM_KEY_MINUS, KBD_SLOT_AE11}, {KVM_KEY_EQUAL, KBD_SLOT_AE12},
    {KBD_USAGE_INT3_YEN, KBD_SLOT_AE13},
    {0x14, KBD_SLOT_AD01}, {0x1A, KBD_SLOT_AD02}, {0x08, KBD_SLOT_AD03}, {0x15, KBD_SLOT_AD04},
    {0x17, KBD_SLOT_AD05}, {0x1C, KBD_SLOT_AD06}, {0x18, KBD_SLOT_AD07}, {0x0C, KBD_SLOT_AD08},
    {0x12, KBD_SLOT_AD09}, {0x13, KBD_SLOT_AD10},
    {KVM_KEY_LEFT_BRACKET, KBD_SLOT_AD11}, {KVM_KEY_RIGHT_BRACKET, KBD_SLOT_AD12},
    {KVM_KEY_BACKSLASH, KBD_SLOT_BKSL},
    {KVM_KEY_A, KBD_SLOT_AC01}, {0x16, KBD_SLOT_AC02}, {0x07, KBD_SLOT_AC03}, {0x09, KBD_SLOT_AC04},
    {0x0A, KBD_SLOT_AC05}, {0x0B, KBD_SLOT_AC06}, {0x0D, KBD_SLOT_AC07}, {0x0E, KBD_SLOT_AC08},
    {0x0F, KBD_SLOT_AC09}, {KVM_KEY_SEMICOLON, KBD_SLOT_AC10}, {KVM_KEY_APOSTROPHE, KBD_SLOT_AC11},
    {KVM_KEY_NON_US_BACKSLASH, KBD_SLOT_LSGT},
    {KVM_KEY_Z, KBD_SLOT_AB01}, {0x1B, KBD_SLOT_AB02}, {0x06, KBD_SLOT_AB03}, {0x19, KBD_SLOT_AB04},
    {0x05, KBD_SLOT_AB05}, {0x11, KBD_SLOT_AB06}, {0x10, KBD_SLOT_AB07},
    {KVM_KEY_COMMA, KBD_SLOT_AB08}, {KVM_KEY_PERIOD, KBD_SLOT_AB09}, {KVM_KEY_SLASH, KBD_SLOT_AB10},
    {KBD_USAGE_INT1_RO, KBD_SLOT_AB11},
    {KBD_USAGE_INT5_MUHENKAN, KBD_SLOT_MUHE}, {KBD_USAGE_INT4_HENKAN, KBD_SLOT_HENK},
    {KBD_USAGE_INT2_KANA, KBD_SLOT_HKTG},
    {KBD_USAGE_LANG1_HANGUL, KBD_SLOT_HNGL}, {KBD_USAGE_LANG2_HANJA, KBD_SLOT_HJCV},
};
#define SLOT_MAP_COUNT ((uint32_t)(sizeof(k_slot_of_usage) / sizeof(k_slot_of_usage[0])))

/** The slot of cap `idx`, or KBD_SLOT_COUNT when it owns none. SPCE is deliberately absent: the space bar carries
 *  no legend and its usage is the same in every family, so there is nothing for a layout to change. */
kbd_slot_t ui_view_keyboard_key_slot(uint32_t idx)
{
    if (idx >= KEY_COUNT || s_keys[idx].local != LOCAL_NONE) {
        return KBD_SLOT_COUNT;
    }
    for (uint32_t i = 0; i < SLOT_MAP_COUNT; i++) {
        if (k_slot_of_usage[i].usage == s_keys[idx].usage) {
            return (kbd_slot_t)k_slot_of_usage[i].slot;
        }
    }
    return KBD_SLOT_COUNT;
}

/** A cap that only some families have is the one with an `anchor`: it is hidden unless the layout defines its slot. */
static bool geometry_cap(uint32_t idx)
{
    return idx < KEY_COUNT && s_keys[idx].anchor != 0u;
}

/** TECH-078: the legend, or the theme's placeholder when the font `label` draws with has no glyph for it. */
static const char *drawable(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return text;
    }
    return ui_theme_drawable(lv_obj_get_style_text_font(label, LV_PART_MAIN), text);
}

/** The single code point of `s`, or 0 when `s` is empty, malformed or longer than one code point. */
static uint32_t one_cp(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp;
    size_t n;
    if (p[0] < 0x80u) {
        cp = p[0];
        n = 1u;
    } else if ((p[0] & 0xE0u) == 0xC0u) {
        cp = (uint32_t)(p[0] & 0x1Fu) << 6;
        n = 2u;
    } else if ((p[0] & 0xF0u) == 0xE0u) {
        cp = (uint32_t)(p[0] & 0x0Fu) << 12;
        n = 3u;
    } else if ((p[0] & 0xF8u) == 0xF0u) {
        cp = (uint32_t)(p[0] & 0x07u) << 18;
        n = 4u;
    } else {
        return 0u;
    }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            return 0u;
        }
        cp |= (uint32_t)(p[i] & 0x3Fu) << (6u * (n - 1u - i));
    }
    return (cp != 0u && p[n] == '\0') ? cp : 0u;
}

/**
 * Whether `shift` is just the upper case of `base`, which is what a real keycap prints as one legend instead of
 * two. Only the cased scripts of the legend tables are mapped - Latin, Greek, Cyrillic; everywhere else a layout
 * either repeats the Base level or puts something unrelated on Shift, and the cap then shows both levels.
 */
static bool case_pair(const char *base, const char *shift)
{
    if (base == NULL || shift == NULL || base[0] == '\0' || shift[0] == '\0') {
        return false;
    }
    const uint32_t b = one_cp(base);
    const uint32_t s = one_cp(shift);
    if (b == 0u || s == 0u) {
        return false; // more than one code point: not a letter cap
    }
    if (b >= 'a' && b <= 'z') {
        return s == b - 0x20u;
    }
    if ((b >= 0xE0u && b <= 0xFEu && b != 0xF7u) || (b >= 0x3B1u && b <= 0x3C9u) ||
        (b >= 0x430u && b <= 0x44Fu)) {
        return s == b - 0x20u; // Latin-1, Greek and basic Cyrillic all keep the 0x20 distance
    }
    if (b >= 0x450u && b <= 0x45Fu) {
        return s == b - 0x50u; // Cyrillic ё and the other extras
    }
    return false;
}

static void set_sub(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    text = drawable(label, text);
    lv_label_set_text_static(label, text); // the legend pool lives in flash: no copy, no allocation
    lv_obj_set_hidden(label, text[0] == '\0');
}

/** The main legend. Not ui_action_key_set_label(): that one takes the cap's last child, and the Shift and AltGr
 *  corners this view adds are created after it. The pointer is kept from make_key() instead. */
static void set_legend(ui_view_keyboard_t *k, uint32_t idx, const char *text)
{
    if (k->cap_label[idx] != NULL && text != NULL) {
        lv_label_set_text_static(k->cap_label[idx], drawable(k->cap_label[idx], text));
    }
}

/**
 * Draw the whole field for `k->layout` (NULL: the US ANSI table this file carries). Called when the view is built
 * and when Settings changes the layout; it allocates nothing - every label already exists - and it never moves a
 * cap that is not a geometry cap, except the one ISO rule below.
 */
/** The short id the strip shows: the base of the xkb id, upper case, without its variant ("fr(oss)" -> "FR"). */
static void strip_short_id(const kbd_layout_t *lay, char *out, size_t cap)
{
    out[0] = '\0';
    if (lay == NULL || cap == 0u) {
        return;
    }
    size_t n = 0;
    for (const char *p = lay->id; *p != '\0' && *p != '(' && n + 1u < cap; p++) {
        out[n++] = (char)((*p >= 'a' && *p <= 'z') ? (*p - 'a' + 'A') : *p);
    }
    out[n] = '\0';
}

static void apply_layout(ui_view_keyboard_t *k)
{
    if (k->root == NULL) {
        return;
    }
    const kbd_layout_t *lay = k->layout;
    const bool iso = lay != NULL && (lay->geometry == KBD_GEOM_ISO);
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        if (k->keys[i] == NULL) {
            continue;
        }
        const kbd_slot_t slot = ui_view_keyboard_key_slot(i);
        if (slot == KBD_SLOT_COUNT) {
            continue; // Esc, the modifiers, the arrows, Enter, Tab, the local keys: no layout has a say here
        }
        const kbd_key_t *key = lay != NULL ? kbd_layouts_key(lay, slot) : NULL;
        if (geometry_cap(i)) {
            lv_obj_set_hidden(k->keys[i], key == NULL);
        }
        if (key == NULL) {
            // No layout, or a family without this key: back to the legends the table carries.
            k->usage_live[i] = 0u;
            set_legend(k, i, s_keys[i].desc.label);
            set_sub(k->sub[i], s_keys[i].sub != NULL ? s_keys[i].sub : "");
            set_sub(k->alt[i], "");
            continue;
        }
        k->usage_live[i] = key->usage;
        const char *base = kbd_layouts_legend(key, 0);
        const char *shift = kbd_layouts_legend(key, 1);
        const char *altgr = kbd_layouts_legend(key, 2);
        if (base[0] == '\0' && shift[0] == '\0') {
            // An IME key, or a slot the layout leaves blank: keep the name of the key (FR-743 - it is a label,
            // not a character this cap would type).
            set_legend(k, i, s_keys[i].desc.label);
            set_sub(k->sub[i], "");
        } else if (case_pair(base, shift)) {
            set_legend(k, i, shift); // one legend, as a letter cap is printed
            set_sub(k->sub[i], "");
        } else {
            set_legend(k, i, base[0] != '\0' ? base : shift);
            set_sub(k->sub[i], (base[0] != '\0' && strcmp(base, shift) != 0) ? shift : "");
        }
        set_sub(k->alt[i], altgr);
    }
    // ISO Enter: `#` leaves the end of the QWERTY row and takes its place next to Enter in the home row, which is
    // the shape every ISO board has. Nothing else about the two rows changes, and ANSI puts it back.
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        if (k->keys[i] == NULL || s_keys[i].usage != KVM_KEY_BACKSLASH) {
            continue;
        }
        lv_obj_t *want = iso ? k->row_mid : k->row_top;
        if (lv_obj_get_parent(k->keys[i]) != want) {
            lv_obj_set_parent(k->keys[i], want);
        }
        if (iso) {
            for (uint32_t j = 0; j < KEY_COUNT; j++) { // in front of Enter
                if (k->keys[j] != NULL && s_keys[j].usage == KVM_KEY_ENTER) {
                    lv_obj_move_to_index(k->keys[i], (int32_t)lv_obj_get_index(k->keys[j]));
                    break;
                }
            }
        }
        break;
    }
    if (k->hint != NULL) {
        if (lay != NULL) {
            // The device cannot read the HOST's own setting, so it says which layout it is drawing and asks for
            // the same one on the other side (FR-742). Nothing here is a promise about the HOST.
            snprintf(k->hint_text, sizeof(k->hint_text), "%s - set the same layout on the HOST", lay->name);
        } else {
            k->hint_text[0] = '\0';
        }
        // The strip carries the short id ("DE") and the name; the full FR-742 sentence stays the accessor's
        // text and is read out by the quick switch's own note, so nothing has to be clipped on the row.
        strip_short_id(lay, k->strip_code_text, sizeof(k->strip_code_text));
        snprintf(k->strip_name_text, sizeof(k->strip_name_text), "%s", lay != NULL ? lay->name : "");
        if (k->strip_code != NULL) {
            lv_label_set_text_static(k->strip_code, k->strip_code_text);
        }
        lv_obj_set_hidden(k->hint, lay == NULL);
    }
    k->c.relabels++;
}

void ui_view_keyboard_set_host_layout(ui_view_keyboard_t *k, const char *id)
{
    if (k == NULL) {
        return;
    }
    if (id == NULL || id[0] == '\0') {
        k->layout_id[0] = '\0';
        k->layout = NULL;
        apply_layout(k);
        return;
    }
    snprintf(k->layout_id, sizeof(k->layout_id), "%s", id);
    const kbd_layout_t *found = kbd_layouts_find(k->layout_id);
    if (found == NULL) {
        k->c.layout_unknown++; // an id this build does not ship: the field stays as it is, nothing is invented
        return;
    }
    k->layout = found;
    k->c.layout_sets++;
    apply_layout(k);
}

const char *ui_view_keyboard_host_layout(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->layout_id : "";
}

const kbd_layout_t *ui_view_keyboard_layout(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->layout : NULL;
}

kbd_geometry_t ui_view_keyboard_geometry(const ui_view_keyboard_t *k)
{
    return (k != NULL && k->layout != NULL) ? (kbd_geometry_t)k->layout->geometry : KBD_GEOM_ANSI;
}

/** What cap `idx` sends: the layout's usage for its slot, else the table's own. */
static uint8_t live_usage(const ui_view_keyboard_t *k, uint32_t idx)
{
    if (idx >= KEY_COUNT) {
        return 0u;
    }
    return k->usage_live[idx] != 0u ? k->usage_live[idx] : s_keys[idx].usage;
}

uint8_t ui_view_keyboard_key_live_usage(const ui_view_keyboard_t *k, uint32_t idx)
{
    return k != NULL ? live_usage(k, idx) : 0u;
}

bool ui_view_keyboard_key_shown(const ui_view_keyboard_t *k, uint32_t idx)
{
    if (k == NULL || idx >= KEY_COUNT || k->keys[idx] == NULL) {
        return false;
    }
    return !lv_obj_is_hidden(k->keys[idx]);
}

const char *ui_view_keyboard_key_legend(const ui_view_keyboard_t *k, uint32_t idx, unsigned level)
{
    if (k == NULL || idx >= KEY_COUNT || k->keys[idx] == NULL) {
        return NULL;
    }
    if (level == 0u) {
        return k->cap_label[idx] != NULL ? lv_label_get_text(k->cap_label[idx]) : "";
    }
    lv_obj_t *lab = level == 1u ? k->sub[idx] : (level == 2u ? k->alt[idx] : NULL);
    if (lab == NULL) {
        return "";
    }
    return lv_obj_is_hidden(lab) ? "" : lv_label_get_text(lab);
}

const char *ui_view_keyboard_hint_text(const ui_view_keyboard_t *k)
{
    if (k == NULL || k->hint == NULL || lv_obj_is_hidden(k->hint)) {
        return "";
    }
    // The strip is a control, not a label: the FR-742 sentence lives in the view's own buffer, and the strip
    // shows its short form (icon, id, name, chevron).
    return k->hint_text;
}

// ---- input languages (ADR-037) -------------------------------------------------------------
// The language in effect is not stored: it is read back from the layers that already carry it, so the cap and the
// composition can never disagree. Switching goes through set_hangul() / set_conv(), which own the commit, cancel
// and release-held rules; this layer only decides which language is next.
static const char *const k_lang_code[UI_KBD_LANG_COUNT] = {"EN", "KO", "ZH", "JA"};
static const char *const k_lang_name[UI_KBD_LANG_COUNT] = {"English", "Korean", "Chinese (pinyin)", "Japanese"};

const char *ui_view_keyboard_lang_code(ui_kbd_lang_t lang)
{
    return (unsigned)lang < UI_KBD_LANG_COUNT ? k_lang_code[lang] : "?";
}

const char *ui_view_keyboard_lang_name(ui_kbd_lang_t lang)
{
    return (unsigned)lang < UI_KBD_LANG_COUNT ? k_lang_name[lang] : "?";
}

static ui_kbd_lang_t lang_of(const ui_view_keyboard_t *k)
{
    if (k->hangul) {
        return UI_KBD_LANG_KO;
    }
    if (k->converting) {
        return k->conv_layer == UI_KBD_LAYER_JA ? UI_KBD_LANG_JA : UI_KBD_LANG_ZH;
    }
    return UI_KBD_LANG_EN;
}

/** Why `lang` cannot be selected right now, or NULL when it can. English and the current language always can. */
static const char *lang_block(const ui_view_keyboard_t *k, ui_kbd_lang_t lang)
{
    if ((unsigned)lang >= UI_KBD_LANG_COUNT) {
        return "Not available";
    }
    if (lang == UI_KBD_LANG_EN || lang == lang_of(k)) {
        return NULL; // going back to English is always allowed, and so is staying where we are
    }
    if ((k->langs & UI_KBD_LANG_BIT(lang)) == 0u) {
        return "Off in Settings";
    }
    if (!k->text_mode_ok) {
        return "Needs a Unicode delivery mode"; // FR-747: host-ime composes on the HOST instead
    }
    if (lang == UI_KBD_LANG_ZH && k->zh == NULL) {
        return "No pinyin decoder installed";
    }
    if (lang == UI_KBD_LANG_JA && k->ja == NULL) {
        return "No Japanese decoder installed";
    }
    return NULL;
}

ui_kbd_lang_t ui_view_keyboard_lang(const ui_view_keyboard_t *k)
{
    return k != NULL ? lang_of(k) : UI_KBD_LANG_EN;
}

bool ui_view_keyboard_lang_available(const ui_view_keyboard_t *k, ui_kbd_lang_t lang)
{
    return k != NULL && lang_block(k, lang) == NULL;
}

const char *ui_view_keyboard_lang_reason(const ui_view_keyboard_t *k, ui_kbd_lang_t lang)
{
    return k != NULL ? lang_block(k, lang) : "Not available";
}

uint8_t ui_view_keyboard_languages(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->langs : 0u;
}

lv_obj_t *ui_view_keyboard_lang_key(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->lang_key : NULL;
}

lv_obj_t *ui_view_keyboard_lang_pick_button(const ui_view_keyboard_t *k, ui_kbd_lang_t lang)
{
    return (k != NULL && (unsigned)lang < UI_KBD_LANG_COUNT) ? k->pick_btn[lang] : NULL;
}

bool ui_view_keyboard_picker_open(const ui_view_keyboard_t *k)
{
    return k != NULL && k->lang_card != NULL;
}

const char *ui_view_keyboard_toast_text(const ui_view_keyboard_t *k)
{
    return (k != NULL && k->toast != NULL) ? k->toast_text : "";
}

static uint64_t now(const ui_view_keyboard_t *k)
{
    return k->now_us != NULL ? k->now_us() : 0u;
}

static bool submit(ui_view_keyboard_t *k, ui_action_type_t type, uint16_t ctrl, uint16_t value)
{
    if (k->actions == NULL) {
        k->c.refused++;
        return false;
    }
    const bool ok = ui_actions_submit(k->actions, type, ctrl, value, now(k), NULL) != 0u;
    if (!ok) {
        k->c.refused++;
    }
    return ok;
}

// The UP owed for an accepted DOWN. Submitted even when the sink may refuse it: the HID core stays the safety
// owner (RELEASE_ALL on every loss), the local flag must never outlive the press (AGENTS.md prohibition 3).
static void release_held(ui_view_keyboard_t *k, bool from_leave)
{
    if (k->held >= KEY_COUNT) {
        k->held = UI_VIEW_KBD_NO_KEY;
        return;
    }
    const kbd_cap_t *e = &s_keys[k->held];
    const uint8_t usage = live_usage(k, k->held); // the same usage that went DOWN, layout and all
    k->held = UI_VIEW_KBD_NO_KEY;
    k->c.key_up++;
    if (from_leave) {
        k->c.leave_releases++;
    }
    (void)submit(k, UI_ACT_KEY_UP, e->desc.control_id, usage);
}

static void update_preedit(ui_view_keyboard_t *k);
static void zh_clear(ui_view_keyboard_t *k);
static void zh_render(ui_view_keyboard_t *k);
static void zh_update(ui_view_keyboard_t *k);

// ---- state projection ----------------------------------------------------------------------
// Three distinct looks for FR-204. OFF is the plain modifier cap, ARMED the shared checked style of the theme
// and LOCKED a local accent style bound to LV_STATE_USER_2 (UiActionKey owns USER_1 for its down flag). The two
// are mutually exclusive, so the cap can never show both.
static void apply_mod_look(lv_obj_t *key, bool armed, bool locked)
{
    ui_action_key_set_checked(key, armed && !locked);
    lv_obj_set_state_user_2(key, locked);
}

static void style_locked_look(lv_obj_t *key)
{
    lv_obj_set_style_bg_color(key, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_USER_2);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, LV_STATE_USER_2);
    lv_obj_set_style_border_color(key, lv_color_hex(UI_COLOR_ACCENT_INK), LV_STATE_USER_2);
    lv_obj_set_style_border_width(key, UI_BORDER_W_BOLD, LV_STATE_USER_2);
    lv_obj_set_style_text_color(key, lv_color_hex(UI_COLOR_ON_INVERSE), LV_STATE_USER_2);
}

static void refresh(ui_view_keyboard_t *k)
{
    if (k->root == NULL) {
        return;
    }
    const bool host = host_mounted(k);
    if (host != k->keys_enabled) {
        if (!host) {
            // The HOST is gone: the owed UP goes out now, as a forced release. Disabling the cap afterwards
            // still emits its own release edge, but the view no longer holds anything, so there is one UP.
            release_held(k, true);
        }
        for (uint32_t i = 0; i < KEY_COUNT; i++) {
            ui_action_key_set_enabled(k->keys[i], host);
        }
        k->keys_enabled = host;
    }
    if (k->offline != NULL) {
        lv_obj_set_hidden(k->offline, host);
    }
    if (!host && k->hangul) {
        kvm_hangul_reset(&k->ime); // nothing composed waits for a HOST that is gone
        update_preedit(k);
    }
    if (!host && k->converting && k->py_len > 0u) {
        zh_clear(k); // nothing composed waits for a HOST that is gone
        zh_render(k);
    }
    const bool composing = k->hangul || k->converting;
    const ui_kbd_lang_t lang = lang_of(k);
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        const uint8_t u = s_keys[i].usage;
        if (s_keys[i].local == LOCAL_LANG) {
            // Enabled whenever the HOST is there, even with English alone turned on: a tap then says where the
            // other languages are instead of doing nothing (ADR-037).
            ui_action_key_set_enabled(k->keys[i], host);
            ui_action_key_set_checked(k->keys[i], lang != UI_KBD_LANG_EN);
            ui_action_key_set_label(k->keys[i], ui_view_keyboard_lang_code(lang));
        } else if (composing && (u == KVM_KEY_CAPS_LOCK || (kvm_usage_is_modifier(u) && u != KVM_KEY_LEFT_SHIFT))) {
            ui_action_key_set_enabled(k->keys[i], false); // FR-746: composition takes plain characters only
        } else if (!composing && host && (u == KVM_KEY_CAPS_LOCK || kvm_usage_is_modifier(u))) {
            ui_action_key_set_enabled(k->keys[i], true);
        }
    }
    for (uint32_t i = 0; i < UI_VIEW_KBD_CANDS; i++) {
        ui_action_key_set_enabled(k->zh_cand[i], host && i + k->zh_page < k->zh_count);
    }
    ui_action_key_set_enabled(k->zh_prev, host && k->zh_page > 0u);
    if (k->flick_toggle != NULL) {
        ui_action_key_set_enabled(k->flick_toggle, host);
    }
    for (uint32_t g = 0; g < UI_VIEW_KBD_FLICK_KEYS; g++) {
        if (k->flick_key[g] == NULL) {
            continue;
        }
        if (g % 4u == 3u) {
            ui_action_key_set_enabled(k->flick_key[g], host);
        } else {
            lv_obj_set_state(k->flick_key[g], LV_STATE_DISABLED, !host);
        }
    }
    ui_action_key_set_enabled(k->zh_next, host && (uint32_t)k->zh_page + UI_VIEW_KBD_CANDS < k->zh_count);

    const int32_t armed_raw = ui_store_get(k->store, UI_SUBJ_MODS_ARMED);
    const int32_t locked_raw = ui_store_get(k->store, UI_SUBJ_MODS_LOCKED);
    const uint8_t armed = armed_raw > 0 ? (uint8_t)armed_raw : 0u; // UNKNOWN is never read as "armed"
    const uint8_t locked = locked_raw > 0 ? (uint8_t)locked_raw : 0u;
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        const uint8_t usage = s_keys[i].usage;
        if (usage == 0u || !kvm_usage_is_modifier(usage)) {
            continue;
        }
        const uint8_t bit = KVM_MOD_BIT(usage);
        if (composing) {
            // A layer's Shift is local; no HID modifier can be armed while one is on (TEXT_CANCEL released).
            const bool local_shift = k->hangul ? k->jamo_shift : k->zh_shift;
            apply_mod_look(k->keys[i], usage == KVM_KEY_LEFT_SHIFT && local_shift, false);
            continue;
        }
        apply_mod_look(k->keys[i], (armed & bit) != 0u, (locked & bit) != 0u);
    }

    // FR-206: the indicator is the HOST LED report, or the word "Unknown" - never a guess.
    const int32_t caps = ui_store_get(k->store, UI_SUBJ_CAPS_LED);
    if (k->caps_sub != NULL) {
        lv_label_set_text_static(k->caps_sub, caps == UI_STORE_UNKNOWN ? "Unknown" : (caps == 1 ? "On" : "Off"));
    }
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        if (s_keys[i].usage == KVM_KEY_CAPS_LOCK) {
            ui_action_key_set_checked(k->keys[i], caps == 1);
        }
    }
}

static void subject_cb(lv_observer_t *obs, lv_subject_t *subj)
{
    (void)subj;
    refresh(lv_observer_get_user_data(obs));
}

// ---- KO composition layer (ADR-024 FR-746) ------------------------------------------------------
static size_t utf8_of(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    out[0] = (char)(0xE0u | (cp >> 12));
    out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
}

static void update_preedit(ui_view_keyboard_t *k)
{
    const uint32_t cp = kvm_hangul_preedit(&k->ime);
    size_t n = cp != 0u ? utf8_of(cp, k->preedit_text) : 0u;
    k->preedit_text[n] = '\0';
    if (k->preedit != NULL) {
        lv_label_set_text_static(k->preedit, k->preedit_text);
    }
}

static void submit_text(ui_view_keyboard_t *k, const uint32_t *cps, size_t n, uint16_t ctrl)
{
    for (size_t i = 0; i < n; i++) {
        if (cps[i] <= 0xFFFFu && submit(k, UI_ACT_TEXT_CHAR, ctrl, (uint16_t)cps[i])) {
            k->c.text_chars++;
        }
    }
}

static void commit_preedit(ui_view_keyboard_t *k, uint16_t ctrl)
{
    uint32_t out[1];
    submit_text(k, out, kvm_hangul_flush(&k->ime, out), ctrl);
    update_preedit(k);
}

static void text_cancel(ui_view_keyboard_t *k, uint16_t ctrl)
{
    k->c.text_cancels++;
    (void)submit(k, UI_ACT_TEXT_CANCEL, ctrl, 0u);
}

static void set_conv(ui_view_keyboard_t *k, uint8_t layer, uint16_t ctrl);

static void set_hangul(ui_view_keyboard_t *k, bool on, uint16_t ctrl)
{
    if (k->hangul == on) {
        return;
    }
    release_held(k, true);
    if (on) {
        if (k->converting) {
            set_conv(k, 0u, ctrl); // its text is committed; no modifier could be armed inside it
        } else {
            text_cancel(k, ctrl); // FR-748: no armed modifier, no queued key may reach the composed text
        }
        kvm_hangul_reset(&k->ime);
    } else {
        commit_preedit(k, ctrl); // the syllable on screen is what the user meant to type
    }
    k->hangul = on;
    k->jamo_shift = false;
    update_preedit(k);
    if (k->row_compose != NULL) {
        lv_obj_set_hidden(k->row_compose, !on);
    }
    refresh(k);
}

static void hangul_key(ui_view_keyboard_t *k, uint32_t idx, uint16_t ctrl)
{
    const uint8_t usage = s_keys[idx].usage;
    if (usage == KVM_KEY_LEFT_SHIFT) {
        k->jamo_shift = !k->jamo_shift;
        refresh(k);
        return;
    }
    if (kvm_usage_is_modifier(usage) || usage == KVM_KEY_CAPS_LOCK) {
        return; // disabled in the layer; a stray edge does nothing
    }
    if (usage >= KVM_KEY_A && usage <= KVM_KEY_Z) {
        const uint32_t jamo = kvm_hangul_2set_jamo((char)('a' + (usage - KVM_KEY_A)), k->jamo_shift);
        uint32_t out[2];
        submit_text(k, out, kvm_hangul_input(&k->ime, jamo, out), ctrl);
    } else if (usage == KVM_KEY_BACKSPACE && kvm_hangul_backspace(&k->ime)) {
        // the composition shrinks locally; nothing reaches the HOST
    } else {
        commit_preedit(k, ctrl); // the syllable goes before the key, in order
        const uint16_t v = (uint16_t)(usage | (k->jamo_shift ? UI_TEXT_KEY_SHIFT : 0u));
        if (submit(k, UI_ACT_TEXT_KEY, ctrl, v)) {
            k->c.text_keys++;
        }
    }
    const bool had_shift = k->jamo_shift;
    k->jamo_shift = false;
    update_preedit(k);
    if (had_shift) {
        refresh(k);
    }
}

// ---- ZH composition layer (ADR-024 FR-746, pinyin) ----------------------------------------------
static size_t utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (u[i] == 0u || (u[i] >= 0xD800u && u[i] <= 0xDFFFu) || o + 4u > cap) {
            continue; // the decoder yields BMP text only; a surrogate or no room is dropped, never split
        }
        o += utf8_of(u[i], out + o);
    }
    out[o] = '\0';
    return o;
}

static void zh_clear(ui_view_keyboard_t *k)
{
    k->py_len = 0;
    k->py[0] = '\0';
    k->flick_n = 0;
    k->zh_count = 0;
    k->zh_page = 0;
    if (k->conv != NULL) {
        k->conv->reset(k->conv->ctx);
    }
}

static void zh_decode(ui_view_keyboard_t *k)
{
    k->zh_page = 0;
    if (k->conv == NULL || k->py_len == 0u) {
        k->zh_count = 0;
        return;
    }
    const size_t n = k->conv->search(k->conv->ctx, k->py, k->py_len);
    k->zh_count = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
}

static size_t zh_candidate(const ui_view_keyboard_t *k, size_t i, uint16_t *out)
{
    out[0] = 0;
    if (k->conv == NULL || i >= k->zh_count) {
        return 0;
    }
    const size_t n = k->conv->candidate(k->conv->ctx, i, out, UI_VIEW_KBD_CAND_UTF16 + 1u);
    return n > UI_VIEW_KBD_CAND_UTF16 ? UI_VIEW_KBD_CAND_UTF16 : n;
}

/** Hanzi fixed so far (a prefix of candidate 0) and the input letters they cover. */
static size_t zh_fixed(const ui_view_keyboard_t *k, uint16_t *out, size_t *consumed)
{
    *consumed = 0;
    if (k->conv == NULL || k->zh_count == 0u) {
        return 0;
    }
    const size_t n = zh_candidate(k, 0, out);
    size_t f = k->conv->fixed_len(k->conv->ctx);
    f = f > n ? n : f;
    const size_t c = k->conv->consumed(k->conv->ctx);
    *consumed = c > k->py_len ? k->py_len : c;
    return f;
}

/** JA: the reading (kana) of the parsed input after the fixed text; `*rest` becomes the first letter it leaves. */
static size_t zh_reading(const ui_view_keyboard_t *k, uint16_t *out, size_t *rest)
{
    out[0] = 0;
    const size_t n = k->conv->preedit(k->conv->ctx, out, UI_VIEW_KBD_READING_UTF16 + 1u);
    const size_t p = k->conv->parsed(k->conv->ctx);
    const size_t q = p > k->py_len ? k->py_len : p;
    *rest = q < *rest ? *rest : q;
    return n > UI_VIEW_KBD_READING_UTF16 ? UI_VIEW_KBD_READING_UTF16 : n;
}

/** The strip's labels from the decoder state (no enable/disable: refresh() owns that). */
static void zh_render(ui_view_keyboard_t *k)
{
    uint16_t u[UI_VIEW_KBD_CAND_UTF16 + 1u] = {0}; // zh_fixed() leaves it unwritten when nothing is fixed
    size_t consumed;
    const size_t fixed = zh_fixed(k, u, &consumed);
    size_t o = utf16_to_utf8(u, fixed, k->zh_input_text, UI_VIEW_KBD_CAND_UTF8);
    size_t from = consumed;
    if (k->conv != NULL && k->conv->preedit != NULL && k->py_len > 0u) {
        uint16_t rd[UI_VIEW_KBD_READING_UTF16 + 1u];
        const size_t r = zh_reading(k, rd, &from);
        o += utf16_to_utf8(rd, r, k->zh_input_text + o, sizeof(k->zh_input_text) - o - UI_VIEW_KBD_INPUT_CAP - 1u);
    }
    for (size_t i = from; i < k->py_len; i++) {
        k->zh_input_text[o++] = k->py[i];
    }
    k->zh_input_text[o] = '\0';
    if (k->zh_input != NULL) {
        lv_label_set_text_static(k->zh_input, k->zh_input_text);
    }
    for (uint32_t s = 0; s < UI_VIEW_KBD_CANDS; s++) {
        char *t = k->zh_cand_text[s];
        const size_t g = (size_t)k->zh_page + s;
        t[0] = '\0';
        if (g < k->zh_count) {
            const size_t n = zh_candidate(k, g, u);
            // candidate 0 is the whole sentence: show what it adds after the fixed hanzi
            const size_t skip = g == 0u ? (fixed < n ? fixed : n) : 0u;
            t[0] = (char)('1' + s);
            t[1] = ' ';
            utf16_to_utf8(u + skip, n - skip, t + 2, sizeof(k->zh_cand_text[s]) - 2u);
        }
        ui_action_key_set_label(k->zh_cand[s], t);
    }
}

static void zh_update(ui_view_keyboard_t *k)
{
    zh_render(k);
    refresh(k);
}

static void zh_submit_units(ui_view_keyboard_t *k, const uint16_t *u, size_t n, uint16_t ctrl)
{
    for (size_t i = 0; i < n; i++) {
        if (u[i] != 0u && (u[i] < 0xD800u || u[i] > 0xDFFFu) && submit(k, UI_ACT_TEXT_CHAR, ctrl, u[i])) {
            k->c.text_chars++;
        }
    }
}

/** The input letters from `from` on, as typed (the syllable separator is not text). */
static void zh_submit_letters(ui_view_keyboard_t *k, size_t from, uint16_t ctrl)
{
    for (size_t i = from; i < k->py_len; i++) {
        const char c = k->py[i];
        if (c >= 'a' && c <= 'z' && submit(k, UI_ACT_TEXT_KEY, ctrl, (uint16_t)(KVM_KEY_A + (c - 'a')))) {
            k->c.text_keys++;
        } else if (c == '-' && submit(k, UI_ACT_TEXT_KEY, ctrl, (uint16_t)KVM_KEY_MINUS)) {
            k->c.text_keys++;
        }
    }
}

/** Enter: the fixed hanzi, then the letters not converted yet, as typed. */
static void zh_commit_raw(ui_view_keyboard_t *k, uint16_t ctrl)
{
    if (k->py_len == 0u) {
        return;
    }
    uint16_t u[UI_VIEW_KBD_CAND_UTF16 + 1u] = {0}; // zh_fixed() leaves it unwritten when nothing is fixed
    size_t consumed;
    const size_t fixed = zh_fixed(k, u, &consumed);
    zh_submit_units(k, u, fixed, ctrl);
    size_t from = fixed > 0u ? consumed : 0u;
    if (k->conv != NULL && k->conv->preedit != NULL) {
        uint16_t rd[UI_VIEW_KBD_READING_UTF16 + 1u];
        const size_t r = zh_reading(k, rd, &from); // JA: Enter gives the kana, not the romaji
        zh_submit_units(k, rd, r, ctrl);
    }
    zh_submit_letters(k, from, ctrl);
    k->c.zh_commits++;
    zh_clear(k);
}

/** The whole sentence (candidate 0) and any trailing letters the decoder could not segment. */
static void zh_commit_sentence(ui_view_keyboard_t *k, uint16_t ctrl)
{
    if (k->py_len == 0u) {
        return;
    }
    if (k->conv == NULL || k->zh_count == 0u) {
        zh_commit_raw(k, ctrl); // nothing decoded: the letters as typed
        return;
    }
    uint16_t u[UI_VIEW_KBD_CAND_UTF16 + 1u];
    const size_t n = zh_candidate(k, 0, u);
    const size_t parsed = k->conv->parsed(k->conv->ctx);
    zh_submit_units(k, u, n, ctrl);
    zh_submit_letters(k, parsed > k->py_len ? k->py_len : parsed, ctrl);
    k->c.zh_commits++;
    zh_clear(k);
}

static void zh_choose(ui_view_keyboard_t *k, size_t i, uint16_t ctrl)
{
    if (k->conv == NULL || i >= k->zh_count) {
        return;
    }
    k->c.zh_choices++;
    const size_t n = k->conv->choose(k->conv->ctx, i);
    k->zh_count = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
    k->zh_page = 0;
    if (k->conv->complete(k->conv->ctx)) {
        zh_commit_sentence(k, ctrl);
    }
}

/** Full-width punctuation of Chinese text for a key typed with nothing composed, or 0. */
static uint16_t zh_punct(uint8_t usage, bool shift)
{
    switch (usage) {
    case KVM_KEY_COMMA: return shift ? 0u : 0xFF0Cu;     // ，
    case KVM_KEY_PERIOD: return shift ? 0u : 0x3002u;    // 。
    case KVM_KEY_SEMICOLON: return shift ? 0xFF1Au : 0xFF1Bu; // ： ；
    case KVM_KEY_BACKSLASH: return shift ? 0u : 0x3001u; // 、
    case KVM_KEY_SLASH: return shift ? 0xFF1Fu : 0u;     // ？
    case KVM_KEY_1: return shift ? 0xFF01u : 0u;         // ！
    default: return 0u;
    }
}

/** Japanese punctuation for a key typed with nothing composed, or 0. */
static uint16_t ja_punct(uint8_t usage, bool shift)
{
    switch (usage) {
    case KVM_KEY_COMMA: return shift ? 0u : 0x3001u;         // 、
    case KVM_KEY_PERIOD: return shift ? 0u : 0x3002u;        // 。
    case KVM_KEY_SLASH: return shift ? 0xFF1Fu : 0x30FBu;    // ？ ・
    case KVM_KEY_LEFT_BRACKET: return shift ? 0u : 0x300Cu;  // 「
    case KVM_KEY_RIGHT_BRACKET: return shift ? 0u : 0x300Du; // 」
    case KVM_KEY_1: return shift ? 0xFF01u : 0u;             // ！
    default: return 0u;
    }
}

/** A key with nothing composed: the layer's punctuation, else the key itself. */
static void zh_plain_key(ui_view_keyboard_t *k, uint8_t usage, bool shift, uint16_t ctrl)
{
    const uint16_t p = k->conv_layer == UI_KBD_LAYER_JA ? ja_punct(usage, shift) : zh_punct(usage, shift);
    if (p != 0u) {
        if (submit(k, UI_ACT_TEXT_CHAR, ctrl, p)) {
            k->c.text_chars++;
        }
        return;
    }
    if (submit(k, UI_ACT_TEXT_KEY, ctrl, (uint16_t)(usage | (shift ? UI_TEXT_KEY_SHIFT : 0u)))) {
        k->c.text_keys++;
    }
}

/** The longest input of the layer that is on. */
static uint8_t conv_cap(const ui_view_keyboard_t *k)
{
    return k->conv_layer == UI_KBD_LAYER_JA ? UI_VIEW_KBD_JA_CAP : UI_VIEW_KBD_PINYIN_CAP;
}

static void zh_usage(ui_view_keyboard_t *k, uint8_t usage, uint16_t ctrl)
{
    if (usage == KVM_KEY_LEFT_SHIFT) {
        k->zh_shift = !k->zh_shift;
        refresh(k);
        return;
    }
    if (kvm_usage_is_modifier(usage) || usage == KVM_KEY_CAPS_LOCK) {
        return; // disabled in the layer; a stray edge does nothing
    }
    const bool shift = k->zh_shift;
    k->zh_shift = false;
    const bool composing = k->py_len > 0u;
    const bool letter = usage >= KVM_KEY_A && usage <= KVM_KEY_Z;
    if (letter && !(shift && !composing)) {
        if (k->py_len < conv_cap(k)) {
            k->py[k->py_len++] = (char)('a' + (usage - KVM_KEY_A));
            k->py[k->py_len] = '\0';
            zh_decode(k);
        }
    } else if (k->conv_layer == UI_KBD_LAYER_JA && usage == KVM_KEY_MINUS && !shift) {
        if (k->py_len < conv_cap(k)) { // the long vowel mark (ー) is part of the reading
            k->py[k->py_len++] = '-';
            k->py[k->py_len] = '\0';
            zh_decode(k);
        }
    } else if (composing && usage == KVM_KEY_APOSTROPHE) {
        if (k->py[k->py_len - 1u] != '\'' && k->py_len < conv_cap(k)) {
            k->py[k->py_len++] = '\'';
            k->py[k->py_len] = '\0';
            zh_decode(k);
        }
    } else if (composing && usage == KVM_KEY_BACKSPACE) {
        if (k->conv != NULL && k->zh_count > 0u && k->conv->fixed_len(k->conv->ctx) > 0u) {
            const size_t n = k->conv->cancel_last(k->conv->ctx); // undo the last choice, keep the letters
            k->zh_count = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
            k->zh_page = 0;
        } else {
            k->py[--k->py_len] = '\0';
            if (k->py_len == 0u) {
                zh_clear(k);
            } else {
                zh_decode(k);
            }
        }
    } else if (composing && usage == KVM_KEY_SPACE) {
        if (k->zh_count > 0u) {
            zh_choose(k, 0, ctrl); // the whole sentence
        } else {
            zh_commit_raw(k, ctrl);
        }
    } else if (composing && usage == KVM_KEY_ENTER) {
        zh_commit_raw(k, ctrl);
    } else if (composing && usage == KVM_KEY_ESCAPE) {
        zh_clear(k); // discarded; nothing reaches the HOST
    } else if (composing && !shift && usage >= KVM_KEY_1 && usage < KVM_KEY_1 + UI_VIEW_KBD_CANDS) {
        zh_choose(k, (size_t)k->zh_page + (size_t)(usage - KVM_KEY_1), ctrl);
    } else {
        if (composing) {
            zh_commit_sentence(k, ctrl); // the text on screen goes before the key, in order
        }
        zh_plain_key(k, usage, shift, ctrl);
    }
    zh_update(k);
}

static void zh_key(ui_view_keyboard_t *k, uint32_t idx, uint16_t ctrl)
{
    zh_usage(k, s_keys[idx].usage, ctrl);
}

// ---- JA flick pad (FR-746 "JP flick") -------------------------------------------------------------------------------
// Grid (row-major): あ か さ ⌫ / た な は Space / ま や ら Enter / ゛゜小 わ 、。？！ ABC. Kana key q (0..11) sits at
// grid index (q / 3) * 4 + q % 3; the right column holds the function keys. Directions: 0 tap, 1 left, 2 up, 3 right,
// 4 down. A kana enters `py` as romaji the ported Romkan reads back as the same kana.
enum { FLICK_MOD = 9, FLICK_WA = 10, FLICK_PUNCT = 11, FLICK_KANA = 12 };
enum { FLICK_FN_BS = 1, FLICK_FN_SPACE, FLICK_FN_ENTER, FLICK_FN_ABC }; // UI_CTRL_KBD_FLICK + n

static const char *const k_flick_rom[FLICK_KANA][5] = {
    {"a", "i", "u", "e", "o"},      {"ka", "ki", "ku", "ke", "ko"}, {"sa", "si", "su", "se", "so"},
    {"ta", "ti", "tu", "te", "to"}, {"na", "ni", "nu", "ne", "no"}, {"ha", "hi", "hu", "he", "ho"},
    {"ma", "mi", "mu", "me", "mo"}, {"ya", NULL, "yu", NULL, "yo"}, {"ra", "ri", "ru", "re", "ro"},
    {NULL, NULL, NULL, NULL, NULL}, {"wa", "wo", "nn", "-", NULL},   {NULL, NULL, NULL, NULL, NULL},
};

// What the key shows under the finger, and the punctuation it types (や's flicks left / right are the brackets).
static const uint16_t k_flick_kana[FLICK_KANA][5] = {
    {0x3042, 0x3044, 0x3046, 0x3048, 0x304A}, {0x304B, 0x304D, 0x304F, 0x3051, 0x3053},
    {0x3055, 0x3057, 0x3059, 0x305B, 0x305D}, {0x305F, 0x3061, 0x3064, 0x3066, 0x3068},
    {0x306A, 0x306B, 0x306C, 0x306D, 0x306E}, {0x306F, 0x3072, 0x3075, 0x3078, 0x307B},
    {0x307E, 0x307F, 0x3080, 0x3081, 0x3082}, {0x3084, 0x300C, 0x3086, 0x300D, 0x3088},
    {0x3089, 0x308A, 0x308B, 0x308C, 0x308D}, {0, 0, 0, 0, 0},
    {0x308F, 0x3092, 0x3093, 0x30FC, 0},      {0x3001, 0x3002, 0xFF1F, 0xFF01, 0},
};

static bool flick_is_punct(uint8_t q, uint8_t d)
{
    return q == FLICK_PUNCT || (q == 7u && (d == 1u || d == 3u));
}

/** Variant v of kana (q, d): 0 plain, 1 dakuten, 2 handakuten, 3 small. */
static bool flick_has(uint8_t q, uint8_t d, uint8_t v)
{
    switch (v) {
    case 0: return k_flick_rom[q][d] != NULL;
    case 1: return q == 1u || q == 2u || q == 3u || q == 5u;     // か さ た は rows
    case 2: return q == 5u;                                     // は row
    case 3: return q == 0u || (q == 3u && d == 2u) || (q == 7u && (d == 0u || d == 2u || d == 4u)) ||
                   (q == FLICK_WA && d == 0u);                  // ぁ..ぉ っ ゃゅょ ゎ
    default: return false;
    }
}

/** The romaji of a kana variant (NUL-terminated in out[4]); its length, 0 if none. */
static size_t flick_romaji(uint8_t q, uint8_t d, uint8_t v, char out[4])
{
    const char *base = k_flick_rom[q][d];
    if (base == NULL || !flick_has(q, d, v)) {
        return 0;
    }
    size_t n = 0;
    if (v == 3u) {
        out[n++] = 'x';
    }
    for (const char *c = base; *c != '\0' && n < 3u; c++) {
        out[n++] = *c;
    }
    out[n] = '\0';
    if (v == 1u || v == 2u) {
        static const char from[] = "ksth";
        static const char dak[] = "gzdb";
        const char *f = strchr(from, out[0]);
        out[0] = v == 2u ? 'p' : (f != NULL ? dak[f - from] : out[0]);
    }
    return n;
}

static uint8_t flick_dir(lv_point_t s, lv_point_t p)
{
    const int32_t dx = p.x - s.x;
    const int32_t dy = p.y - s.y;
    const int32_t ax = dx < 0 ? -dx : dx;
    const int32_t ay = dy < 0 ? -dy : dy;
    if (ax < UI_FLICK_MIN && ay < UI_FLICK_MIN) {
        return 0;
    }
    if (ax >= ay) {
        return dx < 0 ? 1u : 3u;
    }
    return dy < 0 ? 2u : 4u;
}

static bool flick_live(const ui_view_keyboard_t *k)
{
    return k->flick && k->converting && k->conv_layer == UI_KBD_LAYER_JA && host_mounted(k);
}

static void flick_kana_in(ui_view_keyboard_t *k, uint8_t q, uint8_t d)
{
    char r[4];
    const size_t n = flick_romaji(q, d, 0, r);
    if (n == 0u || (size_t)k->py_len + n > UI_VIEW_KBD_JA_CAP || k->flick_n >= UI_VIEW_KBD_INPUT_CAP) {
        return; // a direction with no kana, or the input is full
    }
    memcpy(k->py + k->py_len, r, n + 1u);
    k->py_len = (uint8_t)(k->py_len + n);
    k->flick_seg[k->flick_n] = (uint8_t)(q | (d << 4));
    k->flick_var[k->flick_n] = 0;
    k->flick_len[k->flick_n] = (uint8_t)n;
    k->flick_n++;
    k->c.flicks++;
    zh_decode(k);
}

/** ゛゜小: the last kana typed on the pad moves to its next form (plain -> dakuten -> handakuten -> small). */
static void flick_modify(ui_view_keyboard_t *k)
{
    if (k->flick_n == 0u) {
        return;
    }
    const uint8_t i = (uint8_t)(k->flick_n - 1u);
    const uint8_t q = k->flick_seg[i] & 0x0Fu;
    const uint8_t d = (uint8_t)(k->flick_seg[i] >> 4);
    uint8_t v = k->flick_var[i];
    for (uint8_t step = 0; step < 4u; step++) {
        v = (uint8_t)((v + 1u) & 3u);
        if (flick_has(q, d, v)) {
            break;
        }
    }
    char r[4];
    const size_t n = flick_romaji(q, d, v, r);
    const size_t keep = (size_t)k->py_len - k->flick_len[i];
    if (v == k->flick_var[i] || n == 0u || keep + n > UI_VIEW_KBD_JA_CAP) {
        return;
    }
    memcpy(k->py + keep, r, n + 1u);
    k->py_len = (uint8_t)(keep + n);
    k->flick_var[i] = v;
    k->flick_len[i] = (uint8_t)n;
    zh_decode(k);
}

static void flick_punct(ui_view_keyboard_t *k, uint16_t cp)
{
    if (cp == 0u) {
        return;
    }
    if (k->py_len > 0u) {
        zh_commit_sentence(k, UI_CTRL_KBD_FLICK); // the text on screen goes before the mark, in order
    }
    if (submit(k, UI_ACT_TEXT_CHAR, UI_CTRL_KBD_FLICK, cp)) {
        k->c.text_chars++;
    }
}

static void flick_release(ui_view_keyboard_t *k, uint8_t q, uint8_t d)
{
    if (q == FLICK_MOD) {
        flick_modify(k); // any direction: the key has one function
    } else if (flick_is_punct(q, d)) {
        flick_punct(k, k_flick_kana[q][d]);
    } else {
        flick_kana_in(k, q, d);
    }
    zh_update(k);
}

// The resting legend of each kana key: あかさたなはまやら ゛゜小 わ 、。？！
static const char *const k_pad_tap[FLICK_KANA] = {
    "\xE3\x81\x82", "\xE3\x81\x8B", "\xE3\x81\x95", "\xE3\x81\x9F", "\xE3\x81\xAA", "\xE3\x81\xAF",
    "\xE3\x81\xBE", "\xE3\x82\x84", "\xE3\x82\x89", "\xE3\x82\x9B\xE3\x82\x9C\xE5\xB0\x8F",
    "\xE3\x82\x8F", "\xE3\x80\x81\xE3\x80\x82\xEF\xBC\x9F\xEF\xBC\x81"};

/** While held: the kana of the direction under the finger; `rest` puts the resting legend back. */
static void flick_show(ui_view_keyboard_t *k, uint8_t q, uint8_t d, bool rest)
{
    lv_obj_t *label = k->flick_label[(q / 3u) * 4u + q % 3u];
    if (label == NULL || q == FLICK_MOD) {
        return;
    }
    if (rest) {
        lv_label_set_text_static(label, k_pad_tap[q]);
        return;
    }
    char t[8];
    const uint16_t cp = k_flick_kana[q][d] != 0u ? k_flick_kana[q][d] : k_flick_kana[q][0];
    const size_t n = utf8_of(cp, t);
    t[n] = '\0';
    lv_label_set_text(label, t);
}

static void flick_event_cb(lv_event_t *e)
{
    ui_view_keyboard_t *k = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    const lv_event_code_t code = lv_event_get_code(e);
    int q = -1;
    for (uint32_t g = 0; g < UI_VIEW_KBD_FLICK_KEYS; g++) {
        if (k->flick_key[g] == target && g % 4u != 3u) {
            q = (int)((g / 4u) * 3u + g % 4u);
        }
    }
    lv_indev_t *indev = lv_indev_active();
    if (q < 0 || indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        k->flick_down = (int8_t)q;
        k->flick_start = p;
        flick_show(k, (uint8_t)q, 0, false);
    } else if (code == LV_EVENT_PRESSING && k->flick_down == q) {
        flick_show(k, (uint8_t)q, flick_dir(k->flick_start, p), false);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && k->flick_down == q) {
        const uint8_t d = flick_dir(k->flick_start, p);
        k->flick_down = -1;
        flick_show(k, (uint8_t)q, 0, true);
        if (code == LV_EVENT_RELEASED && flick_live(k)) {
            flick_release(k, (uint8_t)q, d);
        }
    }
}

/** Which rows show: the flick pad replaces the number, F and letter rows while it is on. */
static void apply_rows(ui_view_keyboard_t *k)
{
    if (k->root == NULL) {
        return;
    }
    const bool ja = k->converting && k->conv_layer == UI_KBD_LAYER_JA;
    const bool pad = ja && k->flick;
    lv_obj_set_hidden(k->row_number, pad || k->fn_layer);
    lv_obj_set_hidden(k->row_fn, pad || !k->fn_layer);
    lv_obj_set_hidden(k->row_top, pad);
    lv_obj_set_hidden(k->row_mid, pad);
    lv_obj_set_hidden(k->row_low, pad);
    lv_obj_set_hidden(k->flick_pad, !pad);
    lv_obj_set_hidden(k->flick_toggle, !ja);
    ui_action_key_set_label(k->flick_toggle, k->flick ? "QWERTY" : "Flick");
}

static void set_flick(ui_view_keyboard_t *k, bool on)
{
    if (on && !(k->converting && k->conv_layer == UI_KBD_LAYER_JA)) {
        return;
    }
    release_held(k, true);
    k->flick = on;
    k->flick_n = 0; // the composition stays; only the pad's per-kana history starts over
    k->flick_down = -1;
    apply_rows(k);
    refresh(k);
}

static void flick_fn(ui_view_keyboard_t *k, uint16_t control_id)
{
    switch (control_id - UI_CTRL_KBD_FLICK) {
    case 0:
        set_flick(k, !k->flick);
        return;
    case FLICK_FN_BS:
        if (k->py_len > 0u && k->flick_n > 0u &&
            !(k->conv != NULL && k->zh_count > 0u && k->conv->fixed_len(k->conv->ctx) > 0u)) {
            k->flick_n--; // one kana, however many letters it took
            k->py_len = (uint8_t)(k->py_len - k->flick_len[k->flick_n]);
            k->py[k->py_len] = '\0';
            if (k->py_len == 0u) {
                zh_clear(k);
            } else {
                zh_decode(k);
            }
            zh_update(k);
        } else {
            zh_usage(k, KVM_KEY_BACKSPACE, control_id);
        }
        return;
    case FLICK_FN_SPACE:
        zh_usage(k, KVM_KEY_SPACE, control_id);
        return;
    case FLICK_FN_ENTER:
        zh_usage(k, KVM_KEY_ENTER, control_id);
        return;
    case FLICK_FN_ABC:
        set_flick(k, false);
        return;
    default:
        return;
    }
}

static void zh_strip(ui_view_keyboard_t *k, uint16_t control_id)
{
    if (control_id == UI_CTRL_KBD_CAND_PREV) {
        k->zh_page = k->zh_page >= UI_VIEW_KBD_CANDS ? (uint16_t)(k->zh_page - UI_VIEW_KBD_CANDS) : 0u;
    } else if (control_id == UI_CTRL_KBD_CAND_NEXT) {
        if ((uint32_t)k->zh_page + UI_VIEW_KBD_CANDS < k->zh_count) {
            k->zh_page = (uint16_t)(k->zh_page + UI_VIEW_KBD_CANDS);
        }
    } else {
        zh_choose(k, (size_t)k->zh_page + (size_t)(control_id - UI_CTRL_KBD_CAND_BASE), control_id);
    }
    zh_update(k);
}

static void show_zh_strip(ui_view_keyboard_t *k, bool on)
{
    if (k->row_zh != NULL) {
        lv_obj_set_hidden(k->row_zh, !on);
    }
}

static void apply_strip_font(ui_view_keyboard_t *k)
{
    if (k->zh_input == NULL) {
        return;
    }
    const lv_font_t *f = ui_theme_font(k->conv_layer == UI_KBD_LAYER_JA ? UI_FONT_KANJI : UI_FONT_HANZI);
    lv_obj_set_style_text_font(k->zh_input, f, 0);
    for (uint32_t i = 0; i < UI_VIEW_KBD_CANDS; i++) {
        lv_obj_set_style_text_font(k->zh_cand[i], f, 0);
    }
    const lv_font_t *kana = ui_theme_font(UI_FONT_KANJI); // the pad is JA only
    for (uint32_t g = 0; g < UI_VIEW_KBD_FLICK_KEYS; g++) {
        if (g % 4u != 3u && k->flick_label[g] != NULL) {
            lv_obj_set_style_text_font(k->flick_label[g], kana, 0);
        }
    }
}

/** Turn a conversion layer on (UI_KBD_LAYER_ZH / _JA) or off (0). Switching ZH <-> JA commits the old text. */
static void set_conv(ui_view_keyboard_t *k, uint8_t layer, uint16_t ctrl)
{
    const uint8_t cur = k->converting ? k->conv_layer : 0u;
    if (cur == layer) {
        return;
    }
    release_held(k, true);
    if (k->converting) {
        zh_commit_raw(k, ctrl); // what is on screen is what the user meant to type
    } else if (k->hangul) {
        set_hangul(k, false, ctrl); // its syllable is committed; no modifier could be armed inside it
    } else {
        text_cancel(k, ctrl); // FR-748: no armed modifier, no queued key may reach the composed text
    }
    k->converting = layer != 0u;
    k->conv_layer = layer;
    k->conv = layer == UI_KBD_LAYER_JA ? k->ja : (layer == UI_KBD_LAYER_ZH ? k->zh : NULL);
    zh_clear(k);
    k->zh_shift = false;
    if (layer != UI_KBD_LAYER_JA) {
        k->flick = false; // the pad belongs to the JA layer; it starts as QWERTY next time
    }
    if (k->converting) {
        apply_strip_font(k);
    }
    show_zh_strip(k, k->converting);
    apply_rows(k);
    zh_update(k);
}

/** Leave the conversion layer without sending anything (the composition is discarded). */
static void drop_conv(ui_view_keyboard_t *k)
{
    if (!k->converting) {
        return;
    }
    zh_clear(k);
    text_cancel(k, 0u);
    k->converting = false;
    k->conv_layer = 0u;
    k->conv = NULL;
    k->zh_shift = false;
    k->flick = false;
    show_zh_strip(k, false);
    apply_rows(k);
}

// ---- the language key (ADR-037) ------------------------------------------------------------
/** Put `lang` in effect. Every switch rule (release held, commit, TEXT_CANCEL) belongs to the two calls below. */
static void set_lang(ui_view_keyboard_t *k, ui_kbd_lang_t lang, uint16_t ctrl)
{
    if (lang == lang_of(k)) {
        return;
    }
    switch (lang) {
    case UI_KBD_LANG_KO:
        set_hangul(k, true, ctrl);
        break;
    case UI_KBD_LANG_ZH:
        set_conv(k, UI_KBD_LAYER_ZH, ctrl);
        break;
    case UI_KBD_LANG_JA:
        set_conv(k, UI_KBD_LAYER_JA, ctrl);
        break;
    case UI_KBD_LANG_EN:
    default:
        if (k->hangul) {
            set_hangul(k, false, ctrl); // the syllable on screen is committed
        } else {
            set_conv(k, 0u, ctrl); // ... and so is the conversion input
        }
        break;
    }
    k->c.lang_switches++;
}

static void toast_deleted(lv_event_t *e)
{
    ui_view_keyboard_t *k = lv_event_get_user_data(e);
    k->toast = NULL;
}

/** Say why nothing changed. The toast layer is never clickable, so it cannot swallow the next key. */
static void say(ui_view_keyboard_t *k, const char *text)
{
    lv_obj_t *parent = k->router != NULL ? k->router->toast_layer : NULL;
    if (parent == NULL) {
        return; // no router bound: the refusal is still counted, there is just nowhere to show it
    }
    (void)snprintf(k->toast_text, sizeof(k->toast_text), "%s", text);
    if (k->toast != NULL) {
        ui_toast_dismiss(k->toast); // one message at a time; its delete event clears the pointer
    }
    k->toast = ui_toast_show(parent, k->toast_text, UI_TOAST_DEFAULT_MS);
    if (k->toast == NULL) {
        return;
    }
    lv_obj_add_event_cb(k->toast, toast_deleted, LV_EVENT_DELETE, k);
    k->c.toasts++;
}

/** A tap: the next language that is turned on and available, in the order EN -> KO -> ZH -> JA -> EN. */
static void cycle_lang(ui_view_keyboard_t *k, uint16_t ctrl)
{
    const uint32_t cur = (uint32_t)lang_of(k);
    for (uint32_t step = 1u; step < UI_KBD_LANG_COUNT; step++) {
        const ui_kbd_lang_t next = (ui_kbd_lang_t)((cur + step) % UI_KBD_LANG_COUNT);
        if (lang_block(k, next) == NULL) {
            set_lang(k, next, ctrl);
            return;
        }
    }
    // Only reachable from English: every other language is off or unavailable, and English is always available.
    k->c.lang_refused++;
    say(k, k->langs == UI_KBD_LANG_BIT(UI_KBD_LANG_EN) ? "Add input languages in Settings > Keyboard"
                                                       : "No other input language is available right now");
}

static void close_picker(ui_view_keyboard_t *k, uint16_t ctrl)
{
    // 07 §3 as in the status cards and the gesture help: ask the shell first, fall back to the router.
    const uint32_t rid =
        k->actions != NULL ? ui_actions_submit(k->actions, UI_ACT_DISMISS_OVERLAY, ctrl, 0u, now(k), NULL) : 0u;
    if (rid == 0u && k->router != NULL) {
        ui_overlay_dismiss(k->router);
    }
}

static void lang_tap_cb(lv_event_t *e)
{
    // SHORT_CLICKED, not CLICKED: LVGL suppresses the short click once a long press has fired, so one gesture is
    // either the cycle or the picker, never both (lv_indev.c indev_proc_short_click).
    ui_view_keyboard_t *k = lv_event_get_user_data(e);
    if (k->root == NULL || !host_mounted(k)) {
        return;
    }
    cycle_lang(k, (uint16_t)ui_action_key_control_id(k->lang_key));
}

static void lang_pick_clicked(lv_event_t *e)
{
    ui_view_keyboard_t *k = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    for (uint32_t i = 0; i < UI_KBD_LANG_COUNT; i++) {
        if (k->pick_btn[i] != btn) {
            continue;
        }
        const uint16_t ctrl = (uint16_t)(UI_CTRL_KBD_LANG_PICK_BASE + i);
        if (lang_block(k, (ui_kbd_lang_t)i) != NULL) {
            k->c.lang_refused++; // a disabled row sends no click; this is the belt to that braces
            return;
        }
        set_lang(k, (ui_kbd_lang_t)i, ctrl);
        close_picker(k, ctrl);
        return;
    }
}

static void lang_close_clicked(lv_event_t *e)
{
    close_picker(lv_event_get_user_data(e), UI_CTRL_KBD_LANG_CLOSE);
}

#define LANG_CARD_W 560
#define LANG_CLOSE_W 168
#define LANG_CODE_W 56

static bool lang_build(void *ctx, lv_obj_t *card)
{
    ui_view_keyboard_t *k = ctx;
    if (!ui_theme_is_ready()) {
        return false;
    }
    // As the gesture help and the status cards: the theme card instead of the default style, centering put back.
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_width(card, LANG_CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(card, false);
    lv_obj_center(card);

    lv_obj_t *title = lv_label_create(card);
    ui_theme_apply(title, UI_STYLE_TEXT_HEADING, LV_STATE_DEFAULT);
    lv_label_set_text_static(title, "Input language");
    lv_obj_t *note = lv_label_create(card);
    ui_theme_apply(note, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
    lv_label_set_text_static(note, "Turn languages on in Settings > Keyboard.");

    const ui_kbd_lang_t cur = lang_of(k);
    memset(k->pick_btn, 0, sizeof(k->pick_btn));
    for (uint32_t i = 0; i < UI_KBD_LANG_COUNT; i++) {
        const ui_kbd_lang_t lang = (ui_kbd_lang_t)i;
        if (lang != UI_KBD_LANG_EN && (k->langs & UI_KBD_LANG_BIT(lang)) == 0u) {
            continue; // the picker lists what Settings turned on, nothing else
        }
        const char *why = lang_block(k, lang);
        lv_obj_t *row = lv_button_create(card);
        if (row == NULL) {
            return false;
        }
        lv_obj_remove_style_all(row);
        ui_theme_apply_key(row, UI_KEY_MODIFIER);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, UI_MODAL_BTN_H);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_style_pad_hor(row, UI_SPACE_M, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, UI_SPACE_M, LV_PART_MAIN);

        lv_obj_t *code = lv_label_create(row);
        ui_theme_apply(code, UI_STYLE_TEXT_LABEL, LV_STATE_DEFAULT);
        lv_label_set_text_static(code, ui_view_keyboard_lang_code(lang));
        lv_obj_set_width(code, LANG_CODE_W);
        lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_t *name = lv_label_create(row);
        ui_theme_apply(name, UI_STYLE_TEXT_LABEL, LV_STATE_DEFAULT);
        lv_label_set_text_static(name, ui_view_keyboard_lang_name(lang));
        lv_obj_set_flex_grow(name, 1);
        // The names start in one column, so the eye runs down the list instead of hunting for each label.
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_t *tail = lv_label_create(row);
        ui_theme_apply(tail, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
        lv_label_set_text_static(tail, lang == cur ? "Current" : (why != NULL ? why : ""));

        ui_action_key_set_checked(row, lang == cur);
        lv_obj_set_disabled(row, why != NULL);
        lv_obj_set_clickable(row, why == NULL);
        lv_obj_add_event_cb(row, lang_pick_clicked, LV_EVENT_CLICKED, k);
        k->pick_btn[i] = row;
    }

    lv_obj_t *foot = lv_obj_create(card);
    if (foot == NULL) {
        return false;
    }
    lv_obj_remove_style_all(foot);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(foot, false);
    lv_obj_set_clickable(foot, false);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *close = lv_button_create(foot);
    if (close == NULL) {
        return false;
    }
    lv_obj_remove_style_all(close);
    ui_theme_apply_button(close, UI_BTN_SECONDARY);
    lv_obj_set_size(close, LANG_CLOSE_W, UI_MODAL_BTN_H);
    lv_obj_set_scrollable(close, false);
    lv_obj_t *label = lv_label_create(close);
    lv_label_set_text_static(label, "Close");
    lv_obj_center(label);
    lv_obj_add_event_cb(close, lang_close_clicked, LV_EVENT_CLICKED, k);

    k->lang_card = card;
    k->c.picker_opens++;
    return true;
}

static void lang_on_close(void *ctx)
{
    ui_view_keyboard_t *k = ctx;
    k->lang_card = NULL;
    memset(k->pick_btn, 0, sizeof(k->pick_btn));
    k->c.picker_closes++;
}

static void lang_long_cb(lv_event_t *e)
{
    ui_view_keyboard_t *k = lv_event_get_user_data(e);
    if (k->root == NULL || !host_mounted(k)) {
        return;
    }
    // Blocking, so a tap on the card can never reach a cap underneath it.
    if (k->router == NULL || !ui_overlay_show(k->router, UI_OVERLAY_KBD_LANG)) {
        k->c.lang_refused++;
    }
}

// ---- ADR-039 section 8: the quick switch of the HOST layout ------------------------------------------------
// A long press on the layout strip over the field opens a blocking card of the pinned and the recently used
// layouts; one tap reports that layout and closes the card. This is not the ADR-037 language cap: that one
// changes what the device composes, this one changes which layout the device believes the HOST is set to.

#define LAYOUT_CARD_W 640
#define LAYOUT_TILE_W 188
#define LAYOUT_TILE_H 62

/** The entry of quick-switch tile `i`, or NULL: favorites first, then the recents that are not pinned. */
static const kbd_layout_t *quick_entry(const ui_view_keyboard_t *k, uint8_t i)
{
    uint8_t n = 0;
    for (uint8_t f = 0; f < k->host_fav_count && f < UI_VIEW_KBD_FAV_MAX; f++) {
        const kbd_layout_t *lay = kbd_layouts_find(k->host_fav[f]);
        if (lay == NULL) {
            continue; // an id this build's tables no longer ship: no tile, but the record keeps it
        }
        if (n++ == i) {
            return lay;
        }
    }
    for (uint8_t r = 0; r < k->host_recent_count && r < UI_VIEW_KBD_RECENT_MAX; r++) {
        const kbd_layout_t *lay = kbd_layouts_find(k->host_recent[r]);
        if (lay == NULL) {
            continue;
        }
        bool pinned = false;
        for (uint8_t f = 0; f < k->host_fav_count && f < UI_VIEW_KBD_FAV_MAX; f++) {
            if (strcmp(k->host_fav[f], lay->id) == 0) {
                pinned = true;
                break;
            }
        }
        if (pinned) {
            continue; // a recent that already has a tile above is not offered twice
        }
        if (n++ == i) {
            return lay;
        }
    }
    return NULL;
}

/** The index of `lay` in the generated table, or -1. By id, not by pointer: the table is read through its API. */
static int32_t quick_index_of(const kbd_layout_t *lay)
{
    const size_t total = kbd_layouts_count_get();
    for (size_t i = 0; i < total; i++) {
        const kbd_layout_t *at = kbd_layouts_at(i);
        if (at != NULL && strcmp(at->id, lay->id) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void layout_pick_clicked(lv_event_t *e)
{
    ui_view_keyboard_t *k = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    for (uint8_t i = 0; i < UI_VIEW_KBD_QUICK_MAX; i++) {
        if (k->layout_pick[i] != btn) {
            continue;
        }
        const kbd_layout_t *lay = quick_entry(k, i);
        const int32_t idx = lay != NULL ? quick_index_of(lay) : -1;
        const uint16_t ctrl = (uint16_t)(UI_CTRL_KBD_LAYOUT_PICK_BASE + i);
        if (idx < 0 || k->layout_cb == NULL) {
            k->c.quick_refused++;
            close_picker(k, ctrl);
            return;
        }
        // Reported, not applied: the field is redrawn when the record comes back (AGENTS-UI MUST 5).
        k->layout_cb(k->layout_ctx, idx);
        k->c.quick_picks++;
        close_picker(k, ctrl);
        return;
    }
}

static void layout_close_clicked(lv_event_t *e)
{
    close_picker(lv_event_get_user_data(e), UI_CTRL_KBD_LAYOUT_CLOSE);
}

static bool layout_build(void *ctx, lv_obj_t *card)
{
    ui_view_keyboard_t *k = ctx;
    if (!ui_theme_is_ready()) {
        return false;
    }
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_width(card, LAYOUT_CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(card, false);
    lv_obj_center(card);

    lv_obj_t *title = lv_label_create(card);
    ui_theme_apply(title, UI_STYLE_TEXT_HEADING, LV_STATE_DEFAULT);
    lv_label_set_text_static(title, "HOST layout");
    lv_obj_t *note = lv_label_create(card);
    ui_theme_apply(note, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
    lv_label_set_text_static(note, "Pinned and recently used. Set the same layout on the HOST.");

    lv_obj_t *grid = lv_obj_create(card);
    if (grid == NULL) {
        return false;
    }
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(grid, false);
    lv_obj_set_clickable(grid, false);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, UI_SPACE_S, LV_PART_MAIN);

    memset(k->layout_pick, 0, sizeof(k->layout_pick));
    k->layout_pick_count = 0u;
    for (uint8_t i = 0; i < UI_VIEW_KBD_QUICK_MAX; i++) {
        const kbd_layout_t *lay = quick_entry(k, i);
        if (lay == NULL) {
            break;
        }
        lv_obj_t *tile = lv_button_create(grid);
        if (tile == NULL) {
            return false;
        }
        lv_obj_remove_style_all(tile);
        ui_theme_apply_key(tile, UI_KEY_MODIFIER);
        lv_obj_set_size(tile, LAYOUT_TILE_W, LAYOUT_TILE_H);
        lv_obj_set_scrollable(tile, false);
        snprintf(k->layout_pick_text[i], sizeof(k->layout_pick_text[i]), "%s\n%s", lay->id, lay->name);
        lv_obj_t *label = lv_label_create(tile);
        ui_theme_apply(label, UI_STYLE_TEXT_LABEL, LV_STATE_DEFAULT);
        lv_obj_set_width(label, LAYOUT_TILE_W - 2 * UI_SPACE_S);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text_static(label, k->layout_pick_text[i]);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(label);
        // The layout in force is marked, not disabled: tapping it is harmless and reports the same id.
        ui_action_key_set_checked(tile, strcmp(lay->id, k->layout_id) == 0);
        lv_obj_add_event_cb(tile, layout_pick_clicked, LV_EVENT_CLICKED, k);
        k->layout_pick[i] = tile;
        k->layout_pick_count++;
    }
    if (k->layout_pick_count == 0u) {
        lv_obj_t *empty = lv_label_create(grid);
        ui_theme_apply(empty, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
        lv_label_set_text_static(empty, "Nothing pinned yet - pin a layout in Settings > Keyboard.");
    }

    lv_obj_t *foot = lv_obj_create(card);
    if (foot == NULL) {
        return false;
    }
    lv_obj_remove_style_all(foot);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(foot, false);
    lv_obj_set_clickable(foot, false);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *close = lv_button_create(foot);
    if (close == NULL) {
        return false;
    }
    lv_obj_remove_style_all(close);
    ui_theme_apply_button(close, UI_BTN_SECONDARY);
    lv_obj_set_size(close, LANG_CLOSE_W, UI_MODAL_BTN_H);
    lv_obj_set_scrollable(close, false);
    lv_obj_t *clabel = lv_label_create(close);
    lv_label_set_text_static(clabel, "Close");
    lv_obj_center(clabel);
    lv_obj_add_event_cb(close, layout_close_clicked, LV_EVENT_CLICKED, k);

    k->layout_card = card;
    k->c.quick_opens++;
    return true;
}

static void layout_on_close(void *ctx)
{
    ui_view_keyboard_t *k = ctx;
    k->layout_card = NULL;
    memset(k->layout_pick, 0, sizeof(k->layout_pick));
    k->layout_pick_count = 0u;
    k->c.quick_closes++;
}

static void layout_strip_cb(lv_event_t *e)
{
    ui_view_keyboard_t *k = lv_event_get_user_data(e);
    if (k->root == NULL) {
        return;
    }
    // A screen that cannot change a value must not offer to: without a sink, or with nothing to offer, the
    // strip stays a label and the long press is counted, not acted on.
    if (k->layout_cb == NULL || quick_entry(k, 0) == NULL || k->router == NULL ||
        !ui_overlay_show(k->router, UI_OVERLAY_KBD_LAYOUT)) {
        k->c.quick_refused++;
    }
}

void ui_view_keyboard_set_host_lists(ui_view_keyboard_t *k, const char fav[][UI_VIEW_KBD_LAYOUT_CAP],
                                     uint8_t fav_count, const char recent[][UI_VIEW_KBD_LAYOUT_CAP],
                                     uint8_t recent_count)
{
    if (k == NULL) {
        return;
    }
    memset(k->host_fav, 0, sizeof(k->host_fav));
    memset(k->host_recent, 0, sizeof(k->host_recent));
    k->host_fav_count = fav_count <= UI_VIEW_KBD_FAV_MAX ? fav_count : (uint8_t)UI_VIEW_KBD_FAV_MAX;
    for (uint8_t i = 0; i < k->host_fav_count && fav != NULL; i++) {
        snprintf(k->host_fav[i], sizeof(k->host_fav[i]), "%s", fav[i]);
    }
    k->host_recent_count =
        recent_count <= UI_VIEW_KBD_RECENT_MAX ? recent_count : (uint8_t)UI_VIEW_KBD_RECENT_MAX;
    for (uint8_t i = 0; i < k->host_recent_count && recent != NULL; i++) {
        snprintf(k->host_recent[i], sizeof(k->host_recent[i]), "%s", recent[i]);
    }
    if (k->layout_card != NULL && k->router != NULL) {
        // The card shows a list that just changed under it: close it rather than leave stale tiles up.
        ui_overlay_dismiss(k->router);
    }
}

void ui_view_keyboard_set_layout_action(ui_view_keyboard_t *k, ui_view_keyboard_layout_fn cb, void *ctx)
{
    if (k == NULL) {
        return;
    }
    k->layout_cb = cb;
    k->layout_ctx = ctx;
}

lv_obj_t *ui_view_keyboard_layout_strip(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->hint : NULL;
}

const char *ui_view_keyboard_layout_code(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->strip_code_text : "";
}

const char *ui_view_keyboard_layout_name(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->strip_name_text : "";
}

lv_obj_t *ui_view_keyboard_layout_card(const ui_view_keyboard_t *k)
{
    return k != NULL ? k->layout_card : NULL;
}

lv_obj_t *ui_view_keyboard_layout_pick_button(const ui_view_keyboard_t *k, uint8_t i)
{
    if (k == NULL || k->layout_card == NULL || i >= UI_VIEW_KBD_QUICK_MAX) {
        return NULL;
    }
    return k->layout_pick[i];
}

const char *ui_view_keyboard_layout_pick_text(const ui_view_keyboard_t *k, uint8_t i)
{
    if (k == NULL || k->layout_card == NULL || i >= UI_VIEW_KBD_QUICK_MAX || k->layout_pick[i] == NULL) {
        return "";
    }
    return k->layout_pick_text[i];
}

void ui_view_keyboard_set_languages(ui_view_keyboard_t *k, bool ko, bool zh, bool ja)
{
    if (k == NULL) {
        return;
    }
    const uint8_t mask = (uint8_t)(UI_KBD_LANG_BIT(UI_KBD_LANG_EN) |
                                   (ko ? UI_KBD_LANG_BIT(UI_KBD_LANG_KO) : 0u) |
                                   (zh ? UI_KBD_LANG_BIT(UI_KBD_LANG_ZH) : 0u) |
                                   (ja ? UI_KBD_LANG_BIT(UI_KBD_LANG_JA) : 0u));
    if (k->langs == mask) {
        return;
    }
    k->langs = mask;
    const ui_kbd_lang_t cur = lang_of(k);
    if (cur != UI_KBD_LANG_EN && (mask & UI_KBD_LANG_BIT(cur)) == 0u) {
        // Turned off under the user's hands: fall back to English by the ordinary rule - what is composed is
        // committed, not dropped (a discard belongs to the paths that cannot deliver it at all).
        set_lang(k, UI_KBD_LANG_EN, 0u);
    }
    refresh(k);
}

void ui_view_keyboard_bind_router(ui_view_keyboard_t *k, ui_router_t *router)
{
    if (k != NULL) {
        k->router = router;
    }
}

const ui_overlay_descriptor_t *ui_view_keyboard_overlays(ui_view_keyboard_t *k, uint8_t *n)
{
    if (n != NULL) {
        *n = k != NULL ? UI_KBD_OVERLAYS : 0;
    }
    return k != NULL ? k->ov : NULL;
}

void ui_view_keyboard_set_conv_engine(ui_view_keyboard_t *k, uint8_t layer, const ui_kbd_conv_engine_t *engine)
{
    if (k == NULL || (layer != UI_KBD_LAYER_ZH && layer != UI_KBD_LAYER_JA)) {
        return;
    }
    if (engine != NULL &&
        (engine->search == NULL || engine->candidate == NULL || engine->choose == NULL ||
         engine->cancel_last == NULL || engine->fixed_len == NULL || engine->consumed == NULL ||
         engine->parsed == NULL || engine->complete == NULL || engine->reset == NULL)) {
        engine = NULL; // an incomplete table is no decoder: the cap stays off (fail closed)
    }
    const ui_kbd_conv_engine_t **slot = layer == UI_KBD_LAYER_JA ? &k->ja : &k->zh;
    if (engine != *slot && k->converting && k->conv_layer == layer) {
        drop_conv(k); // the decoder state belongs to the old engine: discard rather than send something else
    }
    *slot = engine;
    if (engine != NULL) {
        engine->reset(engine->ctx);
    }
    zh_update(k);
}

void ui_view_keyboard_set_zh_engine(ui_view_keyboard_t *k, const ui_kbd_conv_engine_t *engine)
{
    ui_view_keyboard_set_conv_engine(k, UI_KBD_LAYER_ZH, engine);
}

void ui_view_keyboard_set_text_mode(ui_view_keyboard_t *k, bool unicode_delivery)
{
    if (k == NULL || k->text_mode_ok == unicode_delivery) {
        return;
    }
    k->text_mode_ok = unicode_delivery;
    if (!unicode_delivery && k->hangul) {
        kvm_hangul_reset(&k->ime); // host-ime cannot carry it: discard rather than send something else
        text_cancel(k, 0u);
        k->hangul = false;
        k->jamo_shift = false;
        update_preedit(k);
        if (k->row_compose != NULL) {
            lv_obj_set_hidden(k->row_compose, true);
        }
    }
    if (!unicode_delivery && k->converting) {
        drop_conv(k); // as above: discarded, never sent some other way
        zh_update(k);
    }
    refresh(k);
}

// ---- input ---------------------------------------------------------------------------------
static void set_fn_layer(ui_view_keyboard_t *k, bool on)
{
    if (k->root == NULL || k->fn_layer == on) {
        return;
    }
    release_held(k, true); // the layer must never be swapped under a key that is still down
    k->fn_layer = on;
    k->c.fn_switches++;
    apply_rows(k);
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        if (s_keys[i].local == LOCAL_FN) {
            ui_action_key_set_checked(k->keys[i], on);
        }
    }
}

static void key_sink(uint16_t control_id, bool pressed, void *user_data)
{
    ui_view_keyboard_t *k = user_data;
    if (k == NULL || control_id < UI_CTRL_KBD_BASE) {
        return;
    }
    if (control_id >= UI_CTRL_KBD_FLICK && control_id <= UI_CTRL_KBD_FLICK + FLICK_FN_ABC) {
        if (pressed && k->converting && k->conv_layer == UI_KBD_LAYER_JA && host_mounted(k)) {
            flick_fn(k, control_id); // taps only, as on the strip
        }
        return;
    }
    if (control_id >= UI_CTRL_KBD_CAND_BASE && control_id <= UI_CTRL_KBD_CAND_NEXT) {
        if (pressed && k->converting && host_mounted(k)) {
            zh_strip(k, control_id); // a candidate is a tap, never a held key
        }
        return;
    }
    const uint32_t idx = (uint32_t)(control_id - UI_CTRL_KBD_BASE);
    if (idx >= KEY_COUNT) {
        return;
    }
    // FR-742/743: what this cap sends is the usage of its slot in the selected layout - still a usage, never a
    // character and never anything read off a legend. The composition layers below keep the US positions, which
    // is what a 2-set Hangul or a romaji key map is defined on.
    const uint8_t usage = live_usage(k, idx);

    if (s_keys[idx].local == LOCAL_FN) { // "F1-F12": a local UI layer, never a HID key (FR-207)
        if (pressed) {
            set_fn_layer(k, !k->fn_layer);
        }
        return;
    }
    if (s_keys[idx].local == LOCAL_LANG) {
        // The language cap (ADR-037), never a HID key. Its gestures are SHORT_CLICKED and LONG_PRESSED, so the
        // press edges are ignored here; the cap still reports them, which keeps its down flag paired.
        return;
    }
    if (k->hangul) {
        if (pressed && host_mounted(k)) {
            hangul_key(k, idx, control_id); // taps only: text items have no held state
        }
        return;
    }
    if (k->converting) {
        if (pressed && host_mounted(k)) {
            zh_key(k, idx, control_id); // taps only, as on the KO layer
        }
        return;
    }
    if (kvm_usage_is_modifier(usage)) {
        // One request per tap; the ARMED/LOCKED decision and the atomic combination belong to the HID core FSM
        // (FR-204/205, ADR-016). The cap's look is not touched here.
        if (pressed && host_mounted(k)) {
            k->c.mod_toggles++;
            (void)submit(k, UI_ACT_MODIFIER_TOGGLE, control_id, usage);
        }
        return;
    }
    if (pressed) {
        release_held(k, false); // FR-211: one deterministic target at a time
        if (!host_mounted(k)) {
            return;
        }
        if (submit(k, UI_ACT_KEY_DOWN, control_id, usage)) {
            k->held = (uint8_t)idx;
            k->c.key_down++;
        }
    } else if (k->held == idx) {
        release_held(k, false);
    }
}

// ---- construction --------------------------------------------------------------------------
static lv_obj_t *make_row(lv_obj_t *parent, int32_t fixed_h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    if (fixed_h > 0) {
        lv_obj_set_height(row, fixed_h);
    } else {
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(row, 1);
    }
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, UI_SPACE_XS, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    return row;
}

static lv_obj_t *make_key(ui_view_keyboard_t *k, lv_obj_t *row, uint32_t idx)
{
    const kbd_cap_t *e = &s_keys[idx];
    lv_obj_t *key = ui_action_key_create(row, &e->desc, &k->sink);
    if (key == NULL) {
        return NULL;
    }
    lv_obj_set_flex_grow(key, e->grow); // the reference key weight; width stays Flex-driven
    lv_obj_set_height(key, lv_pct(100));
    lv_obj_set_style_pad_hor(key, UI_SPACE_XS, 0); // caps are narrow: the legend gets the room
    lv_obj_set_style_pad_ver(key, UI_SPACE_XS, 0);
    if (kvm_usage_is_modifier(e->usage)) {
        style_locked_look(key);
    }
    // The main legend, taken before this view adds its own corners: after them the cap's last child is no longer
    // the legend (which is what ui_action_key_set_label() would reach for).
    k->cap_label[idx] = e->desc.label != NULL ? lv_obj_get_child(key, -1) : NULL;
    // FR-742: a cap that owns a key slot gets its Shift and AltGr corners once, here, and apply_layout() only ever
    // changes their text - a layout change costs no allocation and no widget.
    if (ui_view_keyboard_key_slot(idx) != KBD_SLOT_COUNT) {
        lv_obj_t *shift = lv_label_create(key);
        lv_obj_t *altgr = lv_label_create(key);
        if (shift == NULL || altgr == NULL) {
            return NULL;
        }
        lv_obj_t *const corner[2] = {shift, altgr};
        for (int c = 0; c < 2; c++) {
            lv_label_set_text_static(corner[c], "");
            lv_obj_set_style_text_font(corner[c], ui_theme_font(UI_FONT_LABEL), 0);
            lv_obj_set_style_text_color(corner[c], lv_color_hex(UI_COLOR_SUBTLE), 0);
            lv_obj_set_ignore_layout(corner[c], true);
            lv_obj_align(corner[c], c == 0 ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_BOTTOM_RIGHT, 0, 0);
            lv_obj_set_hidden(corner[c], true);
        }
        k->sub[idx] = shift;
        k->alt[idx] = altgr;
        set_sub(shift, e->sub != NULL ? e->sub : ""); // the US shift sub-legends of the number row, until a layout
    }
    if (e->sub != NULL && k->sub[idx] == NULL) {
        // Reference `.key-sub`: outside the Flex flow, pinned to the top right corner of the cap.
        lv_obj_t *sub = lv_label_create(key);
        lv_label_set_text_static(sub, e->sub);
        lv_obj_set_style_text_font(sub, ui_theme_font(UI_FONT_LABEL), 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(UI_COLOR_SUBTLE), 0);
        lv_obj_set_ignore_layout(sub, true);
        lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, 0, 0);
        if (e->usage == KVM_KEY_CAPS_LOCK) {
            k->caps_sub = sub;
        }
    }
    return key;
}

/** The HOST-lost notice: same wording and same card as the MOUSE view (07 §7 - one look per message kind). */
static lv_obj_t *make_offline(lv_obj_t *root)
{
    lv_obj_t *card = lv_obj_create(root);
    if (card == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, false); // no key is reachable anyway; the notice must not swallow a press either
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ignore_layout(card, true); // the key rows keep the whole field
    lv_obj_center(card);

    lv_obj_t *title = lv_label_create(card);
    ui_theme_apply(title, UI_STYLE_TEXT_HEADING, LV_STATE_DEFAULT);
    lv_label_set_text_static(title, "Host disconnected");
    lv_obj_t *sub = lv_label_create(card);
    ui_theme_apply(sub, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
    lv_label_set_text_static(sub, "Input will resume when the connection returns.");
    return card;
}

static lv_obj_t *create_cb(void *ctx, lv_obj_t *slot)
{
    ui_view_keyboard_t *k = ctx;
    if (!ui_theme_is_ready() && !ui_theme_init()) {
        return NULL; // no shared styles, no caps: report the failure instead of drawing an unstyled field
    }
    lv_obj_t *root = lv_obj_create(slot);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    ui_theme_apply(root, UI_STYLE_SCREEN, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(root, UI_SPACE_S, 0);
    lv_obj_set_style_pad_row(root, UI_SPACE_XS, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_clickable(root, false);

    // KO layer strip (FR-746): the preedit, shown only while the layer is on (Flex skips a hidden row).
    lv_obj_t *compose = make_row(root, UI_TOUCH_MIN);
    if (compose == NULL) {
        lv_obj_delete(root);
        return NULL;
    }
    lv_obj_set_flex_align(compose, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(compose, UI_SPACE_M, 0);
    lv_obj_t *tag = lv_label_create(compose);
    ui_theme_apply(tag, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
    lv_label_set_text_static(tag, "Korean (2-set), composed here:");
    lv_obj_t *pre = lv_label_create(compose);
    lv_obj_set_style_text_font(pre, ui_theme_font(UI_FONT_HANGUL), 0);
    lv_obj_set_style_text_color(pre, lv_color_hex(UI_COLOR_ACCENT_INK), 0);
    k->preedit_text[0] = '\0';
    lv_label_set_text_static(pre, k->preedit_text);
    lv_obj_set_hidden(compose, true);

    // ZH layer strip (FR-746): the input, then the candidates and the paging keys, shown only while it is on.
    lv_obj_t *zh = make_row(root, UI_TOUCH_PREFERRED);
    if (zh == NULL) {
        lv_obj_delete(root);
        return NULL;
    }
    lv_obj_set_style_pad_column(zh, UI_SPACE_S, 0);
    lv_obj_t *zin = lv_label_create(zh);
    lv_obj_set_style_text_font(zin, ui_theme_font(UI_FONT_HANZI), 0);
    lv_obj_set_style_text_color(zin, lv_color_hex(UI_COLOR_ACCENT_INK), 0);
    lv_obj_set_style_max_width(zin, lv_pct(45), 0);
    lv_label_set_long_mode(zin, LV_LABEL_LONG_MODE_CLIP);
    k->zh_input_text[0] = '\0';
    lv_label_set_text_static(zin, k->zh_input_text);
    // Bound to the view only once the whole tree exists: a failed create leaves no pointer to a deleted widget.
    lv_obj_t *zh_keys[UI_VIEW_KBD_CANDS + 3u];
    for (uint32_t i = 0; i < UI_VIEW_KBD_CANDS + 3u; i++) {
        ui_action_key_desc_t *d = &k->zh_desc[i];
        const bool cand = i < UI_VIEW_KBD_CANDS;
        const bool toggle = i == UI_VIEW_KBD_CANDS + 2u; // JA: Flick / QWERTY (control id UI_CTRL_KBD_FLICK)
        *d = (ui_action_key_desc_t){
            .label = cand ? "" : (toggle ? "Flick" : NULL),
            .icon = (cand || toggle) ? NULL : (i == UI_VIEW_KBD_CANDS ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT),
            .control_id = (uint16_t)(UI_CTRL_KBD_CAND_BASE + i),
            .variant = cand ? UI_KEY_NORMAL : UI_KEY_MODIFIER,
            .width = cand ? 0 : (toggle ? 2 * UI_TOUCH_PREFERRED : UI_TOUCH_PREFERRED),
            .height = UI_TOUCH_PREFERRED,
            .font = cand ? UI_FONT_HANZI : UI_FONT_BODY,
        };
        lv_obj_t *key = ui_action_key_create(zh, d, &k->sink);
        if (key == NULL) {
            lv_obj_delete(root);
            return NULL;
        }
        lv_obj_set_style_pad_hor(key, UI_SPACE_XS, 0);
        zh_keys[i] = key;
    }
    lv_obj_set_hidden(zh, true);

    lv_obj_t *rows[ROW_COUNT];
    rows[ROW_UTIL] = make_row(root, UTIL_ROW_H);
    rows[ROW_NUMBER] = make_row(root, 0);
    rows[ROW_FN] = make_row(root, 0);
    rows[ROW_TOP] = make_row(root, 0);
    rows[ROW_MID] = make_row(root, 0);
    rows[ROW_LOW] = make_row(root, 0);
    rows[ROW_BOTTOM] = make_row(root, 0);
    for (int i = 0; i < (int)ROW_COUNT; i++) {
        if (rows[i] == NULL) {
            lv_obj_delete(root);
            return NULL;
        }
    }
    lv_obj_set_hidden(rows[ROW_FN], true); // the layer starts off (FR-207)

    // FR-742 and ADR-039 section 8: the layout the field is drawn for, at the end of the utility row, as a
    // control rather than a line of text - a keyboard icon, the short id, the name and a chevron. It reads as
    // something to press, it is a full touch target, and it is hidden while no layout is in force.
    lv_obj_t *hint = lv_button_create(rows[ROW_UTIL]);
    if (hint == NULL) {
        lv_obj_delete(root);
        return NULL;
    }
    lv_obj_remove_style_all(hint);
    ui_theme_apply_button(hint, UI_BTN_QUIET);
    lv_obj_set_height(hint, UI_TOUCH_MIN);
    lv_obj_set_width(hint, LV_SIZE_CONTENT);
    // The row's own width is the ceiling: the name is shortened with an ellipsis instead of running off the
    // right edge of the panel, which is what the plain label used to do.
    lv_obj_set_style_max_width(hint, lv_pct(42), 0);
    lv_obj_set_style_pad_hor(hint, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_style_pad_column(hint, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_set_scrollable(hint, false);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *strip_icon = lv_label_create(hint);
    lv_obj_t *strip_code = lv_label_create(hint);
    lv_obj_t *strip_chev = lv_label_create(hint);
    if (strip_icon == NULL || strip_code == NULL || strip_chev == NULL) {
        lv_obj_delete(root);
        return NULL;
    }
    ui_theme_apply(strip_icon, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
    ui_theme_apply(strip_code, UI_STYLE_TEXT_LABEL, LV_STATE_DEFAULT);
    ui_theme_apply(strip_chev, UI_STYLE_TEXT_MUTED, LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(strip_icon, ui_theme_font(UI_FONT_LABEL), 0);
    lv_obj_set_style_text_font(strip_code, ui_theme_font(UI_FONT_LABEL), 0);
    lv_obj_set_style_text_font(strip_chev, ui_theme_font(UI_FONT_LABEL), 0);
    // LV_SYMBOL_* are in every built-in Montserrat subset this theme ships, so no icon has to be drawn here.
    lv_label_set_text_static(strip_icon, LV_SYMBOL_KEYBOARD);
    lv_label_set_text_static(strip_chev, LV_SYMBOL_DOWN);
    k->strip_code_text[0] = '\0';
    k->strip_name_text[0] = '\0';
    lv_label_set_text_static(strip_code, k->strip_code_text);
    // The children are decoration: the press belongs to the strip, whichever part of it the finger lands on.
    lv_obj_set_clickable(strip_icon, false);
    lv_obj_set_clickable(strip_code, false);
    lv_obj_set_clickable(strip_chev, false);
    k->strip_code = strip_code;
    // A tap and a long press both open the quick switch: the chevron promises a list, so the ordinary tap has
    // to deliver it. There is no cycling here, so neither gesture can change a layout by itself.
    lv_obj_add_event_cb(hint, layout_strip_cb, LV_EVENT_SHORT_CLICKED, k);
    lv_obj_add_event_cb(hint, layout_strip_cb, LV_EVENT_LONG_PRESSED, k);
    lv_obj_set_hidden(hint, true);

    // JA flick pad: four rows in the place of the number and letter rows (Flex grow 4 for the 4 rows it hides).
    lv_obj_t *pad = lv_obj_create(root);
    if (pad == NULL) {
        lv_obj_delete(root);
        return NULL;
    }
    lv_obj_remove_style_all(pad);
    lv_obj_set_width(pad, lv_pct(100));
    lv_obj_set_flex_grow(pad, 4);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pad, UI_SPACE_XS, 0);
    lv_obj_set_scrollable(pad, false);
    lv_obj_set_clickable(pad, false);
    lv_obj_move_to_index(pad, (int32_t)lv_obj_get_index(rows[ROW_BOTTOM])); // above the bottom row
    static const char *const k_pad_fn[4] = {NULL, "Space", "Enter", "ABC"};
    lv_obj_t *pad_keys[UI_VIEW_KBD_FLICK_KEYS] = {0};
    lv_obj_t *pad_labels[UI_VIEW_KBD_FLICK_KEYS] = {0};
    for (uint32_t r = 0; r < 4u; r++) {
        lv_obj_t *prow = make_row(pad, 0);
        if (prow == NULL) {
            lv_obj_delete(root);
            return NULL;
        }
        for (uint32_t c = 0; c < 4u; c++) {
            const uint32_t g = r * 4u + c;
            lv_obj_t *key;
            if (c == 3u) {
                ui_action_key_desc_t *d = &k->flick_desc[r];
                *d = (ui_action_key_desc_t){
                    .label = k_pad_fn[r],
                    .icon = r == 0u ? LV_SYMBOL_BACKSPACE : NULL,
                    .control_id = (uint16_t)(UI_CTRL_KBD_FLICK + 1u + r),
                    .variant = UI_KEY_MODIFIER,
                    .width = 0,
                    .height = 0,
                    .font = UI_FONT_BODY,
                };
                key = ui_action_key_create(prow, d, &k->sink);
            } else {
                key = lv_obj_create(prow);
                if (key != NULL) {
                    lv_obj_remove_style_all(key);
                    ui_theme_apply_key(key, UI_KEY_NORMAL);
                    lv_obj_set_press_lock(key, true); // a flick may leave the key; it still ends here
                    lv_obj_set_scrollable(key, false);
                    lv_obj_t *lab = lv_label_create(key);
                    lv_obj_set_style_text_font(lab, ui_theme_font(UI_FONT_KANJI), 0);
                    lv_label_set_text_static(lab, k_pad_tap[r * 3u + c]);
                    lv_obj_center(lab);
                    pad_labels[g] = lab;
                    lv_obj_add_event_cb(key, flick_event_cb, LV_EVENT_ALL, k);
                }
            }
            if (key == NULL) {
                lv_obj_delete(root);
                return NULL;
            }
            lv_obj_set_flex_grow(key, c == 3u ? 2 : 3);
            lv_obj_set_height(key, lv_pct(100));
            pad_keys[g] = key;
        }
    }
    lv_obj_set_hidden(pad, true);

    k->caps_sub = NULL;
    k->lang_key = NULL;
    memset(k->cap_label, 0, sizeof(k->cap_label));
    memset(k->sub, 0, sizeof(k->sub));
    memset(k->alt, 0, sizeof(k->alt));
    memset(k->usage_live, 0, sizeof(k->usage_live));
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        k->keys[i] = make_key(k, rows[s_keys[i].row], i);
        if (k->keys[i] == NULL) {
            memset(k->keys, 0, sizeof(k->keys));
            memset(k->cap_label, 0, sizeof(k->cap_label));
            memset(k->sub, 0, sizeof(k->sub));
            memset(k->alt, 0, sizeof(k->alt));
            k->caps_sub = NULL;
            k->lang_key = NULL;
            lv_obj_delete(root);
            return NULL;
        }
        if (geometry_cap(i)) {
            // FR-742: hidden until a layout whose family has this key is selected, and placed where that family
            // puts it - the table appends these caps, so without the move they would sit at the end of the row.
            lv_obj_set_hidden(k->keys[i], true);
            for (uint32_t j = 0; j < i; j++) {
                if (k->keys[j] != NULL && s_keys[j].usage == s_keys[i].anchor &&
                    lv_obj_get_parent(k->keys[j]) == lv_obj_get_parent(k->keys[i])) {
                    lv_obj_move_to_index(k->keys[i], (int32_t)lv_obj_get_index(k->keys[j]));
                    break;
                }
            }
        }
        if (s_keys[i].local == LOCAL_LANG) {
            // SHORT_CLICKED fires only when no long press was sent, so the two gestures never overlap.
            k->lang_key = k->keys[i];
            lv_obj_add_event_cb(k->lang_key, lang_tap_cb, LV_EVENT_SHORT_CLICKED, k);
            lv_obj_add_event_cb(k->lang_key, lang_long_cb, LV_EVENT_LONG_PRESSED, k);
        }
    }

    // FR-604: a disabled key field is not an explanation. The same notice the MOUSE view shows, built last so
    // it draws over the caps, and outside the Flex flow so it displaces no row.
    k->offline = make_offline(root);
    if (k->offline == NULL) {
        memset(k->keys, 0, sizeof(k->keys));
        k->caps_sub = NULL;
        k->lang_key = NULL;
        lv_obj_delete(root);
        return NULL;
    }

    k->root = root;
    k->row_compose = compose;
    k->preedit = pre;
    k->row_zh = zh;
    k->zh_input = zin;
    memcpy(k->zh_cand, zh_keys, sizeof(k->zh_cand));
    k->zh_prev = zh_keys[UI_VIEW_KBD_CANDS];
    k->zh_next = zh_keys[UI_VIEW_KBD_CANDS + 1u];
    k->flick_toggle = zh_keys[UI_VIEW_KBD_CANDS + 2u];
    k->flick_pad = pad;
    memcpy(k->flick_key, pad_keys, sizeof(k->flick_key));
    memcpy(k->flick_label, pad_labels, sizeof(k->flick_label));
    k->flick = false;
    k->flick_down = -1;
    k->flick_n = 0;
    k->hangul = false;
    k->jamo_shift = false;
    k->converting = false;
    k->zh_shift = false;
    k->py_len = 0;
    k->py[0] = '\0';
    k->zh_count = 0;
    k->zh_page = 0;
    k->conv_layer = 0u;
    k->conv = NULL;
    if (k->zh != NULL) {
        k->zh->reset(k->zh->ctx);
    }
    if (k->ja != NULL) {
        k->ja->reset(k->ja->ctx);
    }
    kvm_hangul_reset(&k->ime);
    k->row_util = rows[ROW_UTIL];
    k->row_number = rows[ROW_NUMBER];
    k->row_fn = rows[ROW_FN];
    k->row_top = rows[ROW_TOP];
    k->row_mid = rows[ROW_MID];
    k->row_low = rows[ROW_LOW];
    k->row_bottom = rows[ROW_BOTTOM];
    k->fn_layer = false;
    k->keys_enabled = true; // UiActionKey caps are created enabled; refresh() below reconciles it
    k->hint = hint;
    lv_obj_move_to_index(hint, -1); // after the caps of the utility row, which were created later
    apply_rows(k);
    apply_layout(k); // FR-742: draw the field for the layout Settings selected, if it already did
    k->held = UI_VIEW_KBD_NO_KEY;

    // Observers die with their object (AT-UIFW-09): bound to the root, so deleting the view detaches them.
    lv_subject_add_observer_obj(ui_store_subject(k->store, UI_SUBJ_HOST_USB), subject_cb, root, k);
    lv_subject_add_observer_obj(ui_store_subject(k->store, UI_SUBJ_MODS_ARMED), subject_cb, root, k);
    lv_subject_add_observer_obj(ui_store_subject(k->store, UI_SUBJ_MODS_LOCKED), subject_cb, root, k);
    lv_subject_add_observer_obj(ui_store_subject(k->store, UI_SUBJ_CAPS_LED), subject_cb, root, k);
    refresh(k);
    return root;
}

static void on_enter_cb(void *ctx)
{
    refresh(ctx);
}

static void drop_composition(ui_view_keyboard_t *k)
{
    if (k->hangul) {
        kvm_hangul_reset(&k->ime); // 07: a view change cancels what is pending; nothing half-typed is sent
        text_cancel(k, 0u);
        update_preedit(k);
    }
    if (k->converting) {
        zh_clear(k); // the same for the conversion input and its choices
        text_cancel(k, 0u);
        zh_update(k);
    }
}

static void on_leave_cb(void *ctx)
{
    release_held(ctx, true);
    drop_composition(ctx);
}

static void destroy_cb(void *ctx)
{
    ui_view_keyboard_t *k = ctx;
    release_held(k, true);
    drop_composition(k);
    k->hangul = false;
    k->jamo_shift = false;
    k->converting = false;
    k->conv_layer = 0u;
    k->conv = NULL;
    k->zh_shift = false;
    k->row_compose = NULL;
    k->preedit = NULL;
    k->row_zh = NULL;
    k->zh_input = NULL;
    memset(k->zh_cand, 0, sizeof(k->zh_cand));
    k->zh_prev = k->zh_next = NULL;
    k->flick_toggle = NULL;
    k->flick_pad = NULL;
    memset(k->flick_key, 0, sizeof(k->flick_key));
    memset(k->flick_label, 0, sizeof(k->flick_label));
    k->flick = false;
    k->flick_down = -1;
    if (k->toast != NULL) {
        ui_toast_dismiss(k->toast); // the toast lives on the shell's layer: it must not outlive the view
    }
    k->lang_key = NULL;
    k->lang_card = NULL; // the router's teardown calls lang_on_close(); nulling it here is safe either way
    memset(k->pick_btn, 0, sizeof(k->pick_btn));
    k->root = NULL;
    k->row_util = k->row_number = k->row_fn = k->row_top = k->row_mid = k->row_low = k->row_bottom = NULL;
    k->caps_sub = NULL;
    k->offline = NULL;
    k->hint = NULL;
    k->hint_text[0] = '\0';
    k->strip_code = NULL;
    k->strip_code_text[0] = '\0';
    k->strip_name_text[0] = '\0';
    k->layout_card = NULL;
    memset(k->layout_pick, 0, sizeof(k->layout_pick));
    k->layout_pick_count = 0u;
    memset(k->keys, 0, sizeof(k->keys));
    memset(k->cap_label, 0, sizeof(k->cap_label));
    memset(k->sub, 0, sizeof(k->sub));
    memset(k->alt, 0, sizeof(k->alt));
    memset(k->usage_live, 0, sizeof(k->usage_live)); // the selection itself (k->layout) survives the teardown
    k->fn_layer = false;
    k->keys_enabled = false;
}

void ui_view_keyboard_init(ui_view_keyboard_t *k, ui_store_t *store, ui_actions_t *actions,
                           uint64_t (*now_us)(void))
{
    memset(k, 0, sizeof(*k));
    k->store = store;
    k->actions = actions;
    k->now_us = now_us;
    k->held = UI_VIEW_KBD_NO_KEY;
    k->langs = UI_KBD_LANG_BIT(UI_KBD_LANG_EN); // ADR-037: a fresh device types English alone
    kvm_hangul_reset(&k->ime);
    k->sink = (ui_action_key_sink_t){.fn = key_sink, .user_data = k};
    for (uint32_t i = 0; i < KEY_COUNT; i++) {
        s_keys[i].desc.control_id = (uint16_t)(UI_CTRL_KBD_BASE + i);
    }
    k->ov[0] = (ui_overlay_descriptor_t){
        .id = UI_OVERLAY_KBD_LANG,
        .create = lang_build,
        .on_close = lang_on_close,
        .blocking = true, // a tap meant for the card must never reach a cap underneath it
        .ctx = k,
    };
    k->ov[1] = (ui_overlay_descriptor_t){
        .id = UI_OVERLAY_KBD_LAYOUT,
        .create = layout_build,
        .on_close = layout_on_close,
        .blocking = true, // ADR-039 section 8: a tap meant for a tile must never reach a cap underneath it
        .ctx = k,
    };
    k->desc = (ui_view_descriptor_t){
        .id = UI_VIEW_KEYBOARD,
        .create = create_cb,
        .on_enter = on_enter_cb,
        .on_leave = on_leave_cb,
        .destroy = destroy_cb,
        .pad_obj = NULL, // no touch surface on this screen (07 §6)
        .ctx = k,
    };
}
