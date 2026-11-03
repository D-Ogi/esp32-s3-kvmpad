// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_wnn: see ime_wnn.h. Input is validated here, so the vendored engine only ever sees lowercase
// letters, '-' and the separator, of a bounded length; a candidate id is checked against the last
// count. One allocation at open (the conversion work area, which prefers PSRAM through
// port/ime_alloc.h), none per keystroke: the candidate texts and the fixed prefix live in this
// file's own fixed buffers, which linker.lf places in PSRAM on the device.
#include "ime_wnn.h"

#include <string.h>

#include "ime_alloc.h"
#include "wnn_convert.h"
#include "wnn_romkan.h"

// Letters covered by one fixed clause and its text, so cancel_last can undo exactly one step.
typedef struct {
    uint8_t kana;
    uint8_t text;
} fixed_step_t;

static bool s_open;
static void *s_mem;
static wnn_conv_t *s_conv;

static wnn_romkan_t s_rk;
static size_t s_kana_len;  // leading romkan segments that hold kana: the conversion input
static size_t s_base;      // kana fixed so far

static uint16_t s_fixed[IME_WNN_MAX_TEXT];
static size_t s_fixed_len;
static fixed_step_t s_step[WNN_MAX_KANA];
static size_t s_step_n;

static uint16_t s_cand[IME_WNN_MAX_CANDIDATES][IME_WNN_MAX_TEXT];
static uint8_t s_cand_len[IME_WNN_MAX_CANDIDATES];
static size_t s_cands;
static size_t s_first_kana;  // kana of the clause candidates 1.. are alternatives for
static bool s_truncated;

static bool valid_input(const char *s, size_t len)
{
    if (s == NULL || len == 0 || len > IME_WNN_MAX_INPUT) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') || s[i] == '-' || s[i] == '\'')) {
            return false;
        }
    }
    return true;
}

static void clear_state(void)
{
    wnn_romkan_reset(&s_rk);
    s_kana_len = 0;
    s_base = 0;
    s_fixed_len = 0;
    s_step_n = 0;
    s_cands = 0;
    s_first_kana = 0;
    s_truncated = false;
}

static bool add_cand(const uint16_t *text, size_t n, bool dedup)
{
    if (n == 0 || n > IME_WNN_MAX_TEXT || s_cands >= IME_WNN_MAX_CANDIDATES) {
        return false;
    }
    if (dedup) {
        // Candidate 0 is the whole sentence and is never compared: alternatives are deduplicated
        // among themselves, because the engine reports one entry per part-of-speech pair.
        for (size_t i = 1; i < s_cands; i++) {
            if (s_cand_len[i] == n && memcmp(s_cand[i], text, n * sizeof(uint16_t)) == 0) {
                return false;
            }
        }
    }
    memcpy(s_cand[s_cands], text, n * sizeof(uint16_t));
    s_cand_len[s_cands] = (uint8_t)n;
    s_cands++;
    return true;
}

