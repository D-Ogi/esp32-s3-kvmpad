// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_font_blob (ADR-024 FR-746, TECH-081) against the real LVGL 9.6 font engine:
// - a hand-built two-part blob with every supported cmap type (FORMAT0_TINY, SPARSE_TINY, FORMAT0_FULL with a hole)
//   resolves each code point to the right part and glyph, a hole and a foreign letter reach the fallback, and a
//   label draws from it;
// - each kind of damage is refused with its own status, every truncation is refused, and 3000 random corruptions
//   either load or are refused - a blob that loads still draws without reading outside the buffer (ASan);
// - every blob above also goes through the streamed loader (ADR-034): the same status, the same pixels for every
//   sample glyph, glyph reads inside the blob, nothing left allocated after ui_font_blob_close(); every read and
//   allocation of an open fails once in turn and is refused cleanly;
// - with KVM_HANZI_BLOB set (the blob tools/fonts/gen_hanzi_font_blob.py writes outside the repository), the
//   real candidate-bar font loads, covers the dictionary samples and draws.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_font_blob.h"
#include "ui_test_env.h"
#include "ui_theme.h"

// ---- a small blob, built the way the generator packs it ----------------------------------------
typedef struct {
    uint8_t b[1024];
    size_t n;
} blob_t;

static void put8(blob_t *x, uint8_t v)
{
    x->b[x->n++] = v;
}

static void put16(blob_t *x, uint16_t v)
{
    put8(x, (uint8_t)v);
    put8(x, (uint8_t)(v >> 8));
}

static void put32(blob_t *x, uint32_t v)
{
    put16(x, (uint16_t)v);
    put16(x, (uint16_t)(v >> 16));
}

static void set32(blob_t *x, size_t at, uint32_t v)
{
    x->b[at] = (uint8_t)v;
    x->b[at + 1] = (uint8_t)(v >> 8);
    x->b[at + 2] = (uint8_t)(v >> 16);
    x->b[at + 3] = (uint8_t)(v >> 24);
}

static void put32_at(uint8_t *b, size_t at, uint32_t v)
{
    b[at] = (uint8_t)v;
    b[at + 1] = (uint8_t)(v >> 8);
    b[at + 2] = (uint8_t)(v >> 16);
    b[at + 3] = (uint8_t)(v >> 24);
}

static void align(blob_t *x)
{
    while (x->n % 4u) {
        put8(x, 0);
    }
}

typedef struct {
    size_t cmap_rec[2][2]; // offset of each cmap record, per part
    size_t list[2][2];     // offset of each cmap list (0 = none)
    size_t dsc[2];         // offset of the glyph table of each part
    size_t part[2];        // offset of the part table row
    size_t data_end;       // end of the last bitmap; only alignment padding follows
} layout_t;

// Glyph k (1-based) of a part is (k + 1) px wide and 2 px high: k + 1 bytes of 4 bpp bitmap.
static void glyphs(blob_t *x, uint32_t count, size_t *dsc_off, size_t *bmp_off, uint32_t *bmp_len)
{
    *dsc_off = x->n;
    for (uint32_t i = 0; i < 8; i++) {
        put8(x, 0); // id 0 reserved
    }
    uint32_t index = 0;
    for (uint32_t k = 1; k < count; k++) {
        put32(x, index | (384u << 20));
        put8(x, (uint8_t)(k + 1u));
        put8(x, 2);
        put8(x, 0);
        put8(x, (uint8_t)(int8_t)-2);
        index += k + 1u;
    }
    *bmp_off = x->n;
    for (uint32_t i = 0; i < index; i++) {
        put8(x, (uint8_t)(0x11u * (i % 16u)));
    }
    *bmp_len = index;
    align(x);
}

static void cmap(blob_t *x, uint32_t start, uint16_t range, uint16_t gid, uint32_t list, uint16_t n, uint8_t type)
{
    put32(x, start);
    put16(x, range);
    put16(x, gid);
    put32(x, list);
    put16(x, n);
    put8(x, type);
    put8(x, 0);
}

/**
 * Part 0: 'A','B' (FORMAT0_TINY, glyphs 1-2) and U+4E00, U+4E0A (SPARSE_TINY, glyphs 3-4).
 * Part 1: U+5000 and U+5002 (FORMAT0_FULL over U+5000..U+5003 with holes at U+5001 and U+5003, glyphs 1-2).
 */
