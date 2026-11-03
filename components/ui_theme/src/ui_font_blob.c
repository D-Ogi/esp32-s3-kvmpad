// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_font_blob: see ui_font_blob.h for the format and the contract.
#include "ui_font_blob.h"

#include <string.h>

// LVGL's on-demand glyph loader (lv_font_fmt_txt_glyph_loader_t) is declared in this private header of the pinned
// LVGL 9.6; its binfont loader uses the same mechanism (src/font/binfont_loader/lv_binfont_loader.c).
#include "src/font/fmt_txt/lv_font_fmt_txt_private.h"

static const char k_magic[8] = {'K', 'V', 'M', 'F', 'O', 'N', 'T', '1'};

_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) == 8u, "ui_font_blob: needs LV_FONT_FMT_TXT_LARGE == 0");

// Streamed mode reads the bitmap region through a window this long while it checks the glyphs (glyphs are stored in
// order, so the window moves forward: about 16 reads per 256 KiB).
#define WINDOW_LEN 16384u

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static size_t align4(size_t n)
{
    return (n + 3u) & ~(size_t)3u;
}

/** [off, off + n) lies inside [0, len) without overflow. */
static bool inside(size_t len, uint32_t off, uint64_t n)
{
    return (uint64_t)off <= len && n <= (uint64_t)len - off;
}

/** The blob's glyph descriptor bytes are the compiler's lv_font_fmt_txt_glyph_dsc_t (bit field order). */
static bool abi_ok(void)
{
    lv_font_fmt_txt_glyph_dsc_t g;
    memset(&g, 0, sizeof(g));
    g.bitmap_index = 0x12345u;
    g.adv_w = 0xABCu;
    g.box_w = 0x11u;
    g.box_h = 0x22u;
    g.ofs_x = -3;
    g.ofs_y = 5;
    const uint32_t w = 0x12345u | (0xABCu << 20);
    const uint8_t want[8] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24), 0x11, 0x22,
                             (uint8_t)(int8_t)-3, 5};
    return memcmp(&g, want, sizeof(want)) == 0;
}

// ---- the blob behind both modes -----------------------------------------------------------------------------------
// In place every access is a pointer into the blob. Streamed, small records are read into the caller's buffer, tables
// are read once into the arena (where they stay for LVGL), and glyph streams are checked through a moving window.

typedef struct {
    const uint8_t *mem;            // in place: the blob
    const ui_font_blob_src_t *src; // streamed: the source
    size_t len;
    uint8_t *arena; // streamed: tables carved from here
    size_t cap;
    size_t used;
    uint8_t *win; // streamed: the bitmap window
    uint32_t win_off;
    uint32_t win_len;
    ui_font_blob_status_t err; // the reason a helper returned false / NULL
} ld_t;

static bool ld_read(ld_t *l, uint32_t off, void *dst, uint32_t n)
{
    if (l->mem != NULL) {
        memcpy(dst, l->mem + off, n);
        return true;
    }
    if (n == 0u || l->src->read(l->src->ctx, off, dst, n)) {
        return true;
    }
    l->err = UI_FONT_BLOB_ERR_IO;
    return false;
}

/** A table of the blob that LVGL keeps using: a pointer into the blob, or a copy in the arena. */
static const uint8_t *ld_table(ld_t *l, uint32_t off, uint32_t n)
{
    if (l->mem != NULL) {
        return l->mem + off;
    }
    const size_t need = align4(n);
    if (need > l->cap - l->used) {
        l->err = UI_FONT_BLOB_ERR_NOMEM; // the sizing pass and this pass disagree: never expected
        return NULL;
    }
    uint8_t *dst = l->arena + l->used;
    if (!ld_read(l, off, dst, n)) {
        return NULL;
    }
    l->used += need;
    return dst;
}