// Convert kana[s_base..s_kana_len) and rebuild the candidate list.
static size_t reconvert(void)
{
    s_cands = 0;
    s_first_kana = 0;
    s_truncated = false;

    if (s_kana_len == 0) {
        return 0;
    }
    if (s_base >= s_kana_len) {
        add_cand(s_fixed, s_fixed_len, false);  // everything is fixed: only the sentence is left
        return s_cands;
    }

    uint16_t text[IME_WNN_MAX_TEXT];
    size_t n = s_fixed_len <= IME_WNN_MAX_TEXT ? s_fixed_len : IME_WNN_MAX_TEXT;
    memcpy(text, s_fixed, n * sizeof(uint16_t));

    wnn_word_t clause[WNN_MAX_KANA];
    const size_t nc = wnn_conv_sentence(s_conv, s_base, clause, WNN_MAX_KANA);
    if (nc == 0) {
        // Nothing converts (a reading the dictionaries cannot segment): keep the plain reading.
        s_first_kana = s_kana_len - s_base;
        for (size_t i = s_base; i < s_kana_len && n < IME_WNN_MAX_TEXT; i++) {
            text[n++] = s_rk.seg[i].unit;
        }
    } else {
        s_first_kana = clause[0].stroke_len;
        for (size_t i = 0; i < nc; i++) {
            uint16_t t[2 * WNN_MAX_RESULT];
            const size_t tn = wnn_conv_text(s_conv, &clause[i], t);
            for (size_t k = 0; k < tn && n < IME_WNN_MAX_TEXT; k++) {
                text[n++] = t[k];
            }
        }
    }
    add_cand(text, n, false);

    // Candidates 1..: the single-clause conversions of the first clause, then its plain hiragana,
    // then its katakana.
    wnn_word_t alt[IME_WNN_MAX_CANDIDATES];
    const size_t na = wnn_conv_clause_all(s_conv, s_base, s_first_kana, alt, IME_WNN_MAX_CANDIDATES);
    for (size_t i = 0; i < na; i++) {
        uint16_t t[2 * WNN_MAX_RESULT];
        const size_t tn = wnn_conv_text(s_conv, &alt[i], t);
        add_cand(t, tn, true);
    }
    uint16_t kana[WNN_MAX_KANA];
    for (size_t i = 0; i < s_first_kana; i++) {
        kana[i] = s_rk.seg[s_base + i].unit;
    }
    add_cand(kana, s_first_kana, true);
    const size_t kn = wnn_conv_katakana(s_conv, s_base, s_first_kana, kana);
    add_cand(kana, kn, true);

    s_truncated = wnn_conv_truncated(s_conv);
    return s_cands;
}

bool ime_wnn_dicts_from_blob(const void *blob, size_t len, ime_wnn_dicts_t *out)
{
    if (blob == NULL || out == NULL || len < IME_WNN_BLOB_HEADER) {
        return false;
    }
    const uint8_t *b = (const uint8_t *)blob;
    if (memcmp(b, IME_WNN_BLOB_MAGIC, 8) != 0) {
        return false;
    }
    uint32_t v[2];
    memcpy(v, b + 8, sizeof(v));
    if (v[0] != IME_WNN_BLOB_VERSION || v[1] != IME_WNN_BLOB_COUNT) {
        return false;
    }
    const void *p[IME_WNN_BLOB_COUNT];
    size_t sz[IME_WNN_BLOB_COUNT];
    for (unsigned i = 0; i < IME_WNN_BLOB_COUNT; i++) {
        uint32_t e[2];
        memcpy(e, b + 16 + 8 * i, sizeof(e));
        if (e[0] < IME_WNN_BLOB_HEADER || e[1] == 0 || e[0] > len || len - e[0] < e[1]) {
            return false;
        }
        p[i] = b + e[0];
        sz[i] = e[1];
    }
    memset(out, 0, sizeof(*out));
    out->tankanji = p[0];
    out->tankanji_len = sz[0];
    out->jiritsu = p[1];
    out->jiritsu_len = sz[1];
    out->fzk = p[2];
    out->fzk_len = sz[2];
    out->connect = p[3];
    out->connect_len = sz[3];
    return true;
}

size_t ime_wnn_work_size(void)
{
    return wnn_conv_sizeof();
}

bool ime_wnn_open(const ime_wnn_dicts_t *d)
{
    ime_wnn_close();
    if (d == NULL) {
        return false;
    }
    s_mem = ime_engine_malloc(wnn_conv_sizeof());
    if (s_mem == NULL) {
        return false;
    }
    wnn_dicts_t w;
    memset(&w, 0, sizeof(w));
    w.tankanji = d->tankanji;
    w.tankanji_len = d->tankanji_len;
    w.jiritsu = d->jiritsu;
    w.jiritsu_len = d->jiritsu_len;
    w.fzk = d->fzk;
    w.fzk_len = d->fzk_len;
    w.connect = d->connect;
    w.connect_len = d->connect_len;
    if (!wnn_conv_init(s_mem, &w, &s_conv)) {
        ime_engine_free(s_mem);
        s_mem = NULL;
        s_conv = NULL;
        return false;
    }
    clear_state();
    s_open = true;
    return true;
}