static void build(blob_t *x, layout_t *l)
{
    memset(x, 0, sizeof(*x));
    memset(l, 0, sizeof(*l));
    x->n = UI_FONT_BLOB_HEADER_LEN + 2u * UI_FONT_BLOB_PART_LEN;

    l->list[0][1] = x->n;
    put16(x, 0);
    put16(x, 10);
    align(x);
    const size_t cmap0 = x->n;
    l->cmap_rec[0][0] = x->n;
    cmap(x, 0x41, 2, 1, 0, 0, LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY);
    l->cmap_rec[0][1] = x->n;
    cmap(x, 0x4E00, 100, 3, (uint32_t)l->list[0][1], 2, LV_FONT_FMT_TXT_CMAP_SPARSE_TINY);
    size_t bmp0;
    uint32_t bmp0_len;
    glyphs(x, 5, &l->dsc[0], &bmp0, &bmp0_len);

    l->list[1][0] = x->n;
    put8(x, 0);
    put8(x, 0);
    put8(x, 1);
    put8(x, 0);
    align(x);
    const size_t cmap1 = x->n;
    l->cmap_rec[1][0] = x->n;
    cmap(x, 0x5000, 4, 1, (uint32_t)l->list[1][0], 4, LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL);
    size_t bmp1;
    uint32_t bmp1_len;
    glyphs(x, 3, &l->dsc[1], &bmp1, &bmp1_len);
    l->data_end = bmp1 + bmp1_len;

    memcpy(x->b, "KVMFONT1", 8);
    set32(x, 8, 1);
    set32(x, 12, 2);
    set32(x, 16, 4);
    set32(x, 20, (uint32_t)x->n);
    const size_t cm[2] = {cmap0, cmap1};
    const uint32_t cn[2] = {2, 1};
    const uint32_t gc[2] = {5, 3};
    const size_t bo[2] = {bmp0, bmp1};
    const uint32_t bl[2] = {bmp0_len, bmp1_len};
    for (int p = 0; p < 2; p++) {
        const size_t t = UI_FONT_BLOB_HEADER_LEN + UI_FONT_BLOB_PART_LEN * (size_t)p;
        l->part[p] = t;
        set32(x, t + 0, 24);
        set32(x, t + 4, 3);
        set32(x, t + 8, (uint32_t)-3);
        set32(x, t + 12, 1);
        set32(x, t + 16, (uint32_t)cm[p]);
        set32(x, t + 20, cn[p]);
        set32(x, t + 24, (uint32_t)l->dsc[p]);
        set32(x, t + 28, gc[p]);
        set32(x, t + 32, (uint32_t)bo[p]);
        set32(x, t + 36, bl[p]);
    }
}

// ---- the streamed source (ADR-034): a heap copy read through the callback, with failure injection ---------------
typedef struct {
    const uint8_t *b;
    size_t n;
    int reads;      // successful reads so far
    int fail_read;  // the read with this number (1-based) fails; 0 = never
    int allocs;     // allocations so far
    int fail_alloc; // the allocation with this number (1-based) fails; 0 = never
    int live;       // allocated and not yet freed
    bool out_of_range;
} mem_src_t;

static mem_src_t *s_src_ctx; // the allocator callbacks take no context

static bool src_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    mem_src_t *m = ctx;
    if ((uint64_t)off + n > m->n || n == 0u) {
        m->out_of_range = true; // the loader promises [off, off + n) inside the blob, never an empty read
        return false;
    }
    if (m->fail_read != 0 && m->reads + 1 == m->fail_read) {
        return false;
    }
    m->reads++;
    memcpy(dst, m->b + off, n);
    return true;
}

static void *src_alloc(size_t n)
{
    mem_src_t *m = s_src_ctx;
    m->allocs++;
    if (m->fail_alloc != 0 && m->allocs == m->fail_alloc) {
        return NULL;
    }
    m->live++;
    return malloc(n);
}

static void src_free(void *p)
{
    s_src_ctx->live--;
    free(p);
}

static ui_font_blob_status_t open_mem(ui_font_blob_t *f, mem_src_t *m, const uint8_t *b, size_t n)
{
    memset(m, 0, sizeof(*m));
    m->b = b;
    m->n = n;
    s_src_ctx = m;
    const ui_font_blob_src_t src = {
        .read = src_read, .alloc = src_alloc, .free = src_free, .ctx = m, .len = (uint32_t)n};
    return ui_font_blob_open(f, &src, &lv_font_montserrat_24, NULL, NULL);
}

