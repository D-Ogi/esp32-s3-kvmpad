// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Bounded JSON reader (spec 03 §6.2, TECH-043). Iterative: the only state is an explicit container
// stack of KVM_JSON_MAX_DEPTH entries, so a hostile frame cannot grow the call stack.
#include "remote_json.h"

#include <string.h>

// ---- UTF-8 ---------------------------------------------------------------------------------

static bool utf8_ok(const char *s, uint16_t len)
{
    uint16_t i = 0;
    while (i < len) {
        uint8_t c = (uint8_t)s[i];
        uint16_t need;
        uint32_t cp;
        if (c < 0x80u) {
            i++;
            continue;
        }
        if ((c & 0xE0u) == 0xC0u) {
            need = 1;
            cp = (uint32_t)(c & 0x1Fu);
        } else if ((c & 0xF0u) == 0xE0u) {
            need = 2;
            cp = (uint32_t)(c & 0x0Fu);
        } else if ((c & 0xF8u) == 0xF0u) {
            need = 3;
            cp = (uint32_t)(c & 0x07u);
        } else {
            return false; // continuation byte or 5/6-byte lead
        }
        if ((uint32_t)i + (uint32_t)need >= (uint32_t)len) {
            return false; // truncated sequence
        }
        for (uint16_t k = 1; k <= need; k++) {
            uint8_t cc = (uint8_t)s[i + k];
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        if ((need == 1 && cp < 0x80u) || (need == 2 && cp < 0x800u) || (need == 3 && cp < 0x10000u)) {
            return false; // overlong
        }
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false; // out of range or encoded surrogate
        }
        i = (uint16_t)(i + need + 1u);
    }
    return true;
}

// ---- unescaping ----------------------------------------------------------------------------

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex4(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        uint32_t d;
        if (c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c <= 'F') {
            d = (uint32_t)(c - 'A') + 10u;
        } else {
            d = (uint32_t)(c - 'a') + 10u;
        }
        v = (v << 4) | d;
    }
    return v;
}

static bool emit_byte(char *out, size_t cap, size_t *o, uint8_t b)
{
    if (*o + 1u >= cap) {
        return false; // keep room for the terminator
    }
    out[*o] = (char)b;
    (*o)++;
    return true;
}

static bool emit_cp(uint32_t cp, char *out, size_t cap, size_t *o)
{
    if (cp == 0u) {
        return false; // an embedded NUL would truncate every C-string consumer
    }
    if (cp < 0x80u) {
        return emit_byte(out, cap, o, (uint8_t)cp);
    }
    if (cp < 0x800u) {
        return emit_byte(out, cap, o, (uint8_t)(0xC0u | (cp >> 6))) &&
               emit_byte(out, cap, o, (uint8_t)(0x80u | (cp & 0x3Fu)));
    }
    if (cp < 0x10000u) {
        return emit_byte(out, cap, o, (uint8_t)(0xE0u | (cp >> 12))) &&
               emit_byte(out, cap, o, (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu))) &&
               emit_byte(out, cap, o, (uint8_t)(0x80u | (cp & 0x3Fu)));
    }
    return emit_byte(out, cap, o, (uint8_t)(0xF0u | (cp >> 18))) &&
           emit_byte(out, cap, o, (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu))) &&
           emit_byte(out, cap, o, (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu))) &&
           emit_byte(out, cap, o, (uint8_t)(0x80u | (cp & 0x3Fu)));
}

