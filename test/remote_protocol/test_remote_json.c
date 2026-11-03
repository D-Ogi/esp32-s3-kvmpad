// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Bounded JSON reader (spec 03 §6.2, TECH-043): grammar, limits and fail-closed behavior.
// Covers the AT-SEC-04 parser rows at the reader level.
#include <string.h>

#include "kvm_test.h"
#include "remote_json.h"

static kvm_json_doc_t doc;

static kvm_json_err_t parse(const char *s)
{
    return kvm_json_parse(&doc, s, strlen(s));
}

static void test_envelope_shape(void)
{
    const char *f = "{\"v\":1,\"type\":\"input\",\"seq\":203,\"session_id\":\"c-815a\","
                    "\"lease_epoch\":7,\"payload\":{\"kind\":\"mouse.move\",\"dx\":14,\"dy\":-5}}";
    CHECK_EQ(parse(f), KVM_JSON_OK);
    int root = kvm_json_root(&doc);
    CHECK_EQ(kvm_json_type(&doc, root), KVM_JSON_OBJECT);
    CHECK_EQ(kvm_json_count(&doc, root), 6);

    int64_t v = 0;
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, root, "v"), &v));
    CHECK_EQ(v, 1);
    CHECK(kvm_json_str_eq(&doc, kvm_json_member(&doc, root, "type"), "input"));
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, root, "seq"), &v));
    CHECK_EQ(v, 203);
    CHECK(kvm_json_str_eq(&doc, kvm_json_member(&doc, root, "session_id"), "c-815a"));

    int pl = kvm_json_member(&doc, root, "payload");
    CHECK_EQ(kvm_json_type(&doc, pl), KVM_JSON_OBJECT);
    CHECK_EQ(kvm_json_count(&doc, pl), 3);
    CHECK(kvm_json_str_eq(&doc, kvm_json_member(&doc, pl, "kind"), "mouse.move"));
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, pl, "dy"), &v));
    CHECK_EQ(v, -5);
    CHECK_EQ(kvm_json_member(&doc, pl, "dz"), -1);
    CHECK_EQ(kvm_json_member(&doc, root, "kind"), -1); // not a transitive lookup
}

static void test_arrays_and_literals(void)
{
    CHECK_EQ(parse("{\"a\":[1,2,3],\"b\":true,\"c\":false,\"d\":null,\"e\":[]}"), KVM_JSON_OK);
    int root = kvm_json_root(&doc);
    int a = kvm_json_member(&doc, root, "a");
    CHECK_EQ(kvm_json_type(&doc, a), KVM_JSON_ARRAY);
    CHECK_EQ(kvm_json_count(&doc, a), 3);
    int64_t v = 0;
    CHECK(kvm_json_get_i64(&doc, kvm_json_element(&doc, a, 2), &v));
    CHECK_EQ(v, 3);
    CHECK_EQ(kvm_json_element(&doc, a, 3), -1);
    bool b = false;
    CHECK(kvm_json_get_bool(&doc, kvm_json_member(&doc, root, "b"), &b));
    CHECK(b);
    CHECK(kvm_json_get_bool(&doc, kvm_json_member(&doc, root, "c"), &b));
    CHECK(!b);
    CHECK_EQ(kvm_json_type(&doc, kvm_json_member(&doc, root, "d")), KVM_JSON_NULL);
    CHECK_EQ(kvm_json_count(&doc, kvm_json_member(&doc, root, "e")), 0);
    CHECK_EQ(parse("{\"a\":[\"x\",\"y\"]}"), KVM_JSON_OK);
    CHECK(kvm_json_str_eq(&doc, kvm_json_element(&doc, kvm_json_member(&doc, 0, "a"), 1), "y"));
}

