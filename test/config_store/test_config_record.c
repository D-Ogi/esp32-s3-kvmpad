// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the settings record (TASK-090): defaults, per-field ranges, validate-or-default,
// CRC32 serialization, every single-bit corruption, truncated and oversized records, a record from
// newer firmware, the v1 -> v2, v2 -> v3 and v3 -> v4 migrations, the approval-policy floor and the
// two ADR-039 HOST-layout lists.
//
// Covers AT-FAULT-02 (invalid checksum -> factory-safe defaults) and the record half of
// AT-EXT-18 / FR-736 (unknown or corrupt value -> default, migration never relaxes a policy).

#include <stdio.h>
#include <string.h>

#include "kvm_config.h"
#include "kvm_test.h"

#define HDR KVM_CONFIG_RECORD_HEADER_LEN

// Fixed payload parts of the older schemas, frozen here as literals so a migration test is written
// against the old layout and not against whatever the current one happens to be.
#define V2_FIXED 41u // schema 2: before ADR-037 added the three language bytes
#define V3_FIXED 44u // schema 3: before ADR-039 appended the two HOST-layout lists

// Payload offsets of the two ADR-039 lists.
#define FAV_COUNT_OFF V3_FIXED
#define FAV_OFF (FAV_COUNT_OFF + 1u)
#define RECENT_COUNT_OFF (FAV_OFF + KVM_CONFIG_HOST_FAV_MAX * KVM_CONFIG_LAYOUT_ID_CAP)
#define RECENT_OFF (RECENT_COUNT_OFF + 1u)

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

// Stamp the magic, the schema, the payload length and a matching CRC onto a raw record.
static void stamp(uint8_t *rec, uint16_t schema, uint16_t payload_len)
{
    rec[0] = 'K';
    rec[1] = 'V';
    rec[2] = 'C';
    rec[3] = 'S';
    put_u16le(rec + 8, schema);
    put_u16le(rec + 10, payload_len);
    put_u32le(rec + 4, kvm_config_crc32(rec + 8, (size_t)payload_len + 4u));
}

static void restamp_crc(uint8_t *rec, size_t len)
{
    put_u32le(rec + 4, kvm_config_crc32(rec + 8, len - 8u));
}

static void fill_populated(kvm_config_t *c)
{
    kvm_config_defaults(c);
    CHECK(kvm_config_set(c, KVM_CFG_F_MOUSE_SENSITIVITY, KVM_MOUSE_SENS_FAST));
    CHECK(kvm_config_set(c, KVM_CFG_F_NATURAL_SCROLL, 1));
    CHECK(kvm_config_set(c, KVM_CFG_F_LONGPRESS_RIGHT_CLICK, 0));
    CHECK(kvm_config_set(c, KVM_CFG_F_TWO_FINGER_SCROLL, 0));
    CHECK(kvm_config_set(c, KVM_CFG_F_SCROLL_SPEED, 10));
    CHECK(kvm_config_set(c, KVM_CFG_F_BRIGHTNESS, 10));
    CHECK(kvm_config_set(c, KVM_CFG_F_REMOTE_ENABLED, 1));
    CHECK(kvm_config_set(c, KVM_CFG_F_DIAG_OVERLAY, 1));
    CHECK(kvm_config_set(c, KVM_CFG_F_PAIR_REQUIRE_ACCEPT, 0));
    CHECK(kvm_config_set(c, KVM_CFG_F_CONTROL_APPROVAL, KVM_CONTROL_APPROVAL_AUTO));
    CHECK(kvm_config_set(c, KVM_CFG_F_CONFIRM_ALT_F4, 1));
    CHECK(kvm_config_set(c, KVM_CFG_F_KEEPAWAKE_ENABLED, 1));
    CHECK(kvm_config_set(c, KVM_CFG_F_KEEPAWAKE_INTERVAL_S, 300));
    CHECK(kvm_config_set(c, KVM_CFG_F_UNICODE_MODE, KVM_UNICODE_MODE_WINCOMPOSE));
    CHECK(kvm_config_set(c, KVM_CFG_F_LANG_KO, 1));
    CHECK(kvm_config_set(c, KVM_CFG_F_LANG_ZH, 1));
    CHECK(kvm_config_set(c, KVM_CFG_F_LANG_JA, 1));
    CHECK(kvm_config_set_layout_id(c, "de(neo)"));

    c->wifi.valid = 1;
    c->wifi.bssid[0] = 0x02;
    c->wifi.bssid[1] = 0x11;
    c->wifi.bssid[2] = 0x22;
    c->wifi.bssid[3] = 0x33;
    c->wifi.bssid[4] = 0x44;
    c->wifi.bssid[5] = 0x55;
    c->wifi.channel = 11;

    for (unsigned i = 0; i < KVM_CONFIG_MAX_PAIRED; i++) {
        uint8_t sha[KVM_CONFIG_TOKEN_SHA_LEN];
        for (unsigned k = 0; k < sizeof sha; k++) {
            sha[k] = (uint8_t)(0x40u + i * 31u + k);
        }
        char name[KVM_CONFIG_PAIR_NAME_CAP];
        snprintf(name, sizeof name, "desk-%u", i);
        CHECK(kvm_config_pair_add(c, name, sha, 1762000000u + i, (i % 2u) == 0u) == (int)i);
    }
}

// ---------------------------------------------------------------------------------------------

static void test_defaults(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);

    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        uint32_t min = 0, max = 0, def = 0;
        CHECK(kvm_config_field_range((kvm_config_field_t)f, &min, &max, &def));
        CHECK_EQ(kvm_config_get(&c, (kvm_config_field_t)f), def);
    }

    // 01 §8 and ADR-023: the shipped record is the least surprising and the most restrictive one.
    CHECK_EQ(c.mouse_sensitivity, KVM_MOUSE_SENS_NORMAL);
    CHECK_EQ(c.natural_scroll, 0);
    CHECK_EQ(c.longpress_right_click, 1);
    CHECK_EQ(c.two_finger_scroll, 1);
    CHECK_EQ(c.remote_enabled, 0);
    CHECK_EQ(c.diag_overlay, 0);
    CHECK_EQ(c.pair_require_accept, 1);
    CHECK_EQ(c.control_approval, KVM_CONTROL_APPROVAL_ALWAYS);
    CHECK_EQ(c.confirm_alt_f4, 0);
    CHECK_EQ(c.keepawake_enabled, 0);
    CHECK_EQ(c.keepawake_interval_s, 30);
    CHECK_EQ(c.unicode_mode, KVM_UNICODE_MODE_HOST_IME);
    // ADR-037: a fresh device types English alone; the three composed languages are off.
    CHECK_EQ(c.lang_ko, 0);
    CHECK_EQ(c.lang_zh, 0);
    CHECK_EQ(c.lang_ja, 0);
    CHECK(strcmp(c.host_layout, "us") == 0);
    CHECK_EQ(c.wifi.valid, 0);
    CHECK_EQ(c.pair_count, 0);

    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(!kvm_config_repair_any(&rep));
}

static void test_field_table(void)
{
    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        uint32_t min = 0, max = 0, def = 0;
        CHECK(kvm_config_field_range((kvm_config_field_t)f, &min, &max, &def));
        const char *key = kvm_config_field_key((kvm_config_field_t)f);
        CHECK(key[0] != '\0');
        CHECK(min <= def && def <= max);
        CHECK(min < max);
        for (unsigned g = f + 1u; g < (unsigned)KVM_CFG_F_COUNT; g++) {
            CHECK(strcmp(key, kvm_config_field_key((kvm_config_field_t)g)) != 0);
        }
    }

    // Out-of-table indices must be inert, not a read past the array.
    CHECK(!kvm_config_field_range((kvm_config_field_t)KVM_CFG_F_COUNT, NULL, NULL, NULL));
    CHECK(kvm_config_field_key((kvm_config_field_t)KVM_CFG_F_COUNT)[0] == '\0');

    // ADR-023: exactly three policies, and the two security policies default to the strictest
    // value of their range, so a corrupt record can never relax them.
    unsigned policies = 0;
    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        if (kvm_config_field_is_policy((kvm_config_field_t)f)) {
            policies++;
        }
    }
    CHECK_EQ(policies, 3);
    CHECK(kvm_config_field_is_policy(KVM_CFG_F_PAIR_REQUIRE_ACCEPT));
    CHECK(kvm_config_field_is_policy(KVM_CFG_F_CONTROL_APPROVAL));
    CHECK(kvm_config_field_is_policy(KVM_CFG_F_CONFIRM_ALT_F4));
    CHECK(!kvm_config_field_is_policy(KVM_CFG_F_BRIGHTNESS));

    uint32_t min = 0, max = 0, def = 0;
    CHECK(kvm_config_field_range(KVM_CFG_F_PAIR_REQUIRE_ACCEPT, &min, &max, &def));
    CHECK_EQ(def, max);
    CHECK(kvm_config_field_range(KVM_CFG_F_CONTROL_APPROVAL, &min, &max, &def));
    CHECK_EQ(def, max);
}