/** Unescape a raw string span. Returns the length or (size_t)-1 on overflow / invalid escape. */
static size_t unescape(const char *s, uint16_t len, char *out, size_t cap)
{
    size_t o = 0;
    uint16_t i = 0;
    if (cap == 0u) {
        return (size_t)-1;
    }
    while (i < len) {
        char c = s[i];
        if (c != '\\') {
            if (!emit_byte(out, cap, &o, (uint8_t)c)) {
                return (size_t)-1;
            }
            i++;
            continue;
        }
        i++; // the scanner already guaranteed a valid escape follows
        char e = s[i];
        i++;
        switch (e) {
        case '"':
        case '\\':
        case '/':
            if (!emit_byte(out, cap, &o, (uint8_t)e)) {
                return (size_t)-1;
            }
            break;
        case 'b':
            if (!emit_byte(out, cap, &o, 0x08u)) {
                return (size_t)-1;
            }
            break;
        case 'f':
            if (!emit_byte(out, cap, &o, 0x0Cu)) {
                return (size_t)-1;
            }
            break;
        case 'n':
            if (!emit_byte(out, cap, &o, 0x0Au)) {
                return (size_t)-1;
            }
            break;
        case 'r':
            if (!emit_byte(out, cap, &o, 0x0Du)) {
                return (size_t)-1;
            }
            break;
        case 't':
            if (!emit_byte(out, cap, &o, 0x09u)) {
                return (size_t)-1;
            }
            break;
        case 'u': {
            uint32_t cp = hex4(s + i);
            i = (uint16_t)(i + 4u);
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                // high surrogate: a low surrogate escape must follow
                if ((uint32_t)i + 6u > (uint32_t)len || s[i] != '\\' || s[i + 1] != 'u') {
                    return (size_t)-1;
                }
                uint32_t lo = hex4(s + i + 2);
                if (lo < 0xDC00u || lo > 0xDFFFu) {
                    return (size_t)-1;
                }
                i = (uint16_t)(i + 6u);
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                return (size_t)-1; // lone low surrogate
            }
            if (!emit_cp(cp, out, cap, &o)) {
                return (size_t)-1;
            }
            break;
        }
        default:
            return (size_t)-1;
        }
    }
    out[o] = '\0';
    return o;
}

static bool span_has_escape(const char *s, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if (s[i] == '\\') {
            return true;
        }
    }
    return false;
}

/** Compare two raw string spans by their unescaped value. */
static bool span_equal(const char *a, uint16_t alen, const char *b, uint16_t blen)
{
    char ba[KVM_JSON_ESCAPE_SCRATCH];
    char bb[KVM_JSON_ESCAPE_SCRATCH];
    const char *pa = a;
    const char *pb = b;
    size_t la = alen;
    size_t lb = blen;
    if (span_has_escape(a, alen)) {
        la = unescape(a, alen, ba, sizeof(ba));
        if (la == (size_t)-1) {
            return false; // does not fit / invalid: never equal to a protocol name
        }
        pa = ba;
    }
    if (span_has_escape(b, blen)) {
        lb = unescape(b, blen, bb, sizeof(bb));
        if (lb == (size_t)-1) {
            return false;
        }
        pb = bb;
    }
    return la == lb && memcmp(pa, pb, la) == 0;
}

// ---- scanners ------------------------------------------------------------------------------

static bool scan_string(const char *s, uint16_t len, uint16_t pos, uint16_t *body_start, uint16_t *body_len,
                        uint16_t *next)
{
    uint16_t i = (uint16_t)(pos + 1u);
    *body_start = i;
    while (i < len) {
        uint8_t c = (uint8_t)s[i];
        if (c == '"') {
            *body_len = (uint16_t)(i - *body_start);
            *next = (uint16_t)(i + 1u);
            return true;
        }
        if (c < 0x20u) {
            return false; // unescaped control character
        }
        if (c == '\\') {
            if ((uint32_t)i + 1u >= (uint32_t)len) {
                return false;
            }
            char e = s[i + 1];
            if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
                i = (uint16_t)(i + 2u);
                continue;
            }
            if (e == 'u') {
                if ((uint32_t)i + 5u >= (uint32_t)len) {
                    return false;
                }
                for (int k = 2; k < 6; k++) {
                    if (!is_hex(s[i + k])) {
                        return false;
                    }
                }
                i = (uint16_t)(i + 6u);
                continue;
            }
            return false;
        }
        i++;
    }
    return false; // unterminated
}

