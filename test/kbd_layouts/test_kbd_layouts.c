// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-071 accessors over the generated layout tables (AT-EXT-22). Built twice:
// - KBD_TEST_FAKE: tables emitted by tools/kbd_layouts/test_gen_layouts.py from five fake layouts; exact values;
// - otherwise: the committed generated/kbd_layouts_data.c; invariants over every shipped layout plus the five
//   geometry representatives of AT-EXT-23 (us, de, jp, kr, br).
#include <stdint.h>
#include <string.h>

#include "kbd_layouts.h"
#include "kvm_test.h"

static size_t utf8_len(const char *s)
{
    return strlen(s);
}

static void test_invariants(void)
{
    const size_t n = kbd_layouts_count_get();
    CHECK(n > 0u);
    CHECK(kbd_layouts_at(n) == NULL);
    unsigned keys = 0, dead = 0, ime = 0;
    for (size_t i = 0; i < n; i++) {
        const kbd_layout_t *l = kbd_layouts_at(i);
        CHECK(l != NULL && l->id != NULL && l->name != NULL && l->id[0] != '\0' && l->name[0] != '\0');
        CHECK(l->geometry < KBD_GEOM_COUNT);
        CHECK(kbd_layouts_find(l->id) == l);
        if (i > 0) {
            CHECK(strcmp(kbd_layouts_at(i - 1u)->id, l->id) < 0); // sorted, unique
        }
        for (unsigned s = 0; s < KBD_SLOT_COUNT; s++) {
            const kbd_key_t *k = kbd_layouts_key(l, (kbd_slot_t)s);
            if (k == NULL) {
                continue;
            }
            keys++;
            dead += (k->flags & KBD_KEY_DEAD) != 0u;
            ime += (k->flags & KBD_KEY_IME) != 0u;
            CHECK(k->usage >= 0x04u && k->usage <= 0xA4u); // one page-0x07 key usage, never a modifier
            for (unsigned lv = 0; lv < KBD_LEVELS; lv++) {
                CHECK(utf8_len(kbd_layouts_legend(k, lv)) <= KBD_LEGEND_MAX);
            }
        }
        // the letter rows exist in every shipped layout
        CHECK(kbd_layouts_key(l, KBD_SLOT_AD01) != NULL && kbd_layouts_key(l, KBD_SLOT_AB07) != NULL);
    }
    printf("    %zu layouts, %u keys, %u with dead levels, %u IME keys\n", n, keys, dead, ime);
}

static void test_lookup_edges(void)
{
    CHECK(kbd_layouts_find(NULL) == NULL);
    CHECK(kbd_layouts_find("") == NULL);
    CHECK(kbd_layouts_find("no-such-layout") == NULL);
    const kbd_layout_t bogus = {"us", "x", 0u, 0u, 0u};
    CHECK(kbd_layouts_key(&bogus, KBD_SLOT_AE01) == NULL); // not a table entry
    CHECK(kbd_layouts_key(NULL, KBD_SLOT_AE01) == NULL);
    CHECK(strcmp(kbd_layouts_legend(NULL, 0), "") == 0);
    kvm_keystroke_t ks;
    CHECK(!kbd_layouts_keystroke(NULL, 'a', &ks));
    CHECK(kbd_layouts_uni_key(NULL, 'u', &ks) && ks.usage == 0x18u && ks.mods == 0u); // US positions
}