static void test_every_field_out_of_range_falls_back(void)
{
    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        kvm_config_field_t field = (kvm_config_field_t)f;
        uint32_t min = 0, max = 0, def = 0;
        CHECK(kvm_config_field_range(field, &min, &max, &def));

        // above the range
        kvm_config_t c;
        kvm_config_defaults(&c);
        kvm_config_set_unchecked(&c, field, max + 1u);
        kvm_config_repair_t rep = kvm_config_validate(&c);
        CHECK_EQ(kvm_config_get(&c, field), def);
        CHECK_EQ(rep.field_mask, (uint32_t)1u << f);

        // below the range
        if (min > 0u) {
            kvm_config_defaults(&c);
            kvm_config_set_unchecked(&c, field, min - 1u);
            rep = kvm_config_validate(&c);
            CHECK_EQ(kvm_config_get(&c, field), def);
            CHECK_EQ(rep.field_mask, (uint32_t)1u << f);
        }

        // the setter refuses the same values without touching the record
        kvm_config_defaults(&c);
        CHECK(!kvm_config_set(&c, field, max + 1u));
        CHECK_EQ(kvm_config_get(&c, field), def);
        if (min > 0u) {
            CHECK(!kvm_config_set(&c, field, min - 1u));
            CHECK_EQ(kvm_config_get(&c, field), def);
        }
        CHECK(kvm_config_set(&c, field, min));
        CHECK(kvm_config_set(&c, field, max));
    }
}

static void test_all_fields_corrupt_at_once(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        uint32_t min = 0, max = 0, def = 0;
        (void)kvm_config_field_range((kvm_config_field_t)f, &min, &max, &def);
        kvm_config_set_unchecked(&c, (kvm_config_field_t)f, max + 7u);
    }
    kvm_config_repair_t rep = kvm_config_validate(&c);

    uint32_t all = ((uint32_t)1u << (unsigned)KVM_CFG_F_COUNT) - 1u;
    CHECK_EQ(rep.field_mask, all);

    kvm_config_t fresh;
    kvm_config_defaults(&fresh);
    CHECK(memcmp(&c, &fresh, sizeof fresh) == 0);
}

static void test_layout_ids(void)
{
    static const char *good[] = {"us", "de", "gb", "de(neo)", "fr(oss)", "us-intl", "ara_x",
                                 "latam(dvorak)", "abcdefghijklmno"};
    static const char *bad[] = {"",      "US",      "de(",     "de()",     "de(neo",
                                "(neo)", "de(ne o)", "de neo",  "us;rm -rf", "abcdefghijklmnop",
                                "de(neo))", "(", ")"};
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
        CHECK(kvm_config_layout_id_valid(good[i]));
    }
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!kvm_config_layout_id_valid(bad[i]));
    }
    CHECK(!kvm_config_layout_id_valid(NULL));

    kvm_config_t c;
    kvm_config_defaults(&c);
    CHECK(kvm_config_set_layout_id(&c, "de(neo)"));
    CHECK(strcmp(c.host_layout, "de(neo)") == 0);
    CHECK(!kvm_config_set_layout_id(&c, "DE"));
    CHECK(strcmp(c.host_layout, "de(neo)") == 0);

    // A layout id with no terminator inside the field is repaired to the default (FR-741).
    memset(c.host_layout, 'a', sizeof c.host_layout);
    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(rep.layout_repaired);
    CHECK(strcmp(c.host_layout, "us") == 0);
}

static void test_wifi_cache_validation(void)
{
    struct {
        uint8_t valid;
        uint8_t b0;
        uint8_t channel;
        bool ok;
    } cases[] = {
        {1, 0x02, 1, true},   {1, 0x02, 14, true}, {1, 0x02, 0, false},
        {1, 0x02, 15, false}, {1, 0x03, 6, false}, // group bit set: never a BSSID
        {2, 0x02, 6, false},                        // unknown flag value
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        kvm_config_t c;
        kvm_config_defaults(&c);
        c.wifi.valid = cases[i].valid;
        c.wifi.bssid[0] = cases[i].b0;
        c.wifi.bssid[5] = 0x55;
        c.wifi.channel = cases[i].channel;
        kvm_config_repair_t rep = kvm_config_validate(&c);
        CHECK_EQ(rep.wifi_repaired, !cases[i].ok);
        if (!cases[i].ok) {
            CHECK_EQ(c.wifi.valid, 0);
            CHECK_EQ(c.wifi.channel, 0);
        }
    }

    // An all-zero BSSID marked valid is not a usable hint.
    kvm_config_t c;
    kvm_config_defaults(&c);
    c.wifi.valid = 1;
    c.wifi.channel = 6;
    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(rep.wifi_repaired);

    // A cleared cache that still carries a channel is inconsistent and gets wiped.
    kvm_config_defaults(&c);
    c.wifi.channel = 6;
    rep = kvm_config_validate(&c);
    CHECK(rep.wifi_repaired);
    CHECK_EQ(c.wifi.channel, 0);
}

static void test_pairing_register(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);

    uint8_t sha_a[KVM_CONFIG_TOKEN_SHA_LEN];
    uint8_t sha_b[KVM_CONFIG_TOKEN_SHA_LEN];
    memset(sha_a, 0xA1, sizeof sha_a);
    memset(sha_b, 0xB2, sizeof sha_b);

    CHECK_EQ(kvm_config_pair_add(&c, "desk", sha_a, 100, false), 0);
    CHECK_EQ(kvm_config_pair_add(&c, "laptop", sha_b, 200, true), 1);
    CHECK_EQ(c.pair_count, 2);
    CHECK_EQ(kvm_config_pair_find(&c, sha_a), 0);
    CHECK_EQ(kvm_config_pair_find(&c, sha_b), 1);
    CHECK_EQ(c.pair[1].trusted, 1);
    CHECK_EQ(c.pair[1].paired_at_unix, 200);

    // The same digest refreshes the entry in place rather than filling the register.
    CHECK_EQ(kvm_config_pair_add(&c, "desk-renamed", sha_a, 300, true), 0);
    CHECK_EQ(c.pair_count, 2);
    CHECK(strcmp(c.pair[0].name, "desk-renamed") == 0);
    CHECK_EQ(c.pair[0].trusted, 1);

    // Rejected arguments
    uint8_t zero[KVM_CONFIG_TOKEN_SHA_LEN];
    memset(zero, 0, sizeof zero);
    CHECK_EQ(kvm_config_pair_add(&c, "x", zero, 1, false), -1);
    CHECK_EQ(kvm_config_pair_add(&c, "", sha_b, 1, false), -1);
    CHECK_EQ(kvm_config_pair_add(&c, NULL, sha_b, 1, false), -1);
    CHECK_EQ(kvm_config_pair_add(&c, "desk", NULL, 1, false), -1);
    CHECK_EQ(kvm_config_pair_add(&c, "tab\tname", sha_b, 1, false), -1);
    char too_long[KVM_CONFIG_PAIR_NAME_CAP + 4];
    memset(too_long, 'n', sizeof too_long - 1u);
    too_long[sizeof too_long - 1u] = '\0';
    CHECK_EQ(kvm_config_pair_add(&c, too_long, sha_b, 1, false), -1);
    CHECK_EQ(c.pair_count, 2);

    // Fill the register and refuse the ninth client.
    for (unsigned i = 2; i < KVM_CONFIG_MAX_PAIRED; i++) {
        uint8_t sha[KVM_CONFIG_TOKEN_SHA_LEN];
        memset(sha, (uint8_t)(0x10u + i), sizeof sha);
        CHECK_EQ(kvm_config_pair_add(&c, "filler", sha, i, false), (int)i);
    }
    CHECK_EQ(c.pair_count, KVM_CONFIG_MAX_PAIRED);
    uint8_t sha_x[KVM_CONFIG_TOKEN_SHA_LEN];
    memset(sha_x, 0xCC, sizeof sha_x);
    CHECK_EQ(kvm_config_pair_add(&c, "overflow", sha_x, 1, false), -1);

    // Removal keeps the order of the remaining clients.
    CHECK(kvm_config_pair_remove(&c, 0));
    CHECK_EQ(c.pair_count, KVM_CONFIG_MAX_PAIRED - 1u);
    CHECK_EQ(kvm_config_pair_find(&c, sha_a), -1);
    CHECK_EQ(kvm_config_pair_find(&c, sha_b), 0);
    CHECK(!kvm_config_pair_remove(&c, (uint8_t)c.pair_count));

    CHECK(kvm_config_pair_set_trusted(&c, 0, false));
    CHECK_EQ(c.pair[0].trusted, 0);
    CHECK(!kvm_config_pair_set_trusted(&c, 99, true));

    CHECK_EQ(kvm_config_pair_remove_all(&c), KVM_CONFIG_MAX_PAIRED - 1u);
    CHECK_EQ(c.pair_count, 0);
    CHECK_EQ(kvm_config_pair_find(&c, sha_b), -1);

    // Removal wipes the storage, not just the count: no digest is left behind.
    uint8_t acc = 0;
    const uint8_t *raw = (const uint8_t *)c.pair;
    for (size_t i = 0; i < sizeof c.pair; i++) {
        acc = (uint8_t)(acc | raw[i]);
    }
    CHECK_EQ(acc, 0);
}