/** `need` bytes of glyph bitmap at blob offset `off` ([off, off + need) inside the blob), for the check only. */
static const uint8_t *ld_bitmap(ld_t *l, uint32_t off, uint32_t need)
{
    if (l->mem != NULL) {
        return l->mem + off;
    }
    if (l->win_len == 0u || off < l->win_off || (uint64_t)off + need > (uint64_t)l->win_off + l->win_len) {
        const uint32_t n = (uint32_t)(l->len - off) < WINDOW_LEN ? (uint32_t)(l->len - off) : WINDOW_LEN;
        l->win_len = 0;
        if (!ld_read(l, off, l->win, n)) {
            return NULL;
        }
        l->win_off = off;
        l->win_len = n;
    }
    return l->win + (off - l->win_off);
}

static ui_font_blob_status_t check_cmap(ld_t *l, const uint8_t *c, uint32_t glyphs, lv_font_fmt_txt_cmap_t *out)
{
    const uint32_t start = rd32(c);
    const uint16_t range = rd16(c + 4);
    const uint16_t gid = rd16(c + 6);
    const uint32_t list = rd32(c + 8);
    const uint16_t n = rd16(c + 12);
    const uint8_t type = c[14];
    if (c[15] != 0u || range == 0u || gid == 0u || (uint64_t)start + range > 0x110000u) {
        return UI_FONT_BLOB_ERR_CMAP;
    }
    memset(out, 0, sizeof(*out));
    out->range_start = start;
    out->range_length = range;
    out->glyph_id_start = gid;
    switch (type) {
    case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
        if (list != 0u || n != 0u || (uint32_t)gid + range > glyphs) {
            return UI_FONT_BLOB_ERR_CMAP;
        }
        break;
    case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
        // one uint8 offset per code point of the range; 0 past the first one means "no glyph"
        if (list == 0u || n != range || !inside(l->len, list, n)) {
            return UI_FONT_BLOB_ERR_CMAP;
        }
        const uint8_t *ofs = ld_table(l, list, n);
        if (ofs == NULL) {
            return l->err;
        }
        for (uint32_t i = 0; i < n; i++) {
            if ((uint32_t)gid + ofs[i] >= glyphs) {
                return UI_FONT_BLOB_ERR_CMAP;
            }
        }
        out->glyph_id_ofs_list = ofs;
        out->list_length = n;
        break;
    }
    case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: {
        // strictly increasing code point offsets inside the range; glyph ids gid .. gid + n - 1
        if (list == 0u || (list & 1u) != 0u || n == 0u || !inside(l->len, list, 2u * (uint64_t)n) ||
            (uint32_t)gid + n > glyphs) {
            return UI_FONT_BLOB_ERR_CMAP;
        }
        const uint8_t *raw = ld_table(l, list, 2u * (uint32_t)n);
        if (raw == NULL) {
            return l->err;
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint16_t v = rd16(raw + 2u * i);
            if (v >= range || (i > 0u && v <= rd16(raw + 2u * (i - 1u)))) {
                return UI_FONT_BLOB_ERR_CMAP;
            }
        }
        out->unicode_list = (const uint16_t *)(const void *)raw;
        out->list_length = n;
        break;
    }
    default:
        return UI_FONT_BLOB_ERR_CMAP;
    }
    out->type = (lv_font_fmt_txt_cmap_type_t)type;
    return UI_FONT_BLOB_OK;
}

// ---- compressed glyphs (version 2) --------------------------------------------------------------------------------
// LVGL decodes a compressed glyph with a bit reader that has no end: it reads exactly as many bits as the RLE stream
// of box_w x box_h pixels needs, and every read whose bits end on or past a byte boundary also loads the following
// byte. This walks the same state machine (lv_font_fmt_txt.c, rle_next(), 4 bpp) over the same pixel count and
// fails as soon as any byte LVGL would load lies outside [0, avail), so a stream that is short or damaged is refused
// before LVGL can read past its part. It also reports how many bytes LVGL touches, which is what the streamed loader
// reads for the glyph.

typedef struct {
    const uint8_t *in;
    uint32_t avail; // bytes from the glyph's start to the end of its part's bitmap (at most UI_FONT_BLOB_GLYPH_MAX)
    uint32_t rdp;   // bit position
    uint32_t hi;    // bytes touched so far
    bool ok;
} rle_bound_t;