/** Glyph `cp` resolves to the same box and decodes to the same pixels in both fonts. */
static bool same_glyph(const lv_font_t *a, const lv_font_t *b, uint32_t cp)
{
    lv_font_glyph_dsc_t ga;
    lv_font_glyph_dsc_t gb;
    memset(&ga, 0, sizeof(ga));
    memset(&gb, 0, sizeof(gb));
    const bool ha = lv_font_get_glyph_dsc(a, &ga, cp, 0);
    const bool hb = lv_font_get_glyph_dsc(b, &gb, cp, 0);
    if (ha != hb) {
        return false;
    }
    if (!ha) {
        return true;
    }
    const bool fa = ga.resolved_font == &lv_font_montserrat_24;
    const bool fb = gb.resolved_font == &lv_font_montserrat_24;
    if (fa != fb) {
        return false;
    }
    if (fa) {
        return true; // both from the fallback
    }
    if (ga.box_w != gb.box_w || ga.box_h != gb.box_h || ga.adv_w != gb.adv_w || ga.ofs_x != gb.ofs_x ||
        ga.ofs_y != gb.ofs_y) {
        return false;
    }
    if (ga.box_w == 0u || ga.box_h == 0u) {
        return true;
    }
    lv_draw_buf_t *da = lv_draw_buf_create(ga.box_w, ga.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    lv_draw_buf_t *db = lv_draw_buf_create(gb.box_w, gb.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    memset(da->data, 0x5A, da->data_size);
    memset(db->data, 0xA5, db->data_size);
    const bool got_a = lv_font_get_glyph_bitmap(&ga, da) != NULL;
    const bool got_b = lv_font_get_glyph_bitmap(&gb, db) != NULL;
    bool same = got_a == got_b;
    if (same && got_a) {
        for (uint32_t y = 0; y < ga.box_h && same; y++) {
            same = memcmp(da->data + da->header.stride * y, db->data + db->header.stride * y, ga.box_w) == 0;
        }
    }
    lv_draw_buf_destroy(da);
    lv_draw_buf_destroy(db);
    return same;
}

static void draw(const lv_font_t *font, const char *text);

static const uint32_t k_cps[] = {'A', 'B', 0x4E00, 0x4E0A, 0x5000, 0x5001, 0x5002, 0x5003, 'z'};

/**
 * Load a heap copy of exactly `n` bytes, so a read past the end is an ASan error. The same bytes also go through
 * the streamed loader, which must give the same status; when both load, every sample glyph decodes to the same
 * pixels (a stale byte in the loader's scratch buffer would differ) and the streamed font is drawn and closed with
 * nothing left allocated.
 */
static ui_font_blob_status_t load_copy(ui_font_blob_t *f, const uint8_t *src, size_t n, uint8_t **keep)
{
    uint8_t *copy = malloc(n > 0 ? n : 1);
    if (n > 0) {
        memcpy(copy, src, n);
    }
    const ui_font_blob_status_t st = ui_font_blob_load(f, copy, n, &lv_font_montserrat_24);

    static ui_font_blob_t s; // big: not on the stack
    mem_src_t m;
    const ui_font_blob_status_t st2 = open_mem(&s, &m, copy, n);
    if (st2 != st) {
        printf("    streamed %s, in place %s\n", ui_font_blob_status_name(st2), ui_font_blob_status_name(st));
    }
    CHECK_EQ(st2, st);
    CHECK(!m.out_of_range);
    if (st == UI_FONT_BLOB_OK && st2 == UI_FONT_BLOB_OK) {
        CHECK_EQ(s.parts, f->parts);
        CHECK_EQ(s.glyphs, f->glyphs);
        for (size_t i = 0; i < sizeof(k_cps) / sizeof(k_cps[0]); i++) {
            CHECK(same_glyph(ui_font_blob_font(f), ui_font_blob_font(&s), k_cps[i]));
        }
        draw(ui_font_blob_font(&s), "AB\xE4\xB8\x80\xE4\xB8\x8A\xE5\x80\x80\xE5\x80\x81\xE5\x80\x82");
        CHECK(!m.out_of_range);
    }
    ui_font_blob_close(&s);
    CHECK_EQ(m.live, 0);
    CHECK(ui_font_blob_font(&s) == NULL);

    if (keep != NULL && st == UI_FONT_BLOB_OK) {
        *keep = copy;
    } else {
        free(copy);
    }
    return st;
}

static ui_font_blob_t s_font;

static const lv_font_t *resolve(const lv_font_t *head, uint32_t cp, uint16_t *box_w)
{
    lv_font_glyph_dsc_t g;
    memset(&g, 0, sizeof(g));
    if (!lv_font_get_glyph_dsc(head, &g, cp, 0)) {
        return NULL;
    }
    *box_w = g.box_w;
    return g.resolved_font;
}

static void draw(const lv_font_t *font, const char *text)
{
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_refr_now(NULL);
    lv_obj_delete(label);
    lv_refr_now(NULL);
}

static void test_lookup_and_draw(void)
{
    blob_t x;
    layout_t l;
    build(&x, &l);
    uint8_t *mem = NULL;
    CHECK_EQ(load_copy(&s_font, x.b, x.n, &mem), UI_FONT_BLOB_OK);
    CHECK_EQ(s_font.parts, 2u);
    CHECK_EQ(s_font.glyphs, 6u);
    const lv_font_t *head = ui_font_blob_font(&s_font);
    CHECK(head == &s_font.font[0]);
    CHECK(s_font.font[0].fallback == &s_font.font[1]);
    CHECK(s_font.font[1].fallback == &lv_font_montserrat_24);
    CHECK_EQ(head->line_height, 24);

    uint16_t w = 0;
    CHECK(resolve(head, 'A', &w) == &s_font.font[0] && w == 2u);
    CHECK(resolve(head, 'B', &w) == &s_font.font[0] && w == 3u);
    CHECK(resolve(head, 0x4E00, &w) == &s_font.font[0] && w == 4u);
    CHECK(resolve(head, 0x4E0A, &w) == &s_font.font[0] && w == 5u);
    CHECK(resolve(head, 0x5000, &w) == &s_font.font[1] && w == 2u);
    CHECK(resolve(head, 0x5002, &w) == &s_font.font[1] && w == 3u);
    // a hole of FORMAT0_FULL and a code point of no cmap are not glyph 0 of the part: they fall through
    CHECK(resolve(head, 0x5001, &w) != &s_font.font[1]);
    CHECK(resolve(head, 0x4E01, &w) != &s_font.font[0]);
    CHECK(resolve(head, 'z', &w) == &lv_font_montserrat_24); // Latin from the fallback

    draw(head, "AB\xE4\xB8\x80\xE4\xB8\x8A\xE5\x80\x80\xE5\x80\x82 z"); // AB U+4E00 U+4E0A U+5000 U+5002 z
    ui_theme_set_hanzi_font(head);
    CHECK(ui_theme_font(UI_FONT_HANZI) == head);
    ui_theme_set_hanzi_font(NULL);
    CHECK(ui_theme_font(UI_FONT_HANZI) == &lv_font_montserrat_24);
    free(mem);
}

static void expect(ui_font_blob_status_t want, const blob_t *x, const char *what)
{
    ui_font_blob_t f;
    const ui_font_blob_status_t got = load_copy(&f, x->b, x->n, NULL);
    if (got != want) {
        printf("    %s: %s, expected %s\n", what, ui_font_blob_status_name(got), ui_font_blob_status_name(want));
    }
    CHECK_EQ(got, want);
    CHECK(ui_font_blob_font(&f) == NULL);
    CHECK_EQ(f.parts, 0u);
}

static void test_refusals(void)
{
    blob_t x;
    layout_t l;
    ui_font_blob_t f;
    CHECK_EQ(ui_font_blob_load(NULL, x.b, 64, NULL), UI_FONT_BLOB_ERR_ARG);
    CHECK_EQ(ui_font_blob_load(&f, NULL, 64, NULL), UI_FONT_BLOB_ERR_ARG);
    build(&x, &l);
    static uint32_t aligned[300];
    memcpy(aligned, x.b, x.n);
    CHECK_EQ(ui_font_blob_load(&f, (const uint8_t *)aligned + 1, x.n - 1, NULL), UI_FONT_BLOB_ERR_ARG); // misaligned
    CHECK_EQ(ui_font_blob_load(&f, aligned, x.n, NULL), UI_FONT_BLOB_OK);

#define DAMAGE(want, what, stmt) \
    do {                         \
        build(&x, &l);           \
        stmt;                    \
        expect(want, &x, what);  \
    } while (0)

    DAMAGE(UI_FONT_BLOB_ERR_MAGIC, "erased flash", memset(x.b, 0xFF, 8));
    DAMAGE(UI_FONT_BLOB_ERR_VERSION, "version 3", set32(&x, 8, 3));
    // version 2: the same bytes read as RLE streams; a glyph box larger than its stream can feed is refused
    DAMAGE(UI_FONT_BLOB_ERR_GLYPH, "compressed stream too short",
           (set32(&x, 8, 2), x.b[l.dsc[1] + 8u * 2u + 4u] = 200, x.b[l.dsc[1] + 8u * 2u + 5u] = 200));
    DAMAGE(UI_FONT_BLOB_ERR_VERSION, "8 bpp", set32(&x, 16, 8));
    DAMAGE(UI_FONT_BLOB_ERR_VERSION, "size field", set32(&x, 20, (uint32_t)x.n + 4u));
    DAMAGE(UI_FONT_BLOB_ERR_VERSION, "reserved header", x.b[28] = 1);
    DAMAGE(UI_FONT_BLOB_ERR_PART, "no part", set32(&x, 12, 0));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "too many parts", set32(&x, 12, UI_FONT_BLOB_PART_CAP + 1u));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "line height 0", set32(&x, l.part[1], 0));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "base line above", set32(&x, l.part[0] + 4, 25));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "cmap table outside", set32(&x, l.part[1] + 16, (uint32_t)x.n));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "misaligned glyph table", set32(&x, l.part[0] + 24, (uint32_t)l.dsc[0] + 2u));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "glyph table outside", set32(&x, l.part[1] + 28, 0xFFFFu));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "bitmap over 1 MiB", set32(&x, l.part[0] + 36, 0x100001u));
    DAMAGE(UI_FONT_BLOB_ERR_PART, "reserved part", x.b[l.part[0] + 44] = 1);
    DAMAGE(UI_FONT_BLOB_ERR_PART, "too many cmaps", set32(&x, l.part[0] + 20, UI_FONT_BLOB_CMAP_CAP + 1u));
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "SPARSE_FULL", x.b[l.cmap_rec[0][1] + 14] = LV_FONT_FMT_TXT_CMAP_SPARSE_FULL);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "unknown type", x.b[l.cmap_rec[0][0] + 14] = 9);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "tiny past the glyphs", x.b[l.cmap_rec[0][0] + 4] = 9);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "glyph id 0", x.b[l.cmap_rec[0][0] + 6] = 0);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "tiny with a list", set32(&x, l.cmap_rec[0][0] + 8, (uint32_t)l.list[0][1]));
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "sparse not sorted", x.b[l.list[0][1] + 2] = 0);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "sparse past the range", x.b[l.list[0][1] + 2] = 100);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "sparse list outside", set32(&x, l.cmap_rec[0][1] + 8, (uint32_t)x.n));
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "full offset past the glyphs", x.b[l.list[1][0] + 2] = 5);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "full list short", x.b[l.cmap_rec[1][0] + 12] = 3);
    DAMAGE(UI_FONT_BLOB_ERR_CMAP, "range past U+10FFFF", set32(&x, l.cmap_rec[0][0], 0x10FFFFu));
    DAMAGE(UI_FONT_BLOB_ERR_GLYPH, "bitmap outside", set32(&x, l.dsc[0] + 8u * 4u, 0xFFFu | (384u << 20)));
    DAMAGE(UI_FONT_BLOB_ERR_GLYPH, "box past the bitmap", x.b[l.dsc[1] + 8u * 2u + 4u] = 200);
    DAMAGE(UI_FONT_BLOB_ERR_GLYPH, "reserved glyph", x.b[l.dsc[0] + 4] = 1);