static void test_pairing_validation(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    uint8_t sha[KVM_CONFIG_TOKEN_SHA_LEN];
    memset(sha, 0x5A, sizeof sha);
    CHECK_EQ(kvm_config_pair_add(&c, "good", sha, 1, true), 0);

    // A count beyond the array is clamped and the phantom entries are dropped.
    c.pair_count = (uint8_t)(KVM_CONFIG_MAX_PAIRED + 3u);
    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(rep.pairing_repaired);
    CHECK_EQ(c.pair_count, 1);
    CHECK_EQ(rep.pairs_dropped, KVM_CONFIG_MAX_PAIRED - 1u);

    // An unterminated name drops the entry: a half-readable client is not kept.
    kvm_config_defaults(&c);
    CHECK_EQ(kvm_config_pair_add(&c, "good", sha, 1, true), 0);
    memset(c.pair[0].name, 'x', sizeof c.pair[0].name);
    rep = kvm_config_validate(&c);
    CHECK(rep.pairing_repaired);
    CHECK_EQ(rep.pairs_dropped, 1);
    CHECK_EQ(c.pair_count, 0);

    // An all-zero digest is not a client either.
    kvm_config_defaults(&c);
    CHECK_EQ(kvm_config_pair_add(&c, "good", sha, 1, true), 0);
    memset(c.pair[0].token_sha, 0, sizeof c.pair[0].token_sha);
    rep = kvm_config_validate(&c);
    CHECK_EQ(c.pair_count, 0);
    CHECK_EQ(rep.pairs_dropped, 1);

    // An unknown trusted flag falls to "not trusted" (FR-733, the stricter side).
    kvm_config_defaults(&c);
    CHECK_EQ(kvm_config_pair_add(&c, "good", sha, 1, true), 0);
    c.pair[0].trusted = 7;
    rep = kvm_config_validate(&c);
    CHECK(rep.pairing_repaired);
    CHECK_EQ(c.pair_count, 1);
    CHECK_EQ(c.pair[0].trusted, 0);

    // A damaged middle entry is dropped and the rest keep working.
    kvm_config_defaults(&c);
    for (unsigned i = 0; i < 3u; i++) {
        uint8_t s[KVM_CONFIG_TOKEN_SHA_LEN];
        memset(s, (uint8_t)(0x20u + i), sizeof s);
        CHECK_EQ(kvm_config_pair_add(&c, "client", s, i, false), (int)i);
    }
    memset(c.pair[1].token_sha, 0, sizeof c.pair[1].token_sha);
    rep = kvm_config_validate(&c);
    CHECK_EQ(c.pair_count, 2);
    CHECK_EQ(rep.pairs_dropped, 1);
    CHECK_EQ(c.pair[0].token_sha[0], 0x20);
    CHECK_EQ(c.pair[1].token_sha[0], 0x22);
}

static void test_policy_floor(void)
{
    kvm_config_t strict;
    kvm_config_defaults(&strict);
    kvm_config_policy_rank_t floor = kvm_config_policy_rank(&strict);
    CHECK_EQ(floor.pair_require_accept, 1);
    CHECK_EQ(floor.control_approval, KVM_CONTROL_APPROVAL_ALWAYS);

    // A relaxed record is raised back to the floor and nothing else changes.
    kvm_config_t lax;
    kvm_config_defaults(&lax);
    CHECK(kvm_config_set(&lax, KVM_CFG_F_PAIR_REQUIRE_ACCEPT, 0));
    CHECK(kvm_config_set(&lax, KVM_CFG_F_CONTROL_APPROVAL, KVM_CONTROL_APPROVAL_AUTO));
    CHECK(kvm_config_set(&lax, KVM_CFG_F_BRIGHTNESS, 25));
    CHECK(kvm_config_policy_apply_floor(&lax, floor));
    CHECK_EQ(lax.pair_require_accept, 1);
    CHECK_EQ(lax.control_approval, KVM_CONTROL_APPROVAL_ALWAYS);
    CHECK_EQ(lax.brightness, 25);

    // A floor is never a ceiling: a stricter record is left alone.
    kvm_config_t keep;
    kvm_config_defaults(&keep);
    CHECK(kvm_config_set(&keep, KVM_CFG_F_CONFIRM_ALT_F4, 1));
    kvm_config_policy_rank_t low = {0, KVM_CONTROL_APPROVAL_AUTO, 0};
    CHECK(!kvm_config_policy_apply_floor(&keep, low));
    CHECK_EQ(keep.confirm_alt_f4, 1);
    CHECK_EQ(keep.control_approval, KVM_CONTROL_APPROVAL_ALWAYS);

    CHECK(!kvm_config_policy_apply_floor(NULL, floor));
}

static void test_crc32_known_vector(void)
{
    // CRC-32/ISO-HDLC check value.
    CHECK_EQ(kvm_config_crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(kvm_config_crc32("", 0), 0u);
    CHECK_EQ(kvm_config_crc32(NULL, 4), 0u);
}

static void test_serialize_roundtrip(void)
{
    kvm_config_t def;
    kvm_config_defaults(&def);
    CHECK_EQ(kvm_config_record_len(&def), HDR + KVM_CONFIG_PAYLOAD_FIXED_LEN);

    kvm_config_t full;
    fill_populated(&full);
    CHECK_EQ(kvm_config_record_len(&full), KVM_CONFIG_RECORD_MAX);

    const kvm_config_t *cases[] = {&def, &full};
    for (unsigned i = 0; i < 2u; i++) {
        uint8_t rec[KVM_CONFIG_RECORD_MAX];
        size_t n = kvm_config_serialize(cases[i], rec, sizeof rec);
        CHECK_EQ(n, kvm_config_record_len(cases[i]));
        CHECK_EQ(kvm_config_record_schema(rec, n), KVM_CONFIG_SCHEMA_VERSION);

        kvm_config_t back;
        kvm_config_repair_t rep;
        CHECK_EQ(kvm_config_deserialize(rec, n, &back, &rep), KVM_CONFIG_LOAD_OK);
        CHECK(!kvm_config_repair_any(&rep));
        CHECK(memcmp(&back, cases[i], sizeof back) == 0);
    }

    // A buffer one byte short is refused rather than half filled.
    uint8_t small[HDR + KVM_CONFIG_PAYLOAD_FIXED_LEN - 1u];
    CHECK_EQ(kvm_config_serialize(&def, small, sizeof small), 0u);
    CHECK_EQ(kvm_config_serialize(&def, NULL, 1000), 0u);
    CHECK_EQ(kvm_config_serialize(NULL, small, sizeof small), 0u);
}

// AT-FAULT-02: a single flipped bit anywhere in the record must end in the factory defaults.
static void test_every_single_bit_flip_recovers(void)
{
    kvm_config_t full;
    fill_populated(&full);
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    size_t n = kvm_config_serialize(&full, rec, sizeof rec);
    CHECK_EQ(n, KVM_CONFIG_RECORD_MAX);

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);

    unsigned flips = 0, detected = 0;
    for (size_t byte = 0; byte < n; byte++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            uint8_t broken[KVM_CONFIG_RECORD_MAX];
            memcpy(broken, rec, n);
            broken[byte] = (uint8_t)(broken[byte] ^ (uint8_t)(1u << bit));
            flips++;

            kvm_config_t out;
            kvm_config_load_status_t st = kvm_config_deserialize(broken, n, &out, NULL);
            if (st == KVM_CONFIG_LOAD_RECOVERED && memcmp(&out, &defaults, sizeof out) == 0) {
                detected++;
            }
        }
    }
    CHECK_EQ(flips, n * 8u);
    CHECK_EQ(detected, flips);
}