void ime_wnn_close(void)
{
    clear_state();
    if (s_mem != NULL) {
        ime_engine_free(s_mem);
    }
    s_mem = NULL;
    s_conv = NULL;
    s_open = false;
}

bool ime_wnn_is_open(void)
{
    return s_open;
}

size_t ime_wnn_search(const char *letters, size_t len)
{
    if (!s_open || !valid_input(letters, len)) {
        clear_state();
        return 0;
    }
    clear_state();
    for (size_t i = 0; i < len; i++) {
        if (letters[i] == '\'') {
            wnn_romkan_separator(&s_rk);
        } else if (!wnn_romkan_push(&s_rk, letters[i], i)) {
            break;
        }
    }
    s_kana_len = wnn_romkan_kana_segs(&s_rk);
    if (s_kana_len > WNN_MAX_KANA) {
        s_kana_len = WNN_MAX_KANA;
    }
    uint16_t kana[WNN_MAX_KANA];
    for (size_t i = 0; i < s_kana_len; i++) {
        kana[i] = s_rk.seg[i].unit;
    }
    if (!wnn_conv_set_input(s_conv, kana, s_kana_len)) {
        s_kana_len = 0;
        return 0;
    }
    return reconvert();
}

size_t ime_wnn_candidate(size_t i, uint16_t *out, size_t cap)
{
    if (out != NULL && cap > 0) {
        out[0] = 0;
    }
    if (!s_open || out == NULL || cap < 2 || i >= s_cands) {
        return 0;
    }
    size_t n = s_cand_len[i];
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(out, s_cand[i], n * sizeof(uint16_t));
    out[n] = 0;
    return n;
}

size_t ime_wnn_choose(size_t i)
{
    if (!s_open || i >= s_cands) {
        return s_cands;
    }
    const size_t kana = (i == 0) ? s_kana_len - s_base : s_first_kana;
    // Candidate i already carries the fixed prefix for i == 0; for i > 0 it is the clause alone.
    const size_t add = (i == 0) ? s_cand_len[0] - s_fixed_len : s_cand_len[i];
    if (kana == 0 || s_step_n >= WNN_MAX_KANA || s_fixed_len + add > IME_WNN_MAX_TEXT) {
        return s_cands;  // nothing left to fix, or the fixed prefix would not fit
    }
    if (i == 0) {
        memcpy(&s_fixed[s_fixed_len], &s_cand[0][s_fixed_len], add * sizeof(uint16_t));
    } else {
        memcpy(&s_fixed[s_fixed_len], s_cand[i], add * sizeof(uint16_t));
    }
    s_fixed_len += add;
    s_base += kana;
    s_step[s_step_n].kana = (uint8_t)kana;
    s_step[s_step_n].text = (uint8_t)add;
    s_step_n++;
    return reconvert();
}

size_t ime_wnn_cancel_last(void)
{
    if (!s_open || s_step_n == 0) {
        return s_cands;
    }
    s_step_n--;
    s_base -= s_step[s_step_n].kana;
    s_fixed_len -= s_step[s_step_n].text;
    return reconvert();
}

size_t ime_wnn_fixed_len(void)
{
    return s_open ? s_fixed_len : 0;
}

size_t ime_wnn_consumed(void)
{
    if (!s_open || s_base == 0) {
        return 0;
    }
    return (size_t)s_rk.seg[s_base - 1].to + 1u;
}

size_t ime_wnn_parsed(void)
{
    return s_open ? wnn_romkan_parsed(&s_rk) : 0;
}

bool ime_wnn_is_complete(void)
{
    return s_open && s_kana_len > 0 && s_base >= s_kana_len;
}

size_t ime_wnn_preedit(uint16_t *out, size_t cap)
{
    if (out != NULL && cap > 0) {
        out[0] = 0;
    }
    if (!s_open || out == NULL || cap < 2) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = s_base; i < s_rk.n && n < cap - 1; i++) {
        out[n++] = s_rk.seg[i].unit;
    }
    out[n] = 0;
    return n;
}

void ime_wnn_reset(void)
{
    clear_state();
}

bool ime_wnn_truncated(void)
{
    return s_truncated;
}