static uint8_t rle_bits(rle_bound_t *r, uint8_t len)
{
    const uint32_t byte = r->rdp >> 3;
    const uint32_t bit = r->rdp & 7u;
    const uint32_t last = byte + ((bit + len >= 8u) ? 1u : 0u); // LVGL's get_bits() loads in[byte + 1] then
    if (last >= r->avail) {
        r->ok = false;
        r->rdp += len;
        return 0;
    }
    if (last + 1u > r->hi) {
        r->hi = last + 1u;
    }
    uint8_t v;
    if (bit + len >= 8u) {
        const uint16_t in16 = (uint16_t)((r->in[byte] << 8) | r->in[byte + 1u]);
        v = (uint8_t)((in16 >> (16u - bit - len)) & ((1u << len) - 1u));
    } else {
        v = (uint8_t)((r->in[byte] >> (8u - bit - len)) & ((1u << len) - 1u));
    }
    r->rdp += len;
    return v;
}

static bool rle_fits(const uint8_t *in, uint32_t avail, uint32_t pixels, uint32_t *touched)
{
    enum { SINGLE, REPEATED, COUNTER } state = SINGLE;
    rle_bound_t r = {.in = in, .avail = avail, .rdp = 0, .hi = 0, .ok = true};
    uint8_t prev = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < pixels && r.ok; i++) {
        if (state == SINGLE) {
            const bool first = r.rdp == 0u;
            const uint8_t v = rle_bits(&r, 4);
            if (!first && prev == v) {
                count = 0;
                state = REPEATED;
            }
            prev = v;
        } else if (state == REPEATED) {
            const uint8_t bit = rle_bits(&r, 1);
            count++;
            if (bit == 1u) {
                if (count == 11u) {
                    count = rle_bits(&r, 6);
                    if (count != 0u) {
                        state = COUNTER;
                    } else {
                        prev = rle_bits(&r, 4);
                        state = SINGLE;
                    }
                }
            } else {
                prev = rle_bits(&r, 4);
                state = SINGLE;
            }
        } else { // COUNTER
            count--;
            if (count == 0u) {
                prev = rle_bits(&r, 4);
                state = SINGLE;
            }
        }
    }
    *touched = r.hi;
    return r.ok;
}

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
    uint32_t since; // glyphs checked since the last call
} yield_t;

static void maybe_yield(yield_t *y)
{
    if (y->fn != NULL && ++y->since >= UI_FONT_BLOB_YIELD_GLYPHS) {
        y->since = 0;
        y->fn(y->ctx);
    }
}

/** Check every glyph of a part; `*window` = the longest stream any glyph of the part makes LVGL read. */
static ui_font_blob_status_t check_glyphs(ld_t *l, const uint8_t *g, uint32_t glyphs, uint32_t bmp_off,
                                          uint32_t bitmap_len, bool compressed, yield_t *y, uint32_t *window)
{
    *window = 0;
    for (uint32_t i = 1; i < glyphs; i++) {
        maybe_yield(y);
        const uint8_t *d = g + 8u * i;
        const uint32_t index = rd32(d) & 0xFFFFFu;
        uint32_t touched = 0;
        if (!compressed) {
            const uint64_t bytes = ((uint64_t)d[4] * d[5] * 4u + 7u) / 8u; // plain 4 bpp, bit-packed rows
            if ((uint64_t)index + bytes > bitmap_len || bytes > UI_FONT_BLOB_GLYPH_MAX) {
                return UI_FONT_BLOB_ERR_GLYPH;
            }
            touched = (uint32_t)bytes;
        } else {
            if (index >= bitmap_len) {
                return UI_FONT_BLOB_ERR_GLYPH;
            }
            // LVGL decodes the first line even when box_h is 0; nothing at all when box_w is 0.
            const uint32_t pixels = d[4] == 0u ? 0u : (uint32_t)d[4] * (d[5] == 0u ? 1u : d[5]);
            if (pixels != 0u) {
                const uint32_t left = bitmap_len - index;
                const uint32_t avail = left < UI_FONT_BLOB_GLYPH_MAX ? left : UI_FONT_BLOB_GLYPH_MAX;
                const uint8_t *in = ld_bitmap(l, bmp_off + index, avail);
                if (in == NULL) {
                    return l->err;
                }
                if (!rle_fits(in, avail, pixels, &touched)) {
                    return UI_FONT_BLOB_ERR_GLYPH;
                }
            }
        }
        if (touched > *window) {
            *window = touched;
        }
    }
    return UI_FONT_BLOB_OK;
}