static void test_representatives(void)
{
    static const struct {
        const char *id;
        kbd_geometry_t geom;
    } reps[] = {{"us", KBD_GEOM_ANSI}, {"de", KBD_GEOM_ISO}, {"jp", KBD_GEOM_JIS}, {"kr", KBD_GEOM_KR}, {"br", KBD_GEOM_ABNT2}};
    for (size_t i = 0; i < sizeof(reps) / sizeof(reps[0]); i++) {
        const kbd_layout_t *l = kbd_layouts_find(reps[i].id);
        CHECK(l != NULL);
        if (l == NULL) {
            continue;
        }
        printf("    %s: %s, geometry %u, flags 0x%02x\n", l->id, l->name, l->geometry, l->flags);
        CHECK_EQ(l->geometry, reps[i].geom);
    }
    const kbd_layout_t *us = kbd_layouts_find("us"), *de = kbd_layouts_find("de"), *jp = kbd_layouts_find("jp");
    const kbd_layout_t *kr = kbd_layouts_find("kr");
    if (us == NULL || de == NULL || jp == NULL || kr == NULL) {
        return;
    }
    const kbd_key_t *k = kbd_layouts_key(us, KBD_SLOT_AE01);
    CHECK(k != NULL && k->usage == 0x1Eu);
    CHECK(strcmp(kbd_layouts_legend(k, 0), "1") == 0 && strcmp(kbd_layouts_legend(k, 1), "!") == 0);
    CHECK(!(us->flags & KBD_LAYOUT_ALTGR));
    CHECK(de->flags & KBD_LAYOUT_ALTGR);
    k = kbd_layouts_key(de, KBD_SLOT_BKSL);
    CHECK(k != NULL && k->usage == 0x32u); // ISO: Non-US #
    k = kbd_layouts_key(us, KBD_SLOT_BKSL);
    CHECK(k != NULL && k->usage == 0x31u);
    k = kbd_layouts_key(de, KBD_SLOT_AE12); // dead acute / dead grave
    CHECK(k != NULL && (k->flags & KBD_KEY_DEAD_LEVEL(0)) && (k->flags & KBD_KEY_DEAD_LEVEL(1)));

    kvm_keystroke_t ks;
    CHECK(kbd_layouts_keystroke(de, '@', &ks) && ks.usage == 0x14u && ks.mods == 0x40u); // AltGr+Q
    CHECK(kbd_layouts_keystroke(de, 'Z', &ks) && ks.usage == 0x1Cu && ks.mods == 0x02u); // QWERTZ
    CHECK(kbd_layouts_keystroke(de, 0xF6u, &ks) && ks.usage == 0x33u && ks.mods == 0u);  // o-umlaut
    CHECK(!kbd_layouts_keystroke(de, 0xB4u, &ks)); // the acute accent itself needs the dead key
    CHECK(!kbd_layouts_keystroke(de, 0xE9u, &ks)); // e-acute: dead key + e, not one keystroke
    CHECK(!kbd_layouts_keystroke(us, 0xF6u, &ks));
    CHECK(kbd_layouts_keystroke(us, ' ', &ks) && ks.usage == 0x2Cu);
    CHECK(kbd_layouts_uni_key((void *)de, 'u', &ks) && ks.usage == 0x18u && ks.mods == 0u);

    k = kbd_layouts_key(jp, KBD_SLOT_MUHE);
    CHECK(k != NULL && k->usage == 0x8Bu && (k->flags & KBD_KEY_IME));
    k = kbd_layouts_key(jp, KBD_SLOT_AE13);
    CHECK(k != NULL && k->usage == 0x89u);
    CHECK(kbd_layouts_key(us, KBD_SLOT_MUHE) == NULL);
    k = kbd_layouts_key(kr, KBD_SLOT_HNGL);
    CHECK(k != NULL && k->usage == 0x90u && (k->flags & KBD_KEY_IME));
}

#ifdef KBD_TEST_FAKE
static void test_fake_exact(void)
{
    CHECK_EQ(kbd_layouts_count_get(), 5);
    CHECK(strcmp(kbd_layouts_at(0)->id, "br") == 0 && strcmp(kbd_layouts_at(4)->id, "us") == 0);
    CHECK(kbd_layouts_find("zz") == NULL); // rejected by the generator
    const kbd_layout_t *de = kbd_layouts_find("de");
    CHECK(strcmp(kbd_layouts_legend(kbd_layouts_key(de, KBD_SLOT_AE12), 0), "\xc2\xb4") == 0);
    kvm_keystroke_t ks;
    CHECK(kbd_layouts_keystroke(de, 0xB2u, &ks) && ks.usage == 0x1Fu && ks.mods == 0x40u); // AltGr+2
}
#endif

int main(void)
{
    printf("test_kbd_layouts (%s tables)\n",
#ifdef KBD_TEST_FAKE
           "fake"
#else
           "generated"
#endif
    );
    RUN(test_invariants);
    RUN(test_lookup_edges);
    RUN(test_representatives);
#ifdef KBD_TEST_FAKE
    RUN(test_fake_exact);
#endif
    TEST_MAIN_END("test_kbd_layouts");
}
