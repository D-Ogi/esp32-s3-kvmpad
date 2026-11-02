// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Generated HOST layout tables (ADR-024 FR-741..743, TECH-077..079): for every shipped `layout(variant)` of
// xkeyboard-config, the geometry family and, per key slot, the HID usage, up to four legends (Base, Shift, AltGr,
// Shift+AltGr; UTF-8, <= 8 bytes) and flags. The tables are produced by tools/kbd_layouts/gen_layouts.py into
// generated/kbd_layouts_data.c and read in place from flash: no allocation, no parser (07 §7).
//
// Legends are decoration: a key sends its usage whatever the legend says (FR-743). The character lookup below is
// the "can the layout type it directly" test of FR-747; it never derives a keymap from key text for the key field.
// Without generated tables (a tree in which the generator has not run yet) the accessor reports zero layouts.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbd_unicode.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Key slots, in the generator's SLOTS order (evdev key names). */
typedef enum {
    KBD_SLOT_TLDE = 0,
    KBD_SLOT_AE01, KBD_SLOT_AE02, KBD_SLOT_AE03, KBD_SLOT_AE04, KBD_SLOT_AE05, KBD_SLOT_AE06, KBD_SLOT_AE07,
    KBD_SLOT_AE08, KBD_SLOT_AE09, KBD_SLOT_AE10, KBD_SLOT_AE11, KBD_SLOT_AE12, KBD_SLOT_AE13,
    KBD_SLOT_AD01, KBD_SLOT_AD02, KBD_SLOT_AD03, KBD_SLOT_AD04, KBD_SLOT_AD05, KBD_SLOT_AD06, KBD_SLOT_AD07,
    KBD_SLOT_AD08, KBD_SLOT_AD09, KBD_SLOT_AD10, KBD_SLOT_AD11, KBD_SLOT_AD12,
    KBD_SLOT_BKSL,
    KBD_SLOT_AC01, KBD_SLOT_AC02, KBD_SLOT_AC03, KBD_SLOT_AC04, KBD_SLOT_AC05, KBD_SLOT_AC06, KBD_SLOT_AC07,
    KBD_SLOT_AC08, KBD_SLOT_AC09, KBD_SLOT_AC10, KBD_SLOT_AC11,
    KBD_SLOT_LSGT,
    KBD_SLOT_AB01, KBD_SLOT_AB02, KBD_SLOT_AB03, KBD_SLOT_AB04, KBD_SLOT_AB05, KBD_SLOT_AB06, KBD_SLOT_AB07,
    KBD_SLOT_AB08, KBD_SLOT_AB09, KBD_SLOT_AB10, KBD_SLOT_AB11,
    KBD_SLOT_SPCE,
    KBD_SLOT_MUHE, KBD_SLOT_HENK, KBD_SLOT_HKTG, KBD_SLOT_HNGL, KBD_SLOT_HJCV,
    KBD_SLOT_COUNT
} kbd_slot_t;

/** Geometry family (FR-742). */
typedef enum { KBD_GEOM_ANSI = 0, KBD_GEOM_ISO, KBD_GEOM_JIS, KBD_GEOM_ABNT2, KBD_GEOM_KR, KBD_GEOM_COUNT } kbd_geometry_t;

#define KBD_LEVELS 4
#define KBD_LEGEND_MAX 8

#define KBD_KEY_DEAD 0x01u // at least one level is a dead key (the legend shows the accent)
#define KBD_KEY_DEAD_LEVEL(lv) (0x10u << (lv)) // level lv (0..3) is a dead key
#define KBD_KEY_IME 0x02u  // an IME key (Henkan, Muhenkan, Hiragana/Katakana, Zenkaku/Hankaku, Hangul, Hanja)

#define KBD_LAYOUT_ALTGR 0x01u       // Right Alt is AltGr (levels 3 and 4 exist)
#define KBD_LAYOUT_BEST_EFFORT 0x02u // a script drawn without shaping (TECH-078)

/** One key: usage (page 0x07), flags and legend offsets into kbd_layouts_legend_pool (0 = no legend). */
typedef struct {
    uint8_t usage;
    uint8_t flags;
    uint16_t legend[KBD_LEVELS];
} kbd_key_t;

/** One layout: xkb id `layout(variant)`, English name from base.xml, geometry, flags, row in kbd_layouts_slots. */
typedef struct {
    const char *id;
    const char *name;
    uint8_t geometry;
    uint8_t flags;
    uint16_t row;
} kbd_layout_t;

// Generated data (generated/kbd_layouts_data.c); use the accessors below.
extern const uint16_t kbd_layouts_count;
extern const char kbd_layouts_legend_pool[];
extern const kbd_key_t kbd_layouts_keys[];
extern const uint16_t kbd_layouts_slots[][KBD_SLOT_COUNT];
extern const kbd_layout_t kbd_layouts_table[];

/** Number of shipped layouts (0 when the generated tables are absent). */
size_t kbd_layouts_count_get(void);

/** Layout by index, sorted by id in byte order; NULL past the end. */
const kbd_layout_t *kbd_layouts_at(size_t index);

/** Layout by xkb id ("us", "de(nodeadkeys)"); NULL when the id is not shipped. Binary search, no allocation. */
const kbd_layout_t *kbd_layouts_find(const char *id);

/** The key in `slot` of `layout`; NULL when the layout has no such key (e.g. LSGT on ANSI, the IME keys). */
const kbd_key_t *kbd_layouts_key(const kbd_layout_t *layout, kbd_slot_t slot);

/** Legend of `key` at `level` (0..3); "" when there is none. Never NULL. */
const char *kbd_layouts_legend(const kbd_key_t *key, unsigned level);

/**
 * How `layout` types the code point `cp` with one keystroke: a usage plus Shift (0x02) and / or AltGr (Right Alt,
 * 0x40), 03 §5.2 bit order. Lower level and earlier slot win. Dead-key levels never match. False when the layout
 * cannot type it directly (the caller then uses the FR-747 Unicode delivery mode).
 */
bool kbd_layouts_keystroke(const kbd_layout_t *layout, uint32_t cp, kvm_keystroke_t *out);

/** kvm_uni_key_fn adapter over kbd_layouts_keystroke(); `ctx` is the kbd_layout_t (NULL = US positions). */
bool kbd_layouts_uni_key(void *ctx, char c, kvm_keystroke_t *out);

#ifdef __cplusplus
}
#endif