// ---- streamed glyph loader ----------------------------------------------------------------------------------------

typedef struct {
    lv_font_fmt_txt_glyph_loader_t base; // first: LVGL casts lv_font_fmt_txt_dsc_t.glyph_bitmap to this
    ui_font_blob_t *f;
    const lv_font_fmt_txt_glyph_dsc_t *glyph_dsc;
    uint32_t glyphs;
    uint32_t bmp_off;
    uint32_t bmp_len;
    uint32_t window; // bytes read per glyph, at most UI_FONT_BLOB_GLYPH_MAX
    uint8_t scratch[UI_FONT_BLOB_GLYPH_MAX];
} part_loader_t;

static const size_t k_loader_len = (sizeof(part_loader_t) + 7u) & ~(size_t)7u;

/** LVGL owner, with base->lock held: read glyph `gid` of the part into the scratch buffer. */
static const uint8_t *load_glyph_cb(lv_font_fmt_txt_glyph_loader_t *base, uint32_t gid)
{
    part_loader_t *p = (part_loader_t *)(void *)base;
    if (gid == 0u || gid >= p->glyphs) {
        return NULL;
    }
    const uint32_t index = p->glyph_dsc[gid].bitmap_index; // checked at open: index <= bmp_len
    const uint32_t left = p->bmp_len - index;
    const uint32_t n = left < p->window ? left : p->window;
    if (n != 0u && !p->f->src.read(p->f->src.ctx, p->bmp_off + index, p->scratch, n)) {
        return NULL;
    }
    p->f->glyph_reads++;
    return p->scratch;
}

// ---- the loader of both modes -------------------------------------------------------------------------------------

static bool part_fields_ok(const uint8_t *t, size_t len, uint32_t cmaps_used)
{
    const int32_t line_h = (int32_t)rd32(t);
    const int32_t base = (int32_t)rd32(t + 4);
    const int32_t ul_pos = (int32_t)rd32(t + 8);
    const int32_t ul_thick = (int32_t)rd32(t + 12);
    const uint32_t cmap_off = rd32(t + 16);
    const uint32_t cmap_n = rd32(t + 20);
    const uint32_t dsc_off = rd32(t + 24);
    const uint32_t glyphs = rd32(t + 28);
    const uint32_t bmp_off = rd32(t + 32);
    const uint32_t bmp_len = rd32(t + 36);
    return !(line_h <= 0 || line_h > 255 || base < 0 || base > line_h || ul_pos < -line_h || ul_pos > line_h ||
             ul_thick < 0 || ul_thick > line_h || rd32(t + 40) != 0u || rd32(t + 44) != 0u || cmap_n == 0u ||
             cmap_n > UI_FONT_BLOB_CMAP_CAP - cmaps_used || glyphs < 2u || glyphs > 0xFFFFu || bmp_len == 0u ||
             bmp_len > 0x100000u || ((cmap_off | dsc_off) & 3u) != 0u ||
             !inside(len, cmap_off, (uint64_t)cmap_n * UI_FONT_BLOB_CMAP_LEN) || !inside(len, dsc_off, 8ull * glyphs) ||
             !inside(len, bmp_off, bmp_len));
}

/**
 * Streamed: the arena the tables need (loaders first, then glyph tables and cmap lists). Sizing stops at the first
 * part whose fields are bad: the checking pass refuses the blob there at the latest, with the status the in-place
 * mode reports for the same bytes.
 */