#undef DAMAGE
}

// ADR-034: the streamed font reads each glyph through the source when LVGL draws it, from a block of its own
static void test_streamed(void)
{
    blob_t x;
    layout_t l;
    build(&x, &l);
    uint8_t *copy = malloc(x.n);
    memcpy(copy, x.b, x.n);
    static ui_font_blob_t f;
    mem_src_t m;
    CHECK_EQ(open_mem(&f, &m, copy, x.n), UI_FONT_BLOB_OK);
    CHECK_EQ(f.parts, 2u);
    CHECK_EQ(f.glyphs, 6u);
    CHECK(f.arena != NULL && f.arena_len > 0u);
    CHECK_EQ(m.live, 1); // the window of the check is gone, the arena stays
    CHECK(f.dsc[0].are_glyphs_dynamic_loaded && f.dsc[1].are_glyphs_dynamic_loaded);
    CHECK(f.font[0].static_bitmap == 0u);
    // the tables are copies: nothing of the font points into the blob any more
    CHECK((const uint8_t *)f.dsc[0].glyph_dsc < copy || (const uint8_t *)f.dsc[0].glyph_dsc >= copy + x.n);
    CHECK((const uint8_t *)f.cmap[1].unicode_list < copy || (const uint8_t *)f.cmap[1].unicode_list >= copy + x.n);
    const lv_font_t *head = ui_font_blob_font(&f);
    uint16_t w = 0;
    CHECK(resolve(head, 0x4E0A, &w) == &f.font[0] && w == 5u);
    CHECK(resolve(head, 0x5002, &w) == &f.font[1] && w == 3u);
    CHECK(resolve(head, 'z', &w) == &lv_font_montserrat_24);
    const int reads_before = m.reads;
    CHECK_EQ(f.glyph_reads, 0u);
    draw(head, "AB\xE4\xB8\x80\xE4\xB8\x8A\xE5\x80\x80\xE5\x80\x82 z");
    CHECK(f.glyph_reads >= 6u); // one read per drawn glyph at least
    CHECK(m.reads > reads_before);
    CHECK(!m.out_of_range);
    ui_font_blob_close(&f);
    CHECK_EQ(m.live, 0);
    CHECK(ui_font_blob_font(&f) == NULL && f.arena == NULL);
    ui_font_blob_close(&f); // twice: harmless
    ui_font_blob_close(NULL);

    // arguments
    const ui_font_blob_src_t good = {
        .read = src_read, .alloc = src_alloc, .free = src_free, .ctx = &m, .len = (uint32_t)x.n};
    ui_font_blob_src_t bad = good;
    CHECK_EQ(ui_font_blob_open(NULL, &good, NULL, NULL, NULL), UI_FONT_BLOB_ERR_ARG);
    CHECK_EQ(ui_font_blob_open(&f, NULL, NULL, NULL, NULL), UI_FONT_BLOB_ERR_ARG);
    bad.read = NULL;
    CHECK_EQ(ui_font_blob_open(&f, &bad, NULL, NULL, NULL), UI_FONT_BLOB_ERR_ARG);
    bad = good;
    bad.alloc = NULL;
    CHECK_EQ(ui_font_blob_open(&f, &bad, NULL, NULL, NULL), UI_FONT_BLOB_ERR_ARG);
    bad = good;
    bad.len = UI_FONT_BLOB_HEADER_LEN - 1u;
    CHECK_EQ(ui_font_blob_open(&f, &bad, NULL, NULL, NULL), UI_FONT_BLOB_ERR_ARG);

    // every read of the open fails once in turn: refused with read-failed, nothing left allocated
    CHECK_EQ(open_mem(&f, &m, copy, x.n), UI_FONT_BLOB_OK);
    const int open_reads = m.reads;
    ui_font_blob_close(&f);
    CHECK(open_reads > 4);
    for (int k = 1; k <= open_reads; k++) {
        memset(&m, 0, sizeof(m));
        m.b = copy;
        m.n = x.n;
        m.fail_read = k;
        s_src_ctx = &m;
        const ui_font_blob_src_t src = {
            .read = src_read, .alloc = src_alloc, .free = src_free, .ctx = &m, .len = (uint32_t)x.n};
        CHECK_EQ(ui_font_blob_open(&f, &src, NULL, NULL, NULL), UI_FONT_BLOB_ERR_IO);
        CHECK_EQ(m.live, 0);
        CHECK(ui_font_blob_font(&f) == NULL);
    }
    // the arena or the window cannot be allocated
    for (int k = 1; k <= 2; k++) {
        memset(&m, 0, sizeof(m));
        m.b = copy;
        m.n = x.n;
        m.fail_alloc = k;
        s_src_ctx = &m;
        const ui_font_blob_src_t src = {
            .read = src_read, .alloc = src_alloc, .free = src_free, .ctx = &m, .len = (uint32_t)x.n};
        CHECK_EQ(ui_font_blob_open(&f, &src, NULL, NULL, NULL), UI_FONT_BLOB_ERR_NOMEM);
        CHECK_EQ(m.live, 0);
    }
    // a glyph read that fails while drawing: LVGL skips the glyph, nothing crashes
    CHECK_EQ(open_mem(&f, &m, copy, x.n), UI_FONT_BLOB_OK);
    m.fail_read = m.reads + 1;
    draw(ui_font_blob_font(&f), "AB");
    m.fail_read = 0;
    draw(ui_font_blob_font(&f), "AB");
    ui_font_blob_close(&f);
    CHECK_EQ(m.live, 0);
    free(copy);
}