static void test_syntax_rejected(void)
{
    static const char *const bad[] = {
        "",                      // empty
        " ",                     // whitespace only
        "{",                     // unterminated object
        "}",                     //
        "[",                     //
        "{\"a\"}",               // missing colon
        "{\"a\":}",              // missing value
        "{\"a\":1,}",            // trailing comma
        "[1,]",                  //
        "{,}",                   //
        "{\"a\":1}{\"b\":2}",    // two documents
        "{\"a\":1} x",           // trailing junk
        "{\"a\":01}",            // leading zero
        "{\"a\":+1}",            //
        "{\"a\":.5}",            //
        "{\"a\":1.}",            //
        "{\"a\":1e}",            //
        "{\"a\":--1}",           //
        "{\"a\":tru}",           //
        "{\"a\":TRUE}",          //
        "{\"a\":'x'}",           // single quotes are not JSON
        "{a:1}",                 // bare key
        "{\"a\":\"x}",           // unterminated string
        "{\"a\":\"x\\q\"}",      // unknown escape
        "{\"a\":\"x\\u00\"}",    // short \u
        "{\"a\":\"x\\uZZZZ\"}",  // non-hex \u
        "{\"a\":\"line\nbreak\"}", // raw control character in a string
        "{\"a\":1 \"b\":2}",     // missing comma
        "{\"a\",1}",             //
        "{\"a\":[1,2}",          // mismatched close
        "{\"a\":{\"b\":1]}",     //
        NULL,
    };
    for (int i = 0; bad[i] != NULL; i++) {
        kvm_json_err_t e = parse(bad[i]);
        CHECK(e != KVM_JSON_OK);
        CHECK_EQ(doc.ntok, 0); // no partial document is ever exposed
    }
}

static void test_duplicate_keys(void)
{
    CHECK_EQ(parse("{\"v\":1,\"v\":2}"), KVM_JSON_ERR_DUP_KEY);
    // the duplicate is detected after unescaping, so a smuggled "\u0076" cannot hide a second "v"
    CHECK_EQ(parse("{\"v\":1,\"\\u0076\":2}"), KVM_JSON_ERR_DUP_KEY);
    CHECK_EQ(parse("{\"payload\":{\"dx\":1,\"dx\":2}}"), KVM_JSON_ERR_DUP_KEY);
    CHECK_EQ(parse("{\"a\":{\"x\":1},\"b\":{\"x\":1}}"), KVM_JSON_OK); // different objects
}

static void test_limits(void)
{
    char big[KVM_JSON_MAX_FRAME + 64];
    memset(big, 'x', sizeof(big));
    // 03 §6.5: >1024 B -> reject, never a partial parse
    CHECK_EQ(kvm_json_parse(&doc, big, KVM_JSON_MAX_FRAME + 1u), KVM_JSON_ERR_SIZE);

    // exactly 1024 B of valid JSON is accepted
    char ok[KVM_JSON_MAX_FRAME + 1];
    int n = snprintf(ok, sizeof(ok), "{\"v\":1,\"type\":\"hello\",\"seq\":1,\"payload\":{\"p\":\"");
    while ((size_t)n < KVM_JSON_MAX_FRAME - 3u) {
        ok[n++] = 'a';
    }
    memcpy(ok + n, "\"}}", 3);
    n += 3;
    CHECK_EQ((size_t)n, KVM_JSON_MAX_FRAME);
    CHECK_EQ(kvm_json_parse(&doc, ok, (size_t)n), KVM_JSON_OK);

    // depth: 03 §6.2 forbids recursive nesting
    CHECK_EQ(parse("{\"a\":{\"b\":[1,2]}}"), KVM_JSON_OK); // envelope + payload + array
    CHECK_EQ(parse("{\"a\":{\"b\":{\"c\":1}}}"), KVM_JSON_OK);
    CHECK_EQ(parse("{\"a\":{\"b\":{\"c\":{\"d\":1}}}}"), KVM_JSON_ERR_DEPTH);
    CHECK_EQ(parse("[[[1]]]"), KVM_JSON_OK);
    CHECK_EQ(parse("[[[[1]]]]"), KVM_JSON_ERR_DEPTH);

    // token budget
    char many[KVM_JSON_MAX_FRAME];
    size_t o = 0;
    many[o++] = '[';
    for (int i = 0; i < 80; i++) {
        many[o++] = '1';
        many[o++] = ',';
    }
    many[o - 1] = ']';
    CHECK_EQ(kvm_json_parse(&doc, many, o), KVM_JSON_ERR_TOKENS);
}

