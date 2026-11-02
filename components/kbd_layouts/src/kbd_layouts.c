// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Accessors over the generated layout tables (kbd_layouts.h). Reads flash in place; no allocation.
#include "kbd_layouts.h"

#include <string.h>

#ifndef KBD_LAYOUTS_HAVE_TABLES
#define KBD_LAYOUTS_HAVE_TABLES 1
#endif

#if !KBD_LAYOUTS_HAVE_TABLES
// The generator has not run in this tree: no layout is shipped, every lookup fails cleanly.
const uint16_t kbd_layouts_count = 0u;
const char kbd_layouts_legend_pool[1] = "";
const kbd_key_t kbd_layouts_keys[1] = {{0u, 0u, {0u, 0u, 0u, 0u}}};
const uint16_t kbd_layouts_slots[1][KBD_SLOT_COUNT] = {{0u}};
const kbd_layout_t kbd_layouts_table[1] = {{"", "", 0u, 0u, 0u}};
#endif

#define NO_KEY 0xFFFFu
#define MOD_SHIFT 0x02u
#define MOD_ALTGR 0x40u // Right Alt (03 §5.2 bit order)

size_t kbd_layouts_count_get(void)
{
    return kbd_layouts_count;
}

const kbd_layout_t *kbd_layouts_at(size_t index)
{
    return index < kbd_layouts_count ? &kbd_layouts_table[index] : NULL;
}

const kbd_layout_t *kbd_layouts_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    size_t lo = 0, hi = kbd_layouts_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2u;
        const int c = strcmp(kbd_layouts_table[mid].id, id); // ids are sorted in byte order by the generator
        if (c == 0) {
            return &kbd_layouts_table[mid];
        }
        if (c < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static bool valid_layout(const kbd_layout_t *layout)
{
    return layout != NULL && layout >= kbd_layouts_table && layout < kbd_layouts_table + kbd_layouts_count;
}

const kbd_key_t *kbd_layouts_key(const kbd_layout_t *layout, kbd_slot_t slot)
{
    if (!valid_layout(layout) || (unsigned)slot >= KBD_SLOT_COUNT) {
        return NULL;
    }
    const uint16_t k = kbd_layouts_slots[layout->row][slot];
    return k == NO_KEY ? NULL : &kbd_layouts_keys[k];
}

const char *kbd_layouts_legend(const kbd_key_t *key, unsigned level)
{
    if (key == NULL || level >= KBD_LEVELS) {
        return "";
    }
    return &kbd_layouts_legend_pool[key->legend[level]];
}

// One UTF-8 sequence that is exactly one code point; returns that code point or 0.
static uint32_t single_cp(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t cp;
    unsigned n;
    if (p[0] == 0u) {
        return 0u;
    }
    if (p[0] < 0x80u) {
        cp = p[0];
        n = 0;
    } else if ((p[0] & 0xE0u) == 0xC0u) {
        cp = p[0] & 0x1Fu;
        n = 1;
    } else if ((p[0] & 0xF0u) == 0xE0u) {
        cp = p[0] & 0x0Fu;
        n = 2;
    } else if ((p[0] & 0xF8u) == 0xF0u) {
        cp = p[0] & 0x07u;
        n = 3;
    } else {
        return 0u;
    }
    for (unsigned i = 1; i <= n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            return 0u;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    return p[n + 1u] == 0u ? cp : 0u;
}

bool kbd_layouts_keystroke(const kbd_layout_t *layout, uint32_t cp, kvm_keystroke_t *out)
{
    if (!valid_layout(layout) || out == NULL || cp < 0x20u || cp == 0x7Fu) {
        return false;
    }
    const unsigned levels = (layout->flags & KBD_LAYOUT_ALTGR) ? KBD_LEVELS : 2u;
    for (unsigned lv = 0; lv < levels; lv++) {
        for (unsigned s = 0; s < KBD_SLOT_COUNT; s++) {
            const kbd_key_t *k = kbd_layouts_key(layout, (kbd_slot_t)s);
            if (k == NULL || (k->flags & KBD_KEY_DEAD_LEVEL(lv)) != 0u) {
                continue; // the legend is the accent of a dead key: it latches on the HOST, it types nothing
            }
            if (single_cp(kbd_layouts_legend(k, lv)) == cp) {
                out->usage = k->usage;
                out->mods = (uint8_t)(((lv & 1u) ? MOD_SHIFT : 0u) | ((lv & 2u) ? MOD_ALTGR : 0u));
                return true;
            }
        }
    }
    return false;
}

bool kbd_layouts_uni_key(void *ctx, char c, kvm_keystroke_t *out)
{
    if (ctx == NULL) {
        return kvm_uni_key_us(NULL, c, out);
    }
    return kbd_layouts_keystroke((const kbd_layout_t *)ctx, (uint8_t)c, out);
}