// A plain glyph whose bitmap fits its part but is longer than UI_FONT_BLOB_GLYPH_MAX is refused in both modes.
static void test_glyph_cap(void)
{
    // one part, 'A' only: a 40 x 52 box = 1040 bytes of 4 bpp, in a part bitmap long enough for it (built in a heap
    // buffer: blob_t holds 1 KiB)
    const uint32_t bytes = 40u * 52u / 2u;
    const size_t dsc = UI_FONT_BLOB_HEADER_LEN + UI_FONT_BLOB_PART_LEN;
    const size_t cm = dsc + 16u;
    const size_t bmp = cm + UI_FONT_BLOB_CMAP_LEN;
    const size_t n = (bmp + bytes + 3u) & ~(size_t)3u;
    uint8_t *b = calloc(1, n);
    memcpy(b, "KVMFONT1", 8);
    put32_at(b, 8, 1);
    put32_at(b, 12, 1);
    put32_at(b, 16, 4);
    put32_at(b, 20, (uint32_t)n);
    const size_t t = UI_FONT_BLOB_HEADER_LEN;
    put32_at(b, t + 0, 60);
    put32_at(b, t + 4, 8);
    put32_at(b, t + 16, (uint32_t)cm);
    put32_at(b, t + 20, 1);
    put32_at(b, t + 24, (uint32_t)dsc);
    put32_at(b, t + 28, 2);
    put32_at(b, t + 32, (uint32_t)bmp);
    put32_at(b, t + 36, bytes);
    put32_at(b, dsc + 8, 0u | (640u << 20));
    b[dsc + 12] = 40;
    b[dsc + 13] = 52;
    put32_at(b, cm, 'A');
    b[cm + 4] = 1; // range 1
    b[cm + 6] = 1; // glyph id 1
    b[cm + 14] = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY;
    ui_font_blob_t f;
    CHECK_EQ(load_copy(&f, b, n, NULL), UI_FONT_BLOB_ERR_GLYPH);
    // the same font with a 40 x 51 box (1020 bytes) is accepted
    b[dsc + 13] = 51;
    uint8_t *mem = NULL;
    CHECK_EQ(load_copy(&f, b, n, &mem), UI_FONT_BLOB_OK);
    free(mem);
    free(b);
}