static ui_font_blob_status_t arena_size(ld_t *l, const uint8_t *pt, uint32_t parts, size_t *out)
{
    size_t n = k_loader_len * parts;
    uint32_t cmaps_used = 0;
    for (uint32_t p = 0; p < parts; p++) {
        const uint8_t *t = pt + UI_FONT_BLOB_PART_LEN * p;
        if (!part_fields_ok(t, l->len, cmaps_used)) {
            break;
        }
        const uint32_t cmap_off = rd32(t + 16);
        const uint32_t cmap_n = rd32(t + 20);
        n += align4(8u * (size_t)rd32(t + 28));
        for (uint32_t c = 0; c < cmap_n; c++) {
            uint8_t rec[UI_FONT_BLOB_CMAP_LEN];
            if (!ld_read(l, cmap_off + UI_FONT_BLOB_CMAP_LEN * c, rec, sizeof(rec))) {
                return l->err;
            }
            const uint16_t list_n = rd16(rec + 12);
            if (rec[14] == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL) {
                n += align4(list_n);
            } else if (rec[14] == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) {
                n += align4(2u * (size_t)list_n);
            }
        }
        cmaps_used += cmap_n;
    }
    *out = n;
    return UI_FONT_BLOB_OK;
}

static ui_font_blob_status_t load_core(ui_font_blob_t *f, ld_t *l, const lv_font_t *fallback, yield_t *y)
{
    if (!abi_ok()) {
        return UI_FONT_BLOB_ERR_ABI;
    }
    uint8_t hdr[UI_FONT_BLOB_HEADER_LEN];
    if (!ld_read(l, 0, hdr, sizeof(hdr))) {
        return l->err;
    }
    if (memcmp(hdr, k_magic, sizeof(k_magic)) != 0) {
        return UI_FONT_BLOB_ERR_MAGIC;
    }
    const uint32_t version = rd32(hdr + 8);
    // version 2 = RLE-compressed glyphs with the line prefilter; only usable when LVGL can decode them
    const bool compressed = version == 2u;
    if ((version != 1u && !(compressed && LV_USE_FONT_COMPRESSED)) || rd32(hdr + 16) != 4u || rd32(hdr + 20) != l->len ||
        rd32(hdr + 24) != 0u || rd32(hdr + 28) != 0u) {
        return UI_FONT_BLOB_ERR_VERSION;
    }
    const uint32_t parts = rd32(hdr + 12);
    if (parts == 0u || parts > UI_FONT_BLOB_PART_CAP ||
        !inside(l->len, UI_FONT_BLOB_HEADER_LEN, (uint64_t)parts * UI_FONT_BLOB_PART_LEN)) {
        return UI_FONT_BLOB_ERR_PART;
    }
    uint8_t pt[UI_FONT_BLOB_PART_CAP * UI_FONT_BLOB_PART_LEN];
    if (!ld_read(l, UI_FONT_BLOB_HEADER_LEN, pt, UI_FONT_BLOB_PART_LEN * parts)) {
        return l->err;
    }

    if (l->mem == NULL) {
        size_t need = 0;
        const ui_font_blob_status_t st = arena_size(l, pt, parts, &need);
        if (st != UI_FONT_BLOB_OK) {
            return st;
        }
        l->arena = l->src->alloc(need);
        l->win = l->src->alloc(WINDOW_LEN);
        if (l->arena == NULL || l->win == NULL) {
            return UI_FONT_BLOB_ERR_NOMEM;
        }
        memset(l->arena, 0, need);
        l->cap = need;
        l->used = k_loader_len * parts;
    }

    uint32_t window[UI_FONT_BLOB_PART_CAP] = {0};
    uint32_t cmaps_used = 0;
    uint32_t glyphs_total = 0;
    for (uint32_t p = 0; p < parts; p++) {
        const uint8_t *t = pt + UI_FONT_BLOB_PART_LEN * p;
        if (!part_fields_ok(t, l->len, cmaps_used)) {
            return UI_FONT_BLOB_ERR_PART;
        }
        const uint32_t cmap_off = rd32(t + 16);
        const uint32_t cmap_n = rd32(t + 20);
        const uint32_t dsc_off = rd32(t + 24);
        const uint32_t glyphs = rd32(t + 28);
        const uint32_t bmp_off = rd32(t + 32);
        const uint32_t bmp_len = rd32(t + 36);
        const uint8_t *g = ld_table(l, dsc_off, 8u * glyphs);
        if (g == NULL) {
            return l->err;
        }
        // glyph id 0 is reserved and all zero, as lv_font_conv writes it
        for (uint32_t i = 0; i < 8u; i++) {
            if (g[i] != 0u) {
                return UI_FONT_BLOB_ERR_GLYPH;
            }
        }
        ui_font_blob_status_t st = check_glyphs(l, g, glyphs, bmp_off, bmp_len, compressed, y, &window[p]);
        for (uint32_t c = 0; c < cmap_n && st == UI_FONT_BLOB_OK; c++) {
            uint8_t rec[UI_FONT_BLOB_CMAP_LEN];
            st = ld_read(l, cmap_off + UI_FONT_BLOB_CMAP_LEN * c, rec, sizeof(rec))
                     ? check_cmap(l, rec, glyphs, &f->cmap[cmaps_used + c])
                     : l->err;
        }
        if (st != UI_FONT_BLOB_OK) {
            return st;
        }

        lv_font_fmt_txt_dsc_t *d = &f->dsc[p];
        d->glyph_dsc = (const lv_font_fmt_txt_glyph_dsc_t *)(const void *)g;
        d->cmaps = &f->cmap[cmaps_used];
        d->kern_dsc = NULL;
        d->kern_scale = 0;
        d->cmap_num = (uint16_t)cmap_n;
        d->bpp = 4;
        d->kern_classes = 0;
        d->bitmap_format = compressed ? LV_FONT_FMT_TXT_COMPRESSED : LV_FONT_FMT_TXT_PLAIN;
        d->stride = 0;
        if (l->mem != NULL) {
            d->glyph_bitmap = l->mem + bmp_off;
            d->are_glyphs_dynamic_loaded = false;
        } else {
            part_loader_t *lo = (part_loader_t *)(void *)(l->arena + k_loader_len * p);
            lo->f = f;
            lo->glyph_dsc = d->glyph_dsc;
            lo->glyphs = glyphs;
            lo->bmp_off = bmp_off;
            lo->bmp_len = bmp_len;
            lo->window = window[p];
            lo->base.get_glyph_bitmap_cb = load_glyph_cb;
            d->glyph_bitmap = (const uint8_t *)(const void *)lo;
            d->are_glyphs_dynamic_loaded = true;
        }

        lv_font_t *font = &f->font[p];
        font->get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
        font->get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
        font->line_height = (int32_t)rd32(t);
        font->base_line = (int32_t)rd32(t + 4);
        font->subpx = LV_FONT_SUBPX_NONE;
        font->underline_position = (int8_t)(int32_t)rd32(t + 8);
        font->underline_thickness = (int8_t)(int32_t)rd32(t + 12);
        // never static: a streamed glyph's bitmap is overwritten by the next one, and a compressed one is decoded
        // into a draw buffer, so the draw units must not keep the pointer
        font->static_bitmap = 0;
        font->dsc = d;
        cmaps_used += cmap_n;
        glyphs_total += glyphs - 1u;
    }
    for (uint32_t p = 0; p < parts; p++) {
        f->font[p].fallback = p + 1u < parts ? &f->font[p + 1u] : fallback;
    }
    f->parts = parts;
    f->glyphs = glyphs_total;
    return UI_FONT_BLOB_OK;
}