static void test_utf8_and_escapes(void)
{
    char out[64];
    CHECK_EQ(parse("{\"n\":\"Desk\\u00f31\"}"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_str_copy(&doc, kvm_json_member(&doc, 0, "n"), out, sizeof(out)), 7);
    CHECK_EQ(memcmp(out, "Desk\xc3\xb3" "1", 8), 0);

    CHECK_EQ(parse("{\"n\":\"a\\\"b\\\\c\\/d\\te\"}"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_str_copy(&doc, kvm_json_member(&doc, 0, "n"), out, sizeof(out)), 9);
    CHECK_EQ(memcmp(out, "a\"b\\c/d\te", 10), 0);

    // surrogate pair -> one code point; a lone surrogate is refused
    CHECK_EQ(parse("{\"n\":\"\\ud83d\\ude00\"}"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_str_copy(&doc, kvm_json_member(&doc, 0, "n"), out, sizeof(out)), 4);
    CHECK_EQ(parse("{\"n\":\"\\ud83d\"}"), KVM_JSON_OK); // valid grammar
    CHECK_EQ(kvm_json_str_copy(&doc, kvm_json_member(&doc, 0, "n"), out, sizeof(out)), (size_t)-1);
    CHECK_EQ(parse("{\"n\":\"\\udc00\"}"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_str_copy(&doc, kvm_json_member(&doc, 0, "n"), out, sizeof(out)), (size_t)-1);
    // an escaped NUL must not reach a C string
    CHECK_EQ(parse("{\"n\":\"a\\u0000b\"}"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_str_copy(&doc, kvm_json_member(&doc, 0, "n"), out, sizeof(out)), (size_t)-1);

    // malformed UTF-8 in the frame itself
    CHECK_EQ(parse("{\"n\":\"\xc3\"}"), KVM_JSON_ERR_UTF8);          // truncated
    CHECK_EQ(parse("{\"n\":\"\x80\x80\"}"), KVM_JSON_ERR_UTF8);      // lone continuation
    CHECK_EQ(parse("{\"n\":\"\xc0\xaf\"}"), KVM_JSON_ERR_UTF8);      // overlong "/"
    CHECK_EQ(parse("{\"n\":\"\xed\xa0\x80\"}"), KVM_JSON_ERR_UTF8);  // encoded surrogate
    CHECK_EQ(parse("{\"n\":\"\xf5\x80\x80\x80\"}"), KVM_JSON_ERR_UTF8); // beyond U+10FFFF
    CHECK_EQ(parse("{\"n\":\"\xc3\xa9\"}"), KVM_JSON_OK);            // valid two-byte

    // a value that does not fit the caller's buffer fails instead of truncating
    char tiny[4];
    CHECK_EQ(parse("{\"n\":\"abcdef\"}"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_str_copy(&doc, kvm_json_member(&doc, 0, "n"), tiny, sizeof(tiny)), (size_t)-1);
    CHECK_EQ(tiny[0], '\0');
}

static void test_numbers(void)
{
    int64_t v = 0;
    CHECK_EQ(parse("{\"a\":9007199254740991,\"b\":-9007199254740991,\"c\":0,\"d\":-0}"),
             KVM_JSON_OK);
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "a"), &v));
    CHECK_EQ(v, 9007199254740991ll);
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "b"), &v));
    CHECK_EQ(v, -9007199254740991ll);
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "c"), &v));
    CHECK_EQ(v, 0);

    // int64 bounds
    CHECK_EQ(parse("{\"a\":9223372036854775807,\"b\":-9223372036854775808}"), KVM_JSON_OK);
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "a"), &v));
    CHECK_EQ(v, INT64_MAX);
    CHECK(kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "b"), &v));
    CHECK_EQ(v, INT64_MIN);
    CHECK_EQ(parse("{\"a\":9223372036854775808}"), KVM_JSON_OK); // valid grammar
    CHECK(!kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "a"), &v)); // out of range
    CHECK_EQ(parse("{\"a\":99999999999999999999999999}"), KVM_JSON_OK);
    CHECK(!kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "a"), &v));

    // a fractional or exponential number is never an integer field
    CHECK_EQ(parse("{\"a\":1.5,\"b\":1e3,\"c\":1E+3,\"d\":-0.0}"), KVM_JSON_OK);
    CHECK(!kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "a"), &v));
    CHECK(!kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "b"), &v));
    CHECK(!kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "c"), &v));

    // type confusion is impossible: a string is not a number and vice versa
    CHECK_EQ(parse("{\"a\":\"1\",\"b\":1}"), KVM_JSON_OK);
    CHECK(!kvm_json_get_i64(&doc, kvm_json_member(&doc, 0, "a"), &v));
    CHECK(!kvm_json_str_eq(&doc, kvm_json_member(&doc, 0, "b"), "1"));
    bool bb = false;
    CHECK(!kvm_json_get_bool(&doc, kvm_json_member(&doc, 0, "b"), &bb));
}