static void test_truncated_and_oversized(void)
{
    kvm_config_t full;
    fill_populated(&full);
    uint8_t rec[KVM_CONFIG_RECORD_MAX + 8u];
    memset(rec, 0, sizeof rec);
    size_t n = kvm_config_serialize(&full, rec, KVM_CONFIG_RECORD_MAX);

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    kvm_config_t out;

    // Every truncation, from one byte short down to nothing at all.
    for (size_t len = 0; len < n; len++) {
        CHECK_EQ(kvm_config_deserialize(rec, len, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
        CHECK(memcmp(&out, &defaults, sizeof out) == 0);
    }
    // Trailing bytes past the stated payload length.
    for (size_t extra = 1; extra <= 8u; extra++) {
        CHECK_EQ(kvm_config_deserialize(rec, n + extra, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
    }
    CHECK_EQ(kvm_config_deserialize(NULL, n, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
    CHECK_EQ(kvm_config_deserialize(rec, n, NULL, NULL), KVM_CONFIG_LOAD_RECOVERED);
    CHECK_EQ(kvm_config_record_schema(rec, 4), 0u);
    CHECK_EQ(kvm_config_record_schema(NULL, 100), 0u);
}

static void test_header_rejects(void)
{
    kvm_config_t def;
    kvm_config_defaults(&def);
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    size_t n = kvm_config_serialize(&def, rec, sizeof rec);
    kvm_config_t out;

    // Wrong magic, even with a correct CRC.
    uint8_t bad[KVM_CONFIG_RECORD_MAX];
    memcpy(bad, rec, n);
    bad[1] = 'X';
    restamp_crc(bad, n);
    CHECK_EQ(kvm_config_deserialize(bad, n, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);

    // Schema 0 is below the oldest migratable version.
    memcpy(bad, rec, n);
    put_u16le(bad + 8, 0);
    restamp_crc(bad, n);
    CHECK_EQ(kvm_config_deserialize(bad, n, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);

    // A stated payload length that does not match the buffer.
    memcpy(bad, rec, n);
    put_u16le(bad + 10, (uint16_t)(n - HDR + 4u));
    restamp_crc(bad, n);
    CHECK_EQ(kvm_config_deserialize(bad, n, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);

    // A payload shorter than the fixed part of schema 2.
    uint8_t stub[HDR + 4u];
    memset(stub, 0, sizeof stub);
    stamp(stub, 2, 4);
    CHECK_EQ(kvm_config_deserialize(stub, sizeof stub, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
}

// A record written by newer firmware: run on defaults, report it, and do not try to parse it.
static void test_future_schema(void)
{
    kvm_config_t full;
    fill_populated(&full);
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    size_t n = kvm_config_serialize(&full, rec, sizeof rec);

    put_u16le(rec + 8, (uint16_t)(KVM_CONFIG_SCHEMA_VERSION + 1u));
    restamp_crc(rec, n);

    kvm_config_t out;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(rec, n, &out, &rep), KVM_CONFIG_LOAD_FUTURE_SCHEMA);
    CHECK(!kvm_config_repair_any(&rep));

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(&out, &defaults, sizeof out) == 0);

    // A bad CRC outranks a future schema: a corrupt record is never treated as "newer".
    rec[HDR] = (uint8_t)(rec[HDR] ^ 0xFFu);
    CHECK_EQ(kvm_config_deserialize(rec, n, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
}

static void test_pair_count_mismatch(void)
{
    kvm_config_t full;
    fill_populated(&full);
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    size_t n = kvm_config_serialize(&full, rec, sizeof rec);
    kvm_config_t out;

    // The stated count does not match the stated payload length.
    uint8_t bad[KVM_CONFIG_RECORD_MAX];
    memcpy(bad, rec, n);
    bad[HDR + 40] = 7;
    restamp_crc(bad, n);
    CHECK_EQ(kvm_config_deserialize(bad, n, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);

    // A count past the register capacity.
    memcpy(bad, rec, n);
    bad[HDR + 40] = (uint8_t)(KVM_CONFIG_MAX_PAIRED + 1u);
    restamp_crc(bad, n);
    CHECK_EQ(kvm_config_deserialize(bad, n, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
}

// A record whose header is intact but whose values are out of range: parsed, then repaired.
static void test_repaired_record(void)
{
    kvm_config_t full;
    fill_populated(&full);
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    size_t n = kvm_config_serialize(&full, rec, sizeof rec);

    rec[HDR + 5] = 250;  // brightness, valid range 10..100
    rec[HDR + 14] = 200; // unicode_mode, 5 modes defined
    restamp_crc(rec, n);

    kvm_config_t out;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(rec, n, &out, &rep), KVM_CONFIG_LOAD_REPAIRED);
    CHECK_EQ(out.brightness, 80);
    CHECK_EQ(out.unicode_mode, KVM_UNICODE_MODE_HOST_IME);
    CHECK_EQ(rep.field_mask, ((uint32_t)1u << KVM_CFG_F_BRIGHTNESS) |
                                 ((uint32_t)1u << KVM_CFG_F_UNICODE_MODE));
    // Everything else survived.
    CHECK_EQ(out.scroll_speed, 10);
    CHECK_EQ(out.pair_count, KVM_CONFIG_MAX_PAIRED);
    CHECK(strcmp(out.host_layout, "de(neo)") == 0);
}

// Schema 1 is the frozen legacy layout: magic, CRC, schema, payload_len, then eight setting bytes.
// Written out by hand here so the migrator is tested against the old format, not against itself.
static size_t make_v1(uint8_t *rec, const uint8_t settings[8])
{
    memset(rec, 0, HDR + 8u);
    memcpy(rec + HDR, settings, 8);
    stamp(rec, 1, 8);
    return HDR + 8u;
}

static void test_migrate_v1_to_v2(void)
{
    const uint8_t settings[8] = {
        KVM_MOUSE_SENS_FAST, // mouse.sensitivity
        1,                   // mouse.natural_scroll
        0,                   // mouse.longpress_right_click
        0,                   // mouse.two_finger_scroll
        7,                   // mouse.scroll_speed
        45,                  // display.brightness
        1,                   // remote.enabled
        1,                   // diag.overlay
    };
    uint8_t rec[HDR + 8u];
    size_t n = make_v1(rec, settings);
    CHECK_EQ(kvm_config_record_schema(rec, n), 1u);

    kvm_config_t out;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(rec, n, &out, &rep), KVM_CONFIG_LOAD_MIGRATED);
    CHECK(!kvm_config_repair_any(&rep));

    // The eight legacy settings came across unchanged.
    CHECK_EQ(out.mouse_sensitivity, KVM_MOUSE_SENS_FAST);
    CHECK_EQ(out.natural_scroll, 1);
    CHECK_EQ(out.longpress_right_click, 0);
    CHECK_EQ(out.two_finger_scroll, 0);
    CHECK_EQ(out.scroll_speed, 7);
    CHECK_EQ(out.brightness, 45);
    CHECK_EQ(out.remote_enabled, 1);
    CHECK_EQ(out.diag_overlay, 1);

    // FR-736: the fields schema 1 never had arrive at the strict defaults, not at zero.
    CHECK_EQ(out.pair_require_accept, 1);
    CHECK_EQ(out.control_approval, KVM_CONTROL_APPROVAL_ALWAYS);
    CHECK_EQ(out.confirm_alt_f4, 0);
    CHECK_EQ(out.keepawake_enabled, 0);
    CHECK_EQ(out.keepawake_interval_s, 30);
    CHECK_EQ(out.unicode_mode, KVM_UNICODE_MODE_HOST_IME);
    CHECK(strcmp(out.host_layout, "us") == 0);
    CHECK_EQ(out.wifi.valid, 0);
    CHECK_EQ(out.pair_count, 0);

    // The migrated record serializes as a current-schema record.
    uint8_t again[KVM_CONFIG_RECORD_MAX];
    size_t m = kvm_config_serialize(&out, again, sizeof again);
    CHECK_EQ(kvm_config_record_schema(again, m), KVM_CONFIG_SCHEMA_VERSION);
    kvm_config_t reread;
    CHECK_EQ(kvm_config_deserialize(again, m, &reread, NULL), KVM_CONFIG_LOAD_OK);
    CHECK(memcmp(&reread, &out, sizeof reread) == 0);
}

static void test_migrate_v1_repairs_and_rejects(void)
{
    // Out-of-range legacy values fall back to their defaults during the migration.
    const uint8_t junk[8] = {9, 3, 4, 5, 0, 200, 2, 2};
    uint8_t rec[HDR + 8u];
    size_t n = make_v1(rec, junk);

    kvm_config_t out;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(rec, n, &out, &rep), KVM_CONFIG_LOAD_MIGRATED);
    CHECK(kvm_config_repair_any(&rep));
    CHECK_EQ(out.mouse_sensitivity, KVM_MOUSE_SENS_NORMAL);
    CHECK_EQ(out.natural_scroll, 0);
    CHECK_EQ(out.scroll_speed, 3);
    CHECK_EQ(out.brightness, 80);
    CHECK_EQ(out.remote_enabled, 0);
    CHECK_EQ(out.control_approval, KVM_CONTROL_APPROVAL_ALWAYS);

    // A schema-1 record whose payload is not exactly eight bytes is not a schema-1 record.
    uint8_t wrong[HDR + 12u];
    memset(wrong, 0, sizeof wrong);
    stamp(wrong, 1, 12);
    CHECK_EQ(kvm_config_deserialize(wrong, sizeof wrong, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
}

/**
 * ADR-037 bumped the schema from 2 to 3 by appending the three language bytes, which moved the start of the
 * pairing entries by three bytes. A schema-2 record is built here from the frozen V2_FIXED length and the
 * pairing entries of a current record, so the migrator is tested against the old layout and not against
 * itself. Schema 2 is also the oldest schema that still carries the two ADR-039 lists forward by default.
 */
static void test_migrate_v2_to_v3(void)
{
    kvm_config_t full;
    fill_populated(&full); // all three languages on, eight paired clients
    CHECK_EQ(full.lang_ko, 1);
    uint8_t cur[KVM_CONFIG_RECORD_MAX];
    const size_t n = kvm_config_serialize(&full, cur, sizeof cur);
    CHECK_EQ(n, KVM_CONFIG_RECORD_MAX);

    // the schema-2 payload: the first V2_FIXED settings bytes, then the pairing entries unchanged
    uint8_t old[KVM_CONFIG_RECORD_MAX];
    memcpy(old, cur, HDR + V2_FIXED);
    memcpy(old + HDR + V2_FIXED, cur + HDR + KVM_CONFIG_PAYLOAD_FIXED_LEN,
           n - HDR - KVM_CONFIG_PAYLOAD_FIXED_LEN);
    const size_t m = n - (KVM_CONFIG_PAYLOAD_FIXED_LEN - V2_FIXED);
    stamp(old, 2, (uint16_t)(m - HDR));
    CHECK_EQ(kvm_config_record_schema(old, m), 2u);

    kvm_config_t out;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(old, m, &out, &rep), KVM_CONFIG_LOAD_MIGRATED);
    CHECK(!kvm_config_repair_any(&rep));

    // the three languages arrive at their default (off), everything else exactly as it was written
    CHECK_EQ(out.lang_ko, 0);
    CHECK_EQ(out.lang_zh, 0);
    CHECK_EQ(out.lang_ja, 0);
    CHECK_EQ(out.unicode_mode, KVM_UNICODE_MODE_WINCOMPOSE);
    CHECK(strcmp(out.host_layout, "de(neo)") == 0);
    CHECK_EQ(out.wifi.valid, 1);
    CHECK_EQ(out.wifi.channel, 11);
    // the pairing entries were read three bytes earlier and are intact
    CHECK_EQ(out.pair_count, KVM_CONFIG_MAX_PAIRED);
    for (unsigned i = 0; i < KVM_CONFIG_MAX_PAIRED; i++) {
        CHECK(memcmp(&out.pair[i], &full.pair[i], sizeof out.pair[i]) == 0);
    }
    // ... and the migrated record is byte-for-byte the schema-3 one with the languages off
    kvm_config_t expect = full;
    CHECK(kvm_config_set(&expect, KVM_CFG_F_LANG_KO, 0));
    CHECK(kvm_config_set(&expect, KVM_CFG_F_LANG_ZH, 0));
    CHECK(kvm_config_set(&expect, KVM_CFG_F_LANG_JA, 0));
    CHECK(memcmp(&out, &expect, sizeof out) == 0);

    // it is rewritten as a schema-3 record that reads back unchanged
    uint8_t again[KVM_CONFIG_RECORD_MAX];
    const size_t k = kvm_config_serialize(&out, again, sizeof again);
    CHECK_EQ(k, n);
    CHECK_EQ(kvm_config_record_schema(again, k), KVM_CONFIG_SCHEMA_VERSION);
    kvm_config_t reread;
    CHECK_EQ(kvm_config_deserialize(again, k, &reread, NULL), KVM_CONFIG_LOAD_OK);
    CHECK(memcmp(&reread, &out, sizeof reread) == 0);

    // a schema-2 record whose length does not match its own pairing count is refused, as for schema 3
    uint8_t bad[KVM_CONFIG_RECORD_MAX];
    memcpy(bad, old, m);
    stamp(bad, 2, (uint16_t)(m - HDR - 1u));
    CHECK_EQ(kvm_config_deserialize(bad, m - 1u, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
}

/** The three ADR-037 fields are plain 0/1 settings: out of range falls back to off, and they round trip. */
static void test_language_fields(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    static const kvm_config_field_t ids[3] = {KVM_CFG_F_LANG_KO, KVM_CFG_F_LANG_ZH, KVM_CFG_F_LANG_JA};
    static const char *const keys[3] = {"kbd.lang_ko", "kbd.lang_zh", "kbd.lang_ja"};
    for (unsigned i = 0; i < 3u; i++) {
        uint32_t min = 9, max = 9, def = 9;
        CHECK(kvm_config_field_range(ids[i], &min, &max, &def));
        CHECK_EQ(min, 0u);
        CHECK_EQ(max, 1u);
        CHECK_EQ(def, 0u);
        CHECK(strcmp(kvm_config_field_key(ids[i]), keys[i]) == 0);
        CHECK(kvm_config_set(&c, ids[i], 1));
        CHECK_EQ(kvm_config_get(&c, ids[i]), 1u);
        CHECK(!kvm_config_set(&c, ids[i], 2)); // out of range is refused, not clamped
        CHECK_EQ(kvm_config_get(&c, ids[i]), 1u);
    }

    // one language on: each byte carries its own value through a round trip
    kvm_config_defaults(&c);
    CHECK(kvm_config_set(&c, KVM_CFG_F_LANG_ZH, 1));
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    const size_t n = kvm_config_serialize(&c, rec, sizeof rec);
    CHECK_EQ(n, HDR + KVM_CONFIG_PAYLOAD_FIXED_LEN);
    kvm_config_t back;
    CHECK_EQ(kvm_config_deserialize(rec, n, &back, NULL), KVM_CONFIG_LOAD_OK);
    CHECK_EQ(back.lang_ko, 0);
    CHECK_EQ(back.lang_zh, 1);
    CHECK_EQ(back.lang_ja, 0);

    // a stored byte outside 0/1 is repaired to the default (FR-736)
    rec[HDR + 42u] = 0x5Au; // lang_zh
    restamp_crc(rec, n);
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(rec, n, &back, &rep), KVM_CONFIG_LOAD_REPAIRED);
    CHECK(kvm_config_repair_any(&rep));
    CHECK((rep.field_mask & ((uint32_t)1u << KVM_CFG_F_LANG_ZH)) != 0u);
    CHECK_EQ(back.lang_zh, 0);
}

// ---------------------------------------------------------------------------------------------
// ADR-039 HOST-layout favorites and recents
// ---------------------------------------------------------------------------------------------

static const char k_fav8[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP] = {
    "us", "de(neo)", "fr(oss)", "pl", "gb", "es", "it", "cz(qwerty)"};
static const char k_recent5[KVM_CONFIG_HOST_RECENT_MAX][KVM_CONFIG_LAYOUT_ID_CAP] = {
    "jp", "kr", "ru(phonetic)", "se", "no"};

// Every slot past the count must be all zero, so a validated record is byte-for-byte canonical.
static void check_tail_zero(const char (*slots)[KVM_CONFIG_LAYOUT_ID_CAP], uint8_t count,
                            uint8_t capacity)
{
    for (uint8_t i = count; i < capacity; i++) {
        for (unsigned k = 0; k < KVM_CONFIG_LAYOUT_ID_CAP; k++) {
            CHECK_EQ(slots[i][k], 0);
        }
    }
}

/** The shipped record offers exactly one favorite, the shipped layout, and no recents yet. */
static void test_host_lists_defaults(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    CHECK_EQ(c.host_fav_count, 1);
    CHECK(strcmp(c.host_fav[0], "us") == 0);
    CHECK_EQ(c.host_recent_count, 0);
    check_tail_zero(c.host_fav, 1u, (uint8_t)KVM_CONFIG_HOST_FAV_MAX);
    check_tail_zero(c.host_recent, 0u, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX);

    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(!kvm_config_repair_any(&rep));
    CHECK(!rep.host_fav_repaired);
    CHECK(!rep.host_recent_repaired);
    CHECK_EQ(rep.host_ids_dropped, 0);
}

/** The setters validate every id and the count, and change nothing at all when one of them fails. */
static void test_host_list_setters(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);

    CHECK(kvm_config_set_favorites(&c, k_fav8, (uint8_t)KVM_CONFIG_HOST_FAV_MAX));
    CHECK(kvm_config_set_recents(&c, k_recent5, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX));
    CHECK_EQ(c.host_fav_count, KVM_CONFIG_HOST_FAV_MAX);
    CHECK_EQ(c.host_recent_count, KVM_CONFIG_HOST_RECENT_MAX);
    for (unsigned i = 0; i < KVM_CONFIG_HOST_FAV_MAX; i++) {
        CHECK(strcmp(c.host_fav[i], k_fav8[i]) == 0);
    }
    for (unsigned i = 0; i < KVM_CONFIG_HOST_RECENT_MAX; i++) {
        CHECK(strcmp(c.host_recent[i], k_recent5[i]) == 0);
    }
    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(!kvm_config_repair_any(&rep));

    // a count past the capacity is refused and the stored list is untouched
    kvm_config_t before = c;
    CHECK(!kvm_config_set_favorites(&c, k_fav8, (uint8_t)KVM_CONFIG_HOST_FAV_MAX + 1u));
    CHECK(!kvm_config_set_recents(&c, k_recent5, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX + 1u));
    CHECK(memcmp(&before, &c, sizeof c) == 0);

    // one unusable id refuses the whole list: no half-written list, not even its valid head
    char bad[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP];
    memset(bad, 0, sizeof bad);
    snprintf(bad[0], KVM_CONFIG_LAYOUT_ID_CAP, "us");
    snprintf(bad[1], KVM_CONFIG_LAYOUT_ID_CAP, "DE");   // uppercase is not an id
    snprintf(bad[2], KVM_CONFIG_LAYOUT_ID_CAP, "fr");
    CHECK(!kvm_config_set_favorites(&c, bad, 3u));
    CHECK(!kvm_config_set_recents(&c, bad, 3u));
    snprintf(bad[1], KVM_CONFIG_LAYOUT_ID_CAP, "de()"); // an empty variant is not an id
    CHECK(!kvm_config_set_favorites(&c, bad, 3u));
    memset(bad[1], 'a', KVM_CONFIG_LAYOUT_ID_CAP);      // no terminator inside the slot
    CHECK(!kvm_config_set_favorites(&c, bad, 3u));
    CHECK(memcmp(&before, &c, sizeof c) == 0);

    // the recents may be emptied; the favorites always offer at least one layout
    CHECK(kvm_config_set_recents(&c, NULL, 0u));
    CHECK_EQ(c.host_recent_count, 0);
    check_tail_zero(c.host_recent, 0u, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX);
    CHECK(!kvm_config_set_favorites(&c, NULL, 0u));
    CHECK_EQ(c.host_fav_count, KVM_CONFIG_HOST_FAV_MAX);

    // a shorter list clears the slots it no longer uses
    CHECK(kvm_config_set_favorites(&c, k_fav8, 2u));
    CHECK_EQ(c.host_fav_count, 2);
    check_tail_zero(c.host_fav, 2u, (uint8_t)KVM_CONFIG_HOST_FAV_MAX);

    CHECK(!kvm_config_set_favorites(NULL, k_fav8, 1u));
    CHECK(!kvm_config_set_recents(NULL, k_recent5, 1u));
}

/** Both lists at their maximum survive a serialize / deserialize round trip unchanged. */
static void test_host_lists_roundtrip(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    CHECK(kvm_config_set_favorites(&c, k_fav8, (uint8_t)KVM_CONFIG_HOST_FAV_MAX));
    CHECK(kvm_config_set_recents(&c, k_recent5, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX));

    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    const size_t n = kvm_config_serialize(&c, rec, sizeof rec);
    CHECK_EQ(n, HDR + KVM_CONFIG_PAYLOAD_FIXED_LEN); // no paired clients: the fixed part alone
    CHECK_EQ(kvm_config_record_schema(rec, n), 4u);
    // the lists are where the schema-4 table says they are
    CHECK_EQ(rec[HDR + FAV_COUNT_OFF], KVM_CONFIG_HOST_FAV_MAX);
    CHECK_EQ(rec[HDR + RECENT_COUNT_OFF], KVM_CONFIG_HOST_RECENT_MAX);
    CHECK(memcmp(rec + HDR + FAV_OFF, "us", 3) == 0);
    CHECK(memcmp(rec + HDR + RECENT_OFF, "jp", 3) == 0);

    kvm_config_t back;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(rec, n, &back, &rep), KVM_CONFIG_LOAD_OK);
    CHECK(!kvm_config_repair_any(&rep));
    CHECK(memcmp(&back, &c, sizeof back) == 0);
}

/** A stored list with a corrupt entry loads: the entry is dropped, the order of the rest is kept. */
static void test_host_lists_repaired(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    CHECK(kvm_config_set_favorites(&c, k_fav8, 4u));          // us, de(neo), fr(oss), pl
    CHECK(kvm_config_set_recents(&c, k_recent5, 3u));         // jp, kr, ru(phonetic)
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    const size_t n = kvm_config_serialize(&c, rec, sizeof rec);

    memset(rec + HDR + FAV_OFF + KVM_CONFIG_LAYOUT_ID_CAP, 'a', KVM_CONFIG_LAYOUT_ID_CAP); // de(neo)
    rec[HDR + RECENT_OFF + KVM_CONFIG_LAYOUT_ID_CAP] = '\0';                               // kr
    restamp_crc(rec, n);

    kvm_config_t out;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(rec, n, &out, &rep), KVM_CONFIG_LOAD_REPAIRED);
    CHECK(rep.host_fav_repaired);
    CHECK(rep.host_recent_repaired);
    CHECK_EQ(rep.host_ids_dropped, 2);
    CHECK_EQ(rep.field_mask, 0u);

    CHECK_EQ(out.host_fav_count, 3);
    CHECK(strcmp(out.host_fav[0], "us") == 0);
    CHECK(strcmp(out.host_fav[1], "fr(oss)") == 0);
    CHECK(strcmp(out.host_fav[2], "pl") == 0);
    check_tail_zero(out.host_fav, 3u, (uint8_t)KVM_CONFIG_HOST_FAV_MAX);
    CHECK_EQ(out.host_recent_count, 2);
    CHECK(strcmp(out.host_recent[0], "jp") == 0);
    CHECK(strcmp(out.host_recent[1], "ru(phonetic)") == 0);
    check_tail_zero(out.host_recent, 2u, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX);

    // a count past the capacity is clamped and the slots it claimed are dropped
    uint8_t over[KVM_CONFIG_RECORD_MAX];
    memcpy(over, rec, n);
    memcpy(over + HDR + FAV_OFF + KVM_CONFIG_LAYOUT_ID_CAP, "de(neo)", 8);
    over[HDR + FAV_COUNT_OFF] = 200u;
    over[HDR + RECENT_COUNT_OFF] = (uint8_t)(KVM_CONFIG_HOST_RECENT_MAX + 1u);
    restamp_crc(over, n);
    CHECK_EQ(kvm_config_deserialize(over, n, &out, &rep), KVM_CONFIG_LOAD_REPAIRED);
    CHECK(rep.host_fav_repaired);
    CHECK_EQ(out.host_fav_count, 4); // the four written ids, the empty claimed slots dropped
    CHECK(strcmp(out.host_fav[1], "de(neo)") == 0);
    CHECK_EQ(out.host_recent_count, 2);

    // every entry unusable: the favorites fall back to the shipped layout, the load still succeeds
    uint8_t wiped[KVM_CONFIG_RECORD_MAX];
    memcpy(wiped, rec, n);
    memset(wiped + HDR + FAV_OFF, 'a', (size_t)KVM_CONFIG_HOST_FAV_MAX * KVM_CONFIG_LAYOUT_ID_CAP);
    restamp_crc(wiped, n);
    CHECK_EQ(kvm_config_deserialize(wiped, n, &out, &rep), KVM_CONFIG_LOAD_REPAIRED);
    CHECK(rep.host_fav_repaired);
    CHECK_EQ(out.host_fav_count, 1);
    CHECK(strcmp(out.host_fav[0], "us") == 0);
    check_tail_zero(out.host_fav, 1u, (uint8_t)KVM_CONFIG_HOST_FAV_MAX);

    // the same through a count of zero rather than through corrupt entries
    uint8_t empty[KVM_CONFIG_RECORD_MAX];
    memcpy(empty, rec, n);
    empty[HDR + FAV_COUNT_OFF] = 0u;
    restamp_crc(empty, n);
    CHECK_EQ(kvm_config_deserialize(empty, n, &out, &rep), KVM_CONFIG_LOAD_REPAIRED);
    CHECK(rep.host_fav_repaired);
    CHECK_EQ(rep.host_ids_dropped, 1); // the corrupt recent, the favorites were simply refilled
    CHECK_EQ(out.host_fav_count, 1);
    CHECK(strcmp(out.host_fav[0], "us") == 0);
}

/**
 * An id the layout tables no longer carry is still a well-formed id: the record keeps it and stays
 * valid. Falling back to `us` for an entry the picker cannot resolve is the picker's decision
 * (ADR-039), so the store must not quietly rewrite the list behind the user's back.
 */
static void test_host_list_keeps_an_unresolvable_id(void)
{
    char ids[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP];
    memset(ids, 0, sizeof ids);
    snprintf(ids[0], KVM_CONFIG_LAYOUT_ID_CAP, "us");
    snprintf(ids[1], KVM_CONFIG_LAYOUT_ID_CAP, "zz(nope)"); // well formed, in no table
    snprintf(ids[2], KVM_CONFIG_LAYOUT_ID_CAP, "x1_y-2");   // the whole permitted character set

    kvm_config_t c;
    kvm_config_defaults(&c);
    CHECK(kvm_config_set_favorites(&c, ids, 3u));
    CHECK(kvm_config_set_recents(&c, ids, 3u));
    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(!kvm_config_repair_any(&rep));

    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    const size_t n = kvm_config_serialize(&c, rec, sizeof rec);
    kvm_config_t back;
    CHECK_EQ(kvm_config_deserialize(rec, n, &back, &rep), KVM_CONFIG_LOAD_OK);
    CHECK(!kvm_config_repair_any(&rep));
    CHECK_EQ(back.host_fav_count, 3);
    CHECK(strcmp(back.host_fav[1], "zz(nope)") == 0);
    CHECK(strcmp(back.host_recent[2], "x1_y-2") == 0);
}

/**
 * ADR-039 bumped the schema from 3 to 4 by appending the two HOST-layout lists to the fixed part,
 * which moved the start of the pairing entries by 210 bytes. A schema-3 record is built here from
 * the frozen V3_FIXED length and the pairing entries of a current record, so the migrator is tested
 * against the old layout and not against itself.
 */
static void test_migrate_v3_to_v4(void)
{
    kvm_config_t full;
    fill_populated(&full); // eight paired clients, a Wi-Fi cache, every setting away from default
    CHECK(kvm_config_set_favorites(&full, k_fav8, 6u));
    CHECK(kvm_config_set_recents(&full, k_recent5, 4u));
    uint8_t cur[KVM_CONFIG_RECORD_MAX];
    const size_t n = kvm_config_serialize(&full, cur, sizeof cur);
    CHECK_EQ(n, KVM_CONFIG_RECORD_MAX);

    // the schema-3 payload: the first V3_FIXED settings bytes, then the pairing entries unchanged
    uint8_t old[KVM_CONFIG_RECORD_MAX];
    memcpy(old, cur, HDR + V3_FIXED);
    memcpy(old + HDR + V3_FIXED, cur + HDR + KVM_CONFIG_PAYLOAD_FIXED_LEN,
           n - HDR - KVM_CONFIG_PAYLOAD_FIXED_LEN);
    const size_t m = n - (KVM_CONFIG_PAYLOAD_FIXED_LEN - V3_FIXED);
    stamp(old, 3, (uint16_t)(m - HDR));
    CHECK_EQ(kvm_config_record_schema(old, m), 3u);

    kvm_config_t out;
    kvm_config_repair_t rep;
    CHECK_EQ(kvm_config_deserialize(old, m, &out, &rep), KVM_CONFIG_LOAD_MIGRATED);
    CHECK(!kvm_config_repair_any(&rep));

    // the two lists arrive at their defaults
    CHECK_EQ(out.host_fav_count, 1);
    CHECK(strcmp(out.host_fav[0], "us") == 0);
    CHECK_EQ(out.host_recent_count, 0);
    check_tail_zero(out.host_fav, 1u, (uint8_t)KVM_CONFIG_HOST_FAV_MAX);
    check_tail_zero(out.host_recent, 0u, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX);

    // every schema-3 value survived, the pairing entries read 210 bytes earlier among them
    CHECK_EQ(out.mouse_sensitivity, KVM_MOUSE_SENS_FAST);
    CHECK_EQ(out.scroll_speed, 10);
    CHECK_EQ(out.brightness, 10);
    CHECK_EQ(out.keepawake_interval_s, 300);
    CHECK_EQ(out.unicode_mode, KVM_UNICODE_MODE_WINCOMPOSE);
    CHECK_EQ(out.lang_ko, 1);
    CHECK_EQ(out.lang_zh, 1);
    CHECK_EQ(out.lang_ja, 1);
    CHECK(strcmp(out.host_layout, "de(neo)") == 0);
    CHECK_EQ(out.wifi.valid, 1);
    CHECK_EQ(out.wifi.channel, 11);
    CHECK_EQ(out.pair_count, KVM_CONFIG_MAX_PAIRED);
    for (unsigned i = 0; i < KVM_CONFIG_MAX_PAIRED; i++) {
        CHECK(memcmp(&out.pair[i], &full.pair[i], sizeof out.pair[i]) == 0);
    }
    // ... and the record is byte for byte the schema-4 one with the two lists at their defaults
    kvm_config_t expect = full;
    kvm_config_t fresh;
    kvm_config_defaults(&fresh);
    memcpy(expect.host_fav, fresh.host_fav, sizeof expect.host_fav);
    expect.host_fav_count = fresh.host_fav_count;
    memcpy(expect.host_recent, fresh.host_recent, sizeof expect.host_recent);
    expect.host_recent_count = fresh.host_recent_count;
    CHECK(memcmp(&out, &expect, sizeof out) == 0);

    // it is rewritten as a schema-4 record that reads back unchanged
    uint8_t again[KVM_CONFIG_RECORD_MAX];
    const size_t k = kvm_config_serialize(&out, again, sizeof again);
    CHECK_EQ(k, n);
    CHECK_EQ(kvm_config_record_schema(again, k), KVM_CONFIG_SCHEMA_VERSION);
    kvm_config_t reread;
    CHECK_EQ(kvm_config_deserialize(again, k, &reread, NULL), KVM_CONFIG_LOAD_OK);
    CHECK(memcmp(&reread, &out, sizeof reread) == 0);

    // a schema-3 record whose length does not match its own pairing count is refused
    uint8_t bad[KVM_CONFIG_RECORD_MAX];
    memcpy(bad, old, m);
    stamp(bad, 3, (uint16_t)(m - HDR - 1u));
    CHECK_EQ(kvm_config_deserialize(bad, m - 1u, &out, NULL), KVM_CONFIG_LOAD_RECOVERED);
}

static void test_status_strings(void)
{
    CHECK(strcmp(kvm_config_load_status_str(KVM_CONFIG_LOAD_OK), "ok") == 0);
    CHECK(strcmp(kvm_config_load_status_str(KVM_CONFIG_LOAD_RECOVERED), "recovered") == 0);
    CHECK(strcmp(kvm_config_load_status_str(KVM_CONFIG_LOAD_FUTURE_SCHEMA), "future-schema") == 0);
    CHECK(strcmp(kvm_config_load_status_str((kvm_config_load_status_t)99), "unknown") == 0);
}

static void test_null_tolerance(void)
{
    kvm_config_defaults(NULL);
    kvm_config_repair_t rep = kvm_config_validate(NULL);
    CHECK(!kvm_config_repair_any(&rep));
    CHECK(!kvm_config_repair_any(NULL));
    CHECK_EQ(kvm_config_get(NULL, KVM_CFG_F_BRIGHTNESS), 0u);
    CHECK(!kvm_config_set(NULL, KVM_CFG_F_BRIGHTNESS, 50));
    kvm_config_set_unchecked(NULL, KVM_CFG_F_BRIGHTNESS, 50);
    CHECK(!kvm_config_set_layout_id(NULL, "us"));
    CHECK_EQ(kvm_config_pair_find(NULL, NULL), -1);
    CHECK_EQ(kvm_config_pair_add(NULL, "x", NULL, 0, false), -1);
    CHECK(!kvm_config_pair_remove(NULL, 0));
    CHECK_EQ(kvm_config_pair_remove_all(NULL), 0);
    CHECK(!kvm_config_pair_set_trusted(NULL, 0, true));
    CHECK_EQ(kvm_config_record_len(NULL), HDR + KVM_CONFIG_PAYLOAD_FIXED_LEN);

    kvm_config_policy_rank_t r = kvm_config_policy_rank(NULL);
    CHECK_EQ(r.control_approval, 0);
}

/**
 * ADR-039 section 3: the star pins and unpins, the order of the rest survives an unpin, a ninth favorite is
 * refused rather than pushing one out, and the last favorite cannot be unpinned - the list always offers
 * something.
 */
static void test_pin_toggle(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    CHECK_EQ(c.host_fav_count, 1); // "us"

    CHECK(kvm_config_pin_toggle(&c, "de"));
    CHECK(kvm_config_pin_toggle(&c, "fr(oss)"));
    CHECK_EQ(c.host_fav_count, 3);
    CHECK(strcmp(c.host_fav[0], "us") == 0);
    CHECK(strcmp(c.host_fav[1], "de") == 0);
    CHECK(strcmp(c.host_fav[2], "fr(oss)") == 0); // appended, in the order they were pinned

    // Pinning the same id again unpins it, and the two around it keep their places.
    CHECK(kvm_config_pin_toggle(&c, "de"));
    CHECK_EQ(c.host_fav_count, 2);
    CHECK(strcmp(c.host_fav[0], "us") == 0);
    CHECK(strcmp(c.host_fav[1], "fr(oss)") == 0);
    check_tail_zero(c.host_fav, 2u, (uint8_t)KVM_CONFIG_HOST_FAV_MAX);

    // Fill the eight slots, then refuse the ninth and change nothing.
    CHECK(kvm_config_set_favorites(&c, k_fav8, (uint8_t)KVM_CONFIG_HOST_FAV_MAX));
    kvm_config_t before = c;
    CHECK(!kvm_config_pin_toggle(&c, "jp"));
    CHECK(memcmp(&before, &c, sizeof c) == 0);
    // ... while unpinning one of the eight still works, and then the ninth fits.
    CHECK(kvm_config_pin_toggle(&c, "pl"));
    CHECK_EQ(c.host_fav_count, KVM_CONFIG_HOST_FAV_MAX - 1u);
    CHECK(kvm_config_pin_toggle(&c, "jp"));
    CHECK(strcmp(c.host_fav[KVM_CONFIG_HOST_FAV_MAX - 1u], "jp") == 0);

    // The last one stays: an empty list would leave the picker with nothing to offer first.
    char one[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP];
    memset(one, 0, sizeof one);
    snprintf(one[0], KVM_CONFIG_LAYOUT_ID_CAP, "us");
    CHECK(kvm_config_set_favorites(&c, one, 1u));
    before = c;
    CHECK(!kvm_config_pin_toggle(&c, "us"));
    CHECK(memcmp(&before, &c, sizeof c) == 0);

    // A malformed id is refused, record untouched, and so is a NULL one.
    CHECK(!kvm_config_pin_toggle(&c, "DE"));
    CHECK(!kvm_config_pin_toggle(&c, ""));
    CHECK(!kvm_config_pin_toggle(&c, NULL));
    CHECK(!kvm_config_pin_toggle(NULL, "de"));
    CHECK(memcmp(&before, &c, sizeof c) == 0);
}

/** ADR-039 section 4: the recents are newest first, hold one entry per layout, and are five deep. */
static void test_recent_push(void)
{
    kvm_config_t c;
    kvm_config_defaults(&c);
    CHECK_EQ(c.host_recent_count, 0);

    CHECK(kvm_config_recent_push(&c, "us"));
    CHECK(kvm_config_recent_push(&c, "de"));
    CHECK(kvm_config_recent_push(&c, "fr"));
    CHECK_EQ(c.host_recent_count, 3);
    CHECK(strcmp(c.host_recent[0], "fr") == 0); // newest first
    CHECK(strcmp(c.host_recent[1], "de") == 0);
    CHECK(strcmp(c.host_recent[2], "us") == 0);

    // Using one again moves it to the head instead of appearing twice.
    CHECK(kvm_config_recent_push(&c, "us"));
    CHECK_EQ(c.host_recent_count, 3);
    CHECK(strcmp(c.host_recent[0], "us") == 0);
    CHECK(strcmp(c.host_recent[1], "fr") == 0);
    CHECK(strcmp(c.host_recent[2], "de") == 0);

    // The sixth one drops the oldest.
    CHECK(kvm_config_recent_push(&c, "pl"));
    CHECK(kvm_config_recent_push(&c, "es"));
    CHECK_EQ(c.host_recent_count, KVM_CONFIG_HOST_RECENT_MAX);
    CHECK(kvm_config_recent_push(&c, "it"));
    CHECK_EQ(c.host_recent_count, KVM_CONFIG_HOST_RECENT_MAX);
    CHECK(strcmp(c.host_recent[0], "it") == 0);
    CHECK(strcmp(c.host_recent[KVM_CONFIG_HOST_RECENT_MAX - 1u], "fr") == 0); // "de" fell off
    for (unsigned i = 0; i < c.host_recent_count; i++) {
        CHECK(strcmp(c.host_recent[i], "de") != 0);
    }
    kvm_config_repair_t rep = kvm_config_validate(&c);
    CHECK(!kvm_config_repair_any(&rep));

    // An id the tables no longer have is still a valid id: it is kept, and it is the UI that skips it.
    CHECK(kvm_config_recent_push(&c, "zz(nope)"));
    CHECK(strcmp(c.host_recent[0], "zz(nope)") == 0);
    rep = kvm_config_validate(&c);
    CHECK(!kvm_config_repair_any(&rep));

    kvm_config_t before = c;
    CHECK(!kvm_config_recent_push(&c, "Nope"));
    CHECK(!kvm_config_recent_push(&c, NULL));
    CHECK(!kvm_config_recent_push(NULL, "us"));
    CHECK(memcmp(&before, &c, sizeof c) == 0);
}

int main(void)
{
    printf("config_store record tests\n");
    RUN(test_defaults);
    RUN(test_field_table);
    RUN(test_every_field_out_of_range_falls_back);
    RUN(test_all_fields_corrupt_at_once);
    RUN(test_layout_ids);
    RUN(test_wifi_cache_validation);
    RUN(test_pairing_register);
    RUN(test_pairing_validation);
    RUN(test_policy_floor);
    RUN(test_crc32_known_vector);
    RUN(test_serialize_roundtrip);
    RUN(test_every_single_bit_flip_recovers);
    RUN(test_truncated_and_oversized);
    RUN(test_header_rejects);
    RUN(test_future_schema);
    RUN(test_pair_count_mismatch);
    RUN(test_repaired_record);
    RUN(test_migrate_v1_to_v2);
    RUN(test_migrate_v1_repairs_and_rejects);
    RUN(test_migrate_v2_to_v3);
    RUN(test_language_fields);
    RUN(test_host_lists_defaults);
    RUN(test_host_list_setters);
    RUN(test_host_lists_roundtrip);
    RUN(test_host_lists_repaired);
    RUN(test_host_list_keeps_an_unresolvable_id);
    RUN(test_pin_toggle);
    RUN(test_recent_push);
    RUN(test_migrate_v3_to_v4);
    RUN(test_status_strings);
    RUN(test_null_tolerance);
    TEST_MAIN_END("config_record");
}
