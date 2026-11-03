// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Bounded JSON reader for the CLIENT protocol v1 envelope (spec 03 §6.2, TECH-043).
//
// Properties the protocol depends on:
//   - hard frame limit of 1024 bytes (03 §6.1) and a hard token budget;
//   - iterative parser, no recursion anywhere, so the stack cost does not depend on the input;
//   - zero allocation: every byte lives in the caller's frame buffer and in kvm_json_doc_t;
//   - fail closed: duplicate keys, trailing bytes, control characters in strings, bad UTF-8,
//     lone surrogates and non-JSON numbers are errors, never "best effort" values;
//   - the document keeps a pointer to the caller's frame, it never copies it. The frame must stay
//     valid and unmodified while the document is read.
//
// Pure C11, no ESP-IDF dependency (host unit tests in test/remote_protocol).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum textual frame accepted by the reader (03 §6.1 `max_frame_size`). */
#define KVM_JSON_MAX_FRAME 1024u

/** Token budget for one frame. A v1 envelope needs ~24; `pair.round1` with base64 needs ~16. */
#define KVM_JSON_MAX_TOKENS 64u

/**
 * Maximum container nesting: 1 envelope object + 1 payload object + 1 array of scalars, which is
 * everything protocol v1 defines. 03 §6.2: "payload: typed, nowhere nested recursively".
 */
#define KVM_JSON_MAX_DEPTH 3u

/** Scratch size used by kvm_json_str_eq() for a string that contains escapes. */
#define KVM_JSON_ESCAPE_SCRATCH 64u

typedef enum {
    KVM_JSON_UNDEFINED = 0,
    KVM_JSON_OBJECT,
    KVM_JSON_ARRAY,
    KVM_JSON_STRING,
    KVM_JSON_NUMBER,
    KVM_JSON_BOOL,
    KVM_JSON_NULL,
} kvm_json_type_t;

typedef enum {
    KVM_JSON_OK = 0,
    KVM_JSON_ERR_EMPTY,     // zero-length frame
    KVM_JSON_ERR_SIZE,      // > KVM_JSON_MAX_FRAME
    KVM_JSON_ERR_UTF8,      // not well-formed UTF-8 (03 §6.1: textual UTF-8 JSON)
    KVM_JSON_ERR_SYNTAX,    // any grammar violation, control char in a string, bad escape
    KVM_JSON_ERR_DEPTH,     // nesting beyond KVM_JSON_MAX_DEPTH
    KVM_JSON_ERR_TOKENS,    // token budget exhausted
    KVM_JSON_ERR_DUP_KEY,   // the same member name twice in one object (ambiguous -> reject)
    KVM_JSON_ERR_TRAILING,  // bytes after the root value
    KVM_JSON_ERR_COUNT,
} kvm_json_err_t;

typedef struct {
    uint8_t type;    // kvm_json_type_t
    uint8_t flags;   // bit 0: this STRING is a member name; bit 1: NUMBER has no fraction/exponent
    int16_t parent;  // index of the containing OBJECT/ARRAY, -1 for the root
    uint16_t start;  // byte offset in the frame; for STRING the first byte inside the quotes
    uint16_t len;    // byte length; for STRING the raw span (escapes not expanded)
    uint16_t count;  // OBJECT: members; ARRAY: elements; otherwise 0
} kvm_json_tok_t;

#define KVM_JSON_FLAG_KEY 0x01u
#define KVM_JSON_FLAG_INT 0x02u

typedef struct {
    const char *text; // the caller's frame, not owned
    uint16_t len;
    uint16_t ntok;
    kvm_json_tok_t tok[KVM_JSON_MAX_TOKENS];
} kvm_json_doc_t;

/**
 * Parse one frame. On any error `doc->ntok` is 0 and no partial document is exposed.
 * `text` must stay valid while the document is read.
 */
kvm_json_err_t kvm_json_parse(kvm_json_doc_t *doc, const char *text, size_t len);

/** Root token index (0) when the document holds a value, or -1 for an empty document. */
int kvm_json_root(const kvm_json_doc_t *doc);

static inline kvm_json_type_t kvm_json_type(const kvm_json_doc_t *doc, int tok)
{
    if (tok < 0 || (unsigned)tok >= doc->ntok) {
        return KVM_JSON_UNDEFINED;
    }
    return (kvm_json_type_t)doc->tok[tok].type;
}

/** Value token of member `name` of the object `obj`, or -1 when absent / `obj` is not an object. */
int kvm_json_member(const kvm_json_doc_t *doc, int obj, const char *name);

/** Number of members of an object / elements of an array; 0 otherwise. */
uint16_t kvm_json_count(const kvm_json_doc_t *doc, int container);

/** Element `index` of an array, or -1. */
int kvm_json_element(const kvm_json_doc_t *doc, int arr, uint16_t index);

/** Member-name token number `index` of an object, or -1. Its value token is the next index. */
int kvm_json_key(const kvm_json_doc_t *doc, int obj, uint16_t index);

/** True when every member name of `obj` appears in the NULL-terminated `allowed` list. */
bool kvm_json_keys_allowed(const kvm_json_doc_t *doc, int obj, const char *const *allowed);

/** Compare a STRING token with a C string after unescaping. False for any other token type. */
bool kvm_json_str_eq(const kvm_json_doc_t *doc, int tok, const char *s);

/**
 * Unescape a STRING token into `out` (always NUL-terminated when cap > 0).
 * Returns the byte length, or (size_t)-1 when the token is not a string, does not fit, or holds a
 * lone surrogate / an escape that decodes to NUL.
 */
size_t kvm_json_str_copy(const kvm_json_doc_t *doc, int tok, char *out, size_t cap);

/** Integer NUMBER token (no fraction, no exponent, fits in int64). */
bool kvm_json_get_i64(const kvm_json_doc_t *doc, int tok, int64_t *out);

/** BOOL token. */
bool kvm_json_get_bool(const kvm_json_doc_t *doc, int tok, bool *out);

const char *kvm_json_err_name(kvm_json_err_t err);

#ifdef __cplusplus
}
#endif