static bool scan_number(const char *s, uint16_t len, uint16_t pos, uint16_t *out_len, bool *is_int)
{
    uint16_t i = pos;
    *is_int = true;
    if (i < len && s[i] == '-') {
        i++;
    }
    if (i >= len) {
        return false;
    }
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    } else {
        return false;
    }
    if (i < len && s[i] == '.') {
        *is_int = false;
        i++;
        if (i >= len || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        *is_int = false;
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        if (i >= len || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }
    *out_len = (uint16_t)(i - pos);
    return true;
}

static bool lit(const char *s, uint16_t len, uint16_t pos, const char *word, uint16_t *out_len)
{
    uint16_t wl = (uint16_t)strlen(word);
    if ((uint32_t)pos + (uint32_t)wl > (uint32_t)len) {
        return false;
    }
    if (memcmp(s + pos, word, wl) != 0) {
        return false;
    }
    *out_len = wl;
    return true;
}

// ---- parser --------------------------------------------------------------------------------

static int tok_new(kvm_json_doc_t *d, kvm_json_type_t t, int parent, uint16_t start)
{
    if (d->ntok >= KVM_JSON_MAX_TOKENS) {
        return -1;
    }
    kvm_json_tok_t *k = &d->tok[d->ntok];
    k->type = (uint8_t)t;
    k->flags = 0;
    k->parent = (int16_t)parent;
    k->start = start;
    k->len = 0;
    k->count = 0;
    return (int)d->ntok++;
}

static bool dup_key(const kvm_json_doc_t *d, int obj, uint16_t start, uint16_t len)
{
    for (uint16_t t = 0; t < d->ntok; t++) {
        const kvm_json_tok_t *k = &d->tok[t];
        if (k->parent == (int16_t)obj && (k->flags & KVM_JSON_FLAG_KEY) != 0u &&
            span_equal(d->text + k->start, k->len, d->text + start, len)) {
            return true;
        }
    }
    return false;
}

typedef enum { ST_VALUE = 0, ST_KEY, ST_COLON, ST_AFTER } jstate_t;

kvm_json_err_t kvm_json_parse(kvm_json_doc_t *doc, const char *text, size_t len)
{
    doc->text = text;
    doc->len = 0;
    doc->ntok = 0;
    if (text == NULL || len == 0u) {
        return KVM_JSON_ERR_EMPTY;
    }
    if (len > KVM_JSON_MAX_FRAME) {
        return KVM_JSON_ERR_SIZE;
    }
    uint16_t n = (uint16_t)len;
    if (!utf8_ok(text, n)) {
        return KVM_JSON_ERR_UTF8;
    }
    doc->len = n;

    int16_t stack[KVM_JSON_MAX_DEPTH];
    uint8_t sp = 0;
    jstate_t st = ST_VALUE;
    uint16_t pos = 0;
    kvm_json_err_t err = KVM_JSON_OK;
    int pending_key = -1;

    for (;;) {
        while (pos < n) {
            char w = text[pos];
            if (w == ' ' || w == '\t' || w == '\n' || w == '\r') {
                pos++;
            } else {
                break;
            }
        }
        if (pos >= n) {
            if (sp == 0u && st == ST_AFTER) {
                break; // complete document
            }
            err = KVM_JSON_ERR_SYNTAX;
            goto fail;
        }
        if (sp == 0u && st == ST_AFTER) {
            err = KVM_JSON_ERR_TRAILING; // bytes after the root value
            goto fail;
        }

        char c = text[pos];
        int parent = (sp == 0u) ? -1 : (int)stack[sp - 1u];

        switch (st) {
        case ST_VALUE: {
            if (c == '}' && sp > 0u && doc->tok[stack[sp - 1u]].type == (uint8_t)KVM_JSON_OBJECT) {
                err = KVM_JSON_ERR_SYNTAX; // "{"a":}"
                goto fail;
            }
            if (c == ']') {
                if (sp == 0u || doc->tok[stack[sp - 1u]].type != (uint8_t)KVM_JSON_ARRAY ||
                    doc->tok[stack[sp - 1u]].count != 0u) {
                    err = KVM_JSON_ERR_SYNTAX;
                    goto fail;
                }
                pos++;
                sp--;
                st = ST_AFTER;
                break;
            }
            if (c == '{' || c == '[') {
                if (sp >= KVM_JSON_MAX_DEPTH) {
                    err = KVM_JSON_ERR_DEPTH;
                    goto fail;
                }
                int t = tok_new(doc, (c == '{') ? KVM_JSON_OBJECT : KVM_JSON_ARRAY, parent, pos);
                if (t < 0) {
                    err = KVM_JSON_ERR_TOKENS;
                    goto fail;
                }
                if (parent >= 0 && pending_key < 0) {
                    doc->tok[parent].count++; // array element; an object member was counted with its key
                }
                pending_key = -1;
                stack[sp] = (int16_t)t;
                sp++;
                pos++;
                st = (c == '{') ? ST_KEY : ST_VALUE;
                break;
            }
            uint16_t bs = 0;
            uint16_t bl = 0;
            uint16_t nx = 0;
            int t;
            if (c == '"') {
                if (!scan_string(text, n, pos, &bs, &bl, &nx)) {
                    err = KVM_JSON_ERR_SYNTAX;
                    goto fail;
                }
                t = tok_new(doc, KVM_JSON_STRING, parent, bs);
                if (t < 0) {
                    err = KVM_JSON_ERR_TOKENS;
                    goto fail;
                }
                doc->tok[t].len = bl;
                pos = nx;
            } else if (c == '-' || (c >= '0' && c <= '9')) {
                uint16_t nl = 0;
                bool is_int = false;
                if (!scan_number(text, n, pos, &nl, &is_int)) {
                    err = KVM_JSON_ERR_SYNTAX;
                    goto fail;
                }
                t = tok_new(doc, KVM_JSON_NUMBER, parent, pos);
                if (t < 0) {
                    err = KVM_JSON_ERR_TOKENS;
                    goto fail;
                }
                doc->tok[t].len = nl;
                if (is_int) {
                    doc->tok[t].flags |= KVM_JSON_FLAG_INT;
                }
                pos = (uint16_t)(pos + nl);
            } else {
                uint16_t wl = 0;
                kvm_json_type_t ty;
                if (lit(text, n, pos, "true", &wl) || lit(text, n, pos, "false", &wl)) {
                    ty = KVM_JSON_BOOL;
                } else if (lit(text, n, pos, "null", &wl)) {
                    ty = KVM_JSON_NULL;
                } else {
                    err = KVM_JSON_ERR_SYNTAX;
                    goto fail;
                }
                t = tok_new(doc, ty, parent, pos);
                if (t < 0) {
                    err = KVM_JSON_ERR_TOKENS;
                    goto fail;
                }
                doc->tok[t].len = wl;
                pos = (uint16_t)(pos + wl);
            }
            if (parent >= 0 && pending_key < 0) {
                doc->tok[parent].count++; // array element; an object member was counted with its key
            }
            pending_key = -1;
            st = ST_AFTER;
            break;
        }

        case ST_KEY: {
            if (c == '}') {
                if (doc->tok[stack[sp - 1u]].count != 0u) {
                    err = KVM_JSON_ERR_SYNTAX; // trailing comma
                    goto fail;
                }
                pos++;
                sp--;
                st = ST_AFTER;
                break;
            }
            if (c != '"') {
                err = KVM_JSON_ERR_SYNTAX;
                goto fail;
            }
            uint16_t bs = 0;
            uint16_t bl = 0;
            uint16_t nx = 0;
            if (!scan_string(text, n, pos, &bs, &bl, &nx)) {
                err = KVM_JSON_ERR_SYNTAX;
                goto fail;
            }
            if (dup_key(doc, parent, bs, bl)) {
                err = KVM_JSON_ERR_DUP_KEY;
                goto fail;
            }
            int t = tok_new(doc, KVM_JSON_STRING, parent, bs);
            if (t < 0) {
                err = KVM_JSON_ERR_TOKENS;
                goto fail;
            }
            doc->tok[t].len = bl;
            doc->tok[t].flags |= KVM_JSON_FLAG_KEY;
            doc->tok[parent].count++;
            pending_key = t;
            pos = nx;
            st = ST_COLON;
            break;
        }

        case ST_COLON:
            if (c != ':') {
                err = KVM_JSON_ERR_SYNTAX;
                goto fail;
            }
            pos++;
            st = ST_VALUE;
            break;

        case ST_AFTER:
            if (c == ',') {
                pos++;
                st = (doc->tok[stack[sp - 1u]].type == (uint8_t)KVM_JSON_OBJECT) ? ST_KEY : ST_VALUE;
                break;
            }
            if ((c == '}' && doc->tok[stack[sp - 1u]].type == (uint8_t)KVM_JSON_OBJECT) ||
                (c == ']' && doc->tok[stack[sp - 1u]].type == (uint8_t)KVM_JSON_ARRAY)) {
                pos++;
                sp--;
                st = ST_AFTER;
                break;
            }
            err = KVM_JSON_ERR_SYNTAX;
            goto fail;

        default:
            err = KVM_JSON_ERR_SYNTAX;
            goto fail;
        }
    }

    if (doc->ntok == 0u) {
        err = KVM_JSON_ERR_SYNTAX;
        goto fail;
    }
    return KVM_JSON_OK;

fail:
    doc->ntok = 0;
    doc->len = 0;
    return err;
}

// ---- accessors -----------------------------------------------------------------------------

int kvm_json_root(const kvm_json_doc_t *doc)
{
    return (doc->ntok == 0u) ? -1 : 0;
}

int kvm_json_member(const kvm_json_doc_t *doc, int obj, const char *name)
{
    if (obj < 0 || (unsigned)obj >= doc->ntok || doc->tok[obj].type != (uint8_t)KVM_JSON_OBJECT) {
        return -1;
    }
    uint16_t nlen = (uint16_t)strlen(name);
    for (uint16_t t = (uint16_t)(obj + 1); t < doc->ntok; t++) {
        const kvm_json_tok_t *k = &doc->tok[t];
        if (k->parent != (int16_t)obj || (k->flags & KVM_JSON_FLAG_KEY) == 0u) {
            continue;
        }
        if (span_equal(doc->text + k->start, k->len, name, nlen)) {
            return ((uint16_t)(t + 1u) < doc->ntok) ? (int)(t + 1) : -1;
        }
    }
    return -1;
}

uint16_t kvm_json_count(const kvm_json_doc_t *doc, int container)
{
    if (container < 0 || (unsigned)container >= doc->ntok) {
        return 0;
    }
    uint8_t ty = doc->tok[container].type;
    if (ty != (uint8_t)KVM_JSON_OBJECT && ty != (uint8_t)KVM_JSON_ARRAY) {
        return 0;
    }
    return doc->tok[container].count;
}

int kvm_json_element(const kvm_json_doc_t *doc, int arr, uint16_t index)
{
    if (arr < 0 || (unsigned)arr >= doc->ntok || doc->tok[arr].type != (uint8_t)KVM_JSON_ARRAY) {
        return -1;
    }
    uint16_t seen = 0;
    for (uint16_t t = (uint16_t)(arr + 1); t < doc->ntok; t++) {
        if (doc->tok[t].parent != (int16_t)arr) {
            continue;
        }
        if (seen == index) {
            return (int)t;
        }
        seen++;
    }
    return -1;
}

int kvm_json_key(const kvm_json_doc_t *doc, int obj, uint16_t index)
{
    if (obj < 0 || (unsigned)obj >= doc->ntok || doc->tok[obj].type != (uint8_t)KVM_JSON_OBJECT) {
        return -1;
    }
    uint16_t seen = 0;
    for (uint16_t t = (uint16_t)(obj + 1); t < doc->ntok; t++) {
        const kvm_json_tok_t *k = &doc->tok[t];
        if (k->parent != (int16_t)obj || (k->flags & KVM_JSON_FLAG_KEY) == 0u) {
            continue;
        }
        if (seen == index) {
            return (int)t;
        }
        seen++;
    }
    return -1;
}

bool kvm_json_keys_allowed(const kvm_json_doc_t *doc, int obj, const char *const *allowed)
{
    if (obj < 0 || (unsigned)obj >= doc->ntok || doc->tok[obj].type != (uint8_t)KVM_JSON_OBJECT) {
        return false;
    }
    for (uint16_t t = (uint16_t)(obj + 1); t < doc->ntok; t++) {
        const kvm_json_tok_t *k = &doc->tok[t];
        if (k->parent != (int16_t)obj || (k->flags & KVM_JSON_FLAG_KEY) == 0u) {
            continue;
        }
        bool ok = false;
        for (const char *const *a = allowed; *a != NULL; a++) {
            if (span_equal(doc->text + k->start, k->len, *a, (uint16_t)strlen(*a))) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool kvm_json_str_eq(const kvm_json_doc_t *doc, int tok, const char *s)
{
    if (tok < 0 || (unsigned)tok >= doc->ntok || doc->tok[tok].type != (uint8_t)KVM_JSON_STRING) {
        return false;
    }
    return span_equal(doc->text + doc->tok[tok].start, doc->tok[tok].len, s, (uint16_t)strlen(s));
}

size_t kvm_json_str_copy(const kvm_json_doc_t *doc, int tok, char *out, size_t cap)
{
    if (tok < 0 || (unsigned)tok >= doc->ntok || doc->tok[tok].type != (uint8_t)KVM_JSON_STRING || cap == 0u) {
        if (cap > 0u && out != NULL) {
            out[0] = '\0';
        }
        return (size_t)-1;
    }
    size_t r = unescape(doc->text + doc->tok[tok].start, doc->tok[tok].len, out, cap);
    if (r == (size_t)-1) {
        out[0] = '\0';
    }
    return r;
}

bool kvm_json_get_i64(const kvm_json_doc_t *doc, int tok, int64_t *out)
{
    if (tok < 0 || (unsigned)tok >= doc->ntok || doc->tok[tok].type != (uint8_t)KVM_JSON_NUMBER ||
        (doc->tok[tok].flags & KVM_JSON_FLAG_INT) == 0u) {
        return false;
    }
    const char *s = doc->text + doc->tok[tok].start;
    uint16_t len = doc->tok[tok].len;
    uint16_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
    }
    uint64_t v = 0;
    for (; i < len; i++) {
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (uint64_t)INT64_MAX / 10u) {
            return false; // magnitude already out of range
        }
        v = v * 10u + d;
        if (!neg && v > (uint64_t)INT64_MAX) {
            return false;
        }
        if (neg && v > (uint64_t)INT64_MAX + 1u) {
            return false;
        }
    }
    *out = neg ? (int64_t)(-(int64_t)(v - 1u) - 1) : (int64_t)v;
    return true;
}

bool kvm_json_get_bool(const kvm_json_doc_t *doc, int tok, bool *out)
{
    if (tok < 0 || (unsigned)tok >= doc->ntok || doc->tok[tok].type != (uint8_t)KVM_JSON_BOOL) {
        return false;
    }
    *out = (doc->text[doc->tok[tok].start] == 't');
    return true;
}

const char *kvm_json_err_name(kvm_json_err_t err)
{
    switch (err) {
    case KVM_JSON_OK:
        return "OK";
    case KVM_JSON_ERR_EMPTY:
        return "EMPTY";
    case KVM_JSON_ERR_SIZE:
        return "SIZE";
    case KVM_JSON_ERR_UTF8:
        return "UTF8";
    case KVM_JSON_ERR_SYNTAX:
        return "SYNTAX";
    case KVM_JSON_ERR_DEPTH:
        return "DEPTH";
    case KVM_JSON_ERR_TOKENS:
        return "TOKENS";
    case KVM_JSON_ERR_DUP_KEY:
        return "DUP_KEY";
    case KVM_JSON_ERR_TRAILING:
        return "TRAILING";
    default:
        return "UNKNOWN";
    }
}