static void test_truncation_and_corruption(void)
{
    blob_t x;
    layout_t l;
    build(&x, &l);
    // every truncation, with the size field kept honest so validation goes past the header: refused unless only the
    // alignment padding after the last bitmap was cut
    CHECK(l.data_end < x.n);
    for (size_t n = 0; n < x.n; n++) {
        blob_t t = x;
        t.n = n;
        if (n >= 24) {
            set32(&t, 20, (uint32_t)n);
        }
        ui_font_blob_t f;
        CHECK((load_copy(&f, t.b, t.n, NULL) == UI_FONT_BLOB_OK) == (n >= l.data_end));
    }
    // random damage past the magic: refused, or loaded and drawn without a read outside the buffer
    uint32_t seed = 12345u;
    unsigned loaded = 0;
    for (int i = 0; i < 3000; i++) {
        blob_t t = x;
        const int flips = 1 + i % 3;
        for (int k = 0; k < flips; k++) {
            seed = seed * 1103515245u + 12345u;
            const size_t at = 8u + (seed >> 8) % (x.n - 8u);
            seed = seed * 1103515245u + 12345u;
            t.b[at] ^= (uint8_t)(1u + (seed >> 16) % 255u);
        }
        uint8_t *mem = NULL;
        ui_font_blob_t f;
        if (load_copy(&f, t.b, t.n, &mem) == UI_FONT_BLOB_OK) {
            loaded++;
            draw(ui_font_blob_font(&f), "AB\xE4\xB8\x80\xE4\xB8\x8A\xE5\x80\x80\xE5\x80\x81\xE5\x80\x82");
            free(mem);
        }
    }
    printf("    3000 corruptions: %u still valid (drawn), the rest refused\n", loaded);

    // the same as version 2 (compressed): whatever passes the bounded RLE walk is drawn by LVGL's own decoder from an
    // exact-size copy, so a read past any glyph's part would trip AddressSanitizer
    unsigned loaded2 = 0;
    for (int i = 0; i < 3000; i++) {
        blob_t t = x;
        set32(&t, 8, 2);
        const int flips = i % 4; // 0: the hand-built bytes as they are
        for (int k = 0; k < flips; k++) {
            seed = seed * 1103515245u + 12345u;
            const size_t at = 12u + (seed >> 8) % (x.n - 12u);
            seed = seed * 1103515245u + 12345u;
            t.b[at] ^= (uint8_t)(1u + (seed >> 16) % 255u);
        }
        uint8_t *mem = NULL;
        ui_font_blob_t f;
        if (load_copy(&f, t.b, t.n, &mem) == UI_FONT_BLOB_OK) {
            loaded2++;
            draw(ui_font_blob_font(&f), "AB\xE4\xB8\x80\xE4\xB8\x8A\xE5\x80\x80\xE5\x80\x81\xE5\x80\x82");
            free(mem);
        }
    }
    printf("    3000 compressed variants: %u loaded (drawn by LVGL's decoder), the rest refused\n", loaded2);
    CHECK(loaded2 > 0u);
}