static void test_keys_allowed(void)
{
    static const char *const env[] = {"v", "type", "seq", NULL};
    static const char *const none[] = {NULL};
    CHECK_EQ(parse("{\"v\":1,\"type\":\"ping\",\"seq\":2}"), KVM_JSON_OK);
    CHECK(kvm_json_keys_allowed(&doc, 0, env));
    CHECK_EQ(parse("{\"v\":1,\"evil\":1}"), KVM_JSON_OK);
    CHECK(!kvm_json_keys_allowed(&doc, 0, env));
    CHECK_EQ(parse("{}"), KVM_JSON_OK);
    CHECK(kvm_json_keys_allowed(&doc, 0, none));
    CHECK(kvm_json_keys_allowed(&doc, 0, env));
    CHECK_EQ(parse("{\"a\":1}"), KVM_JSON_OK);
    CHECK(!kvm_json_keys_allowed(&doc, 0, none));
    // nested members belong to their own object, not to the root
    CHECK_EQ(parse("{\"v\":1,\"type\":\"x\",\"seq\":1}"), KVM_JSON_OK);
    CHECK(kvm_json_keys_allowed(&doc, 0, env));
}

static void test_non_object_roots(void)
{
    CHECK_EQ(parse("[1,2]"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_type(&doc, kvm_json_root(&doc)), KVM_JSON_ARRAY);
    CHECK_EQ(parse("42"), KVM_JSON_OK);
    CHECK_EQ(kvm_json_type(&doc, kvm_json_root(&doc)), KVM_JSON_NUMBER);
    CHECK_EQ(parse("\"x\""), KVM_JSON_OK);
    CHECK_EQ(kvm_json_type(&doc, kvm_json_root(&doc)), KVM_JSON_STRING);
    // accessors on the wrong token type stay silent instead of reading out of bounds
    CHECK_EQ(kvm_json_member(&doc, 0, "a"), -1);
    CHECK_EQ(kvm_json_member(&doc, -1, "a"), -1);
    CHECK_EQ(kvm_json_member(&doc, 99, "a"), -1);
    CHECK_EQ(kvm_json_count(&doc, 0), 0);
    CHECK_EQ(kvm_json_element(&doc, 0, 0), -1);
    CHECK_EQ(kvm_json_key(&doc, 0, 0), -1);
    CHECK(!kvm_json_str_eq(&doc, -1, "x"));
    CHECK_EQ(kvm_json_type(&doc, 500), KVM_JSON_UNDEFINED);
}

static void test_injection_strings(void)
{
    // AT-SEC-04: SQL / script strings are just bytes to the reader; nothing is evaluated
    static const char *const payloads[] = {
        "{\"n\":\"'; DROP TABLE clients;--\"}",
        "{\"n\":\"<script>alert(1)</script>\"}",
        "{\"n\":\"../../etc/passwd\"}",
        "{\"n\":\"%s%n%p\"}",
        "{\"n\":\"${jndi:ldap://x/y}\"}",
        NULL,
    };
    char out[64];
    for (int i = 0; payloads[i] != NULL; i++) {
        CHECK_EQ(parse(payloads[i]), KVM_JSON_OK);
        int t = kvm_json_member(&doc, 0, "n");
        CHECK_EQ(kvm_json_type(&doc, t), KVM_JSON_STRING);
        CHECK(kvm_json_str_copy(&doc, t, out, sizeof(out)) != (size_t)-1);
    }
}

int main(void)
{
    printf("remote_json\n");
    RUN(test_envelope_shape);
    RUN(test_arrays_and_literals);
    RUN(test_syntax_rejected);
    RUN(test_duplicate_keys);
    RUN(test_limits);
    RUN(test_utf8_and_escapes);
    RUN(test_numbers);
    RUN(test_keys_allowed);
    RUN(test_non_object_roots);
    RUN(test_injection_strings);
    TEST_MAIN_END("remote_json");
}