ui_font_blob_status_t ui_font_blob_load(ui_font_blob_t *f, const void *blob, size_t len, const lv_font_t *fallback)
{
    return ui_font_blob_load_ex(f, blob, len, fallback, NULL, NULL);
}

ui_font_blob_status_t ui_font_blob_load_ex(ui_font_blob_t *f, const void *blob, size_t len, const lv_font_t *fallback,
                                           void (*yield)(void *ctx), void *ctx)
{
    yield_t y = {.fn = yield, .ctx = ctx, .since = 0};
    if (f != NULL) {
        memset(f, 0, sizeof(*f));
    }
    const uint8_t *b = blob;
    if (f == NULL || b == NULL || ((uintptr_t)b & 3u) != 0u || len < UI_FONT_BLOB_HEADER_LEN) {
        return UI_FONT_BLOB_ERR_ARG;
    }
    ld_t l = {.mem = b, .len = len, .err = UI_FONT_BLOB_OK};
    const ui_font_blob_status_t st = load_core(f, &l, fallback, &y);
    if (st != UI_FONT_BLOB_OK) {
        memset(f, 0, sizeof(*f));
    }
    return st;
}

ui_font_blob_status_t ui_font_blob_open(ui_font_blob_t *f, const ui_font_blob_src_t *src, const lv_font_t *fallback,
                                        void (*yield)(void *ctx), void *ctx)
{
    yield_t y = {.fn = yield, .ctx = ctx, .since = 0};
    if (f != NULL) {
        memset(f, 0, sizeof(*f));
    }
    if (f == NULL || src == NULL || src->read == NULL || src->alloc == NULL || src->free == NULL ||
        src->len < UI_FONT_BLOB_HEADER_LEN) {
        return UI_FONT_BLOB_ERR_ARG;
    }
    f->src = *src;
    ld_t l = {.src = &f->src, .len = src->len, .err = UI_FONT_BLOB_OK};
    ui_font_blob_status_t st = load_core(f, &l, fallback, &y);
    if (l.win != NULL) {
        src->free(l.win);
    }
    if (st == UI_FONT_BLOB_OK) {
        for (uint32_t p = 0; p < f->parts; p++) {
            part_loader_t *lo = (part_loader_t *)(void *)(l.arena + k_loader_len * p);
            if (lv_mutex_init(&lo->base.lock) != LV_RESULT_OK) {
                for (uint32_t q = 0; q < p; q++) {
                    lv_mutex_delete(&((part_loader_t *)(void *)(l.arena + k_loader_len * q))->base.lock);
                }
                st = UI_FONT_BLOB_ERR_NOMEM;
                break;
            }
        }
    }
    if (st != UI_FONT_BLOB_OK) {
        if (l.arena != NULL) {
            src->free(l.arena);
        }
        memset(f, 0, sizeof(*f));
        return st;
    }
    f->arena = l.arena;
    f->arena_len = l.cap;
    return UI_FONT_BLOB_OK;
}