#ifdef KVM_HANZI_BLOB
static void count_yield(void *ctx)
{
    (*(unsigned *)ctx)++;
}

static void test_real_blob(void)
{
    FILE *fp = fopen(KVM_HANZI_BLOB, "rb");
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    CHECK(fread(buf, 1, (size_t)len, fp) == (size_t)len);
    fclose(fp);
    ui_font_blob_t *f = malloc(sizeof(*f));
    CHECK_EQ(ui_font_blob_load(f, buf, (size_t)len, &lv_font_montserrat_24), UI_FONT_BLOB_OK);
    printf("    %s: %ld bytes, %u parts, %u glyphs\n", KVM_HANZI_BLOB, len, (unsigned)f->parts, (unsigned)f->glyphs);
    const lv_font_t *head = ui_font_blob_font(f);
    CHECK(head != NULL);
    // 你 好 〇 鼥 (the rarest of the dictionary) 。 ！
    static const uint32_t cps[] = {0x4F60, 0x597D, 0x3007, 0x9F25, 0x3002, 0xFF01};
    for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); i++) {
        uint16_t w = 0;
        const lv_font_t *r = resolve(head, cps[i], &w);
        CHECK(r != NULL && r != &lv_font_montserrat_24 && w > 0u);
    }
    uint16_t w = 0;
    CHECK(resolve(head, 'a', &w) == &lv_font_montserrat_24);
    CHECK_EQ(lv_font_get_glyph_width(head, 0x4F60, 0x597D), 24); // 你: a 24 px em box
    draw(head, "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xE3\x80\x82 nihao"); // 你好，世界。

    // the yield of the device's loader task: called once per UI_FONT_BLOB_YIELD_GLYPHS glyphs, same result
    ui_font_blob_t *g = malloc(sizeof(*g));
    unsigned calls = 0;
    CHECK_EQ(ui_font_blob_load_ex(g, buf, (size_t)len, &lv_font_montserrat_24, count_yield, &calls), UI_FONT_BLOB_OK);
    printf("    load_ex: %u yields for %u glyphs\n", calls, (unsigned)g->glyphs);
    CHECK_EQ(calls, (unsigned)(g->glyphs / UI_FONT_BLOB_YIELD_GLYPHS));
    CHECK(calls > 0u);
    CHECK_EQ(g->glyphs, f->glyphs);
    CHECK_EQ(g->parts, f->parts);
    calls = 0;
    CHECK_EQ(ui_font_blob_load_ex(g, buf, (size_t)len, &lv_font_montserrat_24, NULL, &calls), UI_FONT_BLOB_OK);
    CHECK_EQ(calls, 0u); // no hook: nothing is called
    free(g);

    // ADR-034: the same blob streamed - same glyphs, same pixels, and the tables it keeps in memory
    ui_font_blob_t *s = malloc(sizeof(*s));
    mem_src_t m;
    CHECK_EQ(open_mem(s, &m, buf, (size_t)len), UI_FONT_BLOB_OK);
    printf("    streamed: %u parts, %u glyphs, %u bytes kept in memory, %d reads to open\n", (unsigned)s->parts,
           (unsigned)s->glyphs, (unsigned)s->arena_len, m.reads);
    CHECK_EQ(s->glyphs, f->glyphs);
    CHECK_EQ(m.live, 1);
    unsigned differ = 0;
    for (uint32_t cp = 0x3000; cp < 0xA000; cp += 7u) {
        differ += same_glyph(head, ui_font_blob_font(s), cp) ? 0u : 1u;
    }
    for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); i++) {
        differ += same_glyph(head, ui_font_blob_font(s), cps[i]) ? 0u : 1u;
    }
    CHECK_EQ(differ, 0u);
    draw(ui_font_blob_font(s), "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xE3\x80\x82 nihao");
    CHECK(!m.out_of_range);
    ui_font_blob_close(s);
    CHECK_EQ(m.live, 0);
    free(s);
    free(f);
    free(buf);
}
#endif

int main(void)
{
    printf("test_ui_font_blob (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    const size_t base = ui_test_env_start();
    RUN(test_lookup_and_draw);
    RUN(test_refusals);
    RUN(test_streamed);
    RUN(test_glyph_cap);
    RUN(test_truncation_and_corruption);
#ifdef KVM_HANZI_BLOB
    RUN(test_real_blob);
#else
    printf("  test_real_blob: skipped (configure with -DKVM_HANZI_BLOB=<path>)\n");
#endif
    lv_obj_clean(lv_screen_active());
    lv_refr_now(NULL);
    ui_test_env_stop(base);
    TEST_MAIN_END("test_ui_font_blob");
}