void ui_font_blob_close(ui_font_blob_t *f)
{
    if (f == NULL) {
        return;
    }
    if (f->arena != NULL) {
        for (uint32_t p = 0; p < f->parts; p++) {
            lv_mutex_delete(&((part_loader_t *)(void *)((uint8_t *)f->arena + k_loader_len * p))->base.lock);
        }
        f->src.free(f->arena);
    }
    memset(f, 0, sizeof(*f));
}

const lv_font_t *ui_font_blob_font(const ui_font_blob_t *f)
{
    return (f != NULL && f->parts > 0u) ? &f->font[0] : NULL;
}

const char *ui_font_blob_status_name(ui_font_blob_status_t s)
{
    switch (s) {
    case UI_FONT_BLOB_OK: return "ok";
    case UI_FONT_BLOB_ERR_ARG: return "bad-argument";
    case UI_FONT_BLOB_ERR_MAGIC: return "bad-magic";
    case UI_FONT_BLOB_ERR_VERSION: return "bad-version";
    case UI_FONT_BLOB_ERR_PART: return "bad-part";
    case UI_FONT_BLOB_ERR_CMAP: return "bad-cmap";
    case UI_FONT_BLOB_ERR_GLYPH: return "bad-glyph";
    case UI_FONT_BLOB_ERR_ABI: return "abi-mismatch";
    case UI_FONT_BLOB_ERR_IO: return "read-failed";
    case UI_FONT_BLOB_ERR_NOMEM: return "no-memory";
    default: return "unknown";
    }
}
