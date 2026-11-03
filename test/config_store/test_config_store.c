// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the settings store (TASK-090) against the in-memory backend in fake_nvs.h: load
// and recovery paths, the write debounce on a fake clock, backend faults (failed write, power loss
// mid-write, a torn record), the pairing register and the factory reset.
//
// Covers AT-FAULT-02 (invalid checksum -> safe defaults, no crash), AT-FAULT-03 (a settings write
// never lands on the per-tap path, so it cannot stall a frame), AT-FAULT-10 (factory reset removes
// every paired client) and AT-SEC-08 (no token ever reaches storage).

#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "fake_nvs.h"
#include "kvm_test.h"

#define QUIET_MS 1500u
#define DEFER_MS 10000u
#define INTERVAL_MS 2000u

static void seed_from_config(fake_nvs_t *f, const kvm_config_t *cfg)
{
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    size_t n = kvm_config_serialize(cfg, rec, sizeof rec);
    CHECK(n > 0u);
    fake_nvs_seed(f, rec, n);
}

// Rewrite the schema of a stored record and fix its CRC, as if newer firmware had written it.
static void seed_with_schema(fake_nvs_t *f, const kvm_config_t *cfg, uint16_t schema)
{
    uint8_t rec[KVM_CONFIG_RECORD_MAX];
    size_t n = kvm_config_serialize(cfg, rec, sizeof rec);
    CHECK(n > 0u);
    rec[8] = (uint8_t)(schema & 0xFFu);
    rec[9] = (uint8_t)((schema >> 8) & 0xFFu);
    uint32_t crc = kvm_config_crc32(rec + 8, n - 8u);
    rec[4] = (uint8_t)(crc & 0xFFu);
    rec[5] = (uint8_t)((crc >> 8) & 0xFFu);
    rec[6] = (uint8_t)((crc >> 16) & 0xFFu);
    rec[7] = (uint8_t)((crc >> 24) & 0xFFu);
    fake_nvs_seed(f, rec, n);
}

static kvm_config_t stored_config(const fake_nvs_t *f)
{
    kvm_config_t out;
    kvm_config_defaults(&out);
    (void)kvm_config_deserialize(f->blob, f->len, &out, NULL);
    return out;
}

static void open_store(kvm_config_store_t *s, fake_nvs_t *f, uint64_t now_ms)
{
    kvm_config_backend_t b = fake_nvs_backend(f);
    kvm_config_debounce_t d = {QUIET_MS, DEFER_MS, INTERVAL_MS};
    (void)kvm_config_store_init(s, &b, &d, now_ms);
}

// ---------------------------------------------------------------------------------------------
// Load and recovery
// ---------------------------------------------------------------------------------------------

static void test_init_first_boot(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    CHECK_EQ(s.load_status, KVM_CONFIG_LOAD_DEFAULTS);
    CHECK(!s.recovered);
    CHECK(!s.future_schema);
    CHECK(!kvm_config_store_is_dirty(&s));

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(kvm_config_store_get(&s), &defaults, sizeof defaults) == 0);

    // A device nobody has configured never writes flash, however long it runs.
    CHECK(!kvm_config_store_tick(&s, 1000000u));
    CHECK_EQ(f.writes, 0);
    CHECK_EQ(kvm_config_store_ms_until_write(&s, 0), UINT32_MAX);
}

static void test_init_reads_back_record(void)
{
    kvm_config_t saved;
    kvm_config_defaults(&saved);
    CHECK(kvm_config_set(&saved, KVM_CFG_F_BRIGHTNESS, 35));
    CHECK(kvm_config_set(&saved, KVM_CFG_F_MOUSE_SENSITIVITY, KVM_MOUSE_SENS_SLOW));
    CHECK(kvm_config_set_layout_id(&saved, "pl"));

    fake_nvs_t f;
    fake_nvs_reset(&f);
    seed_from_config(&f, &saved);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK_EQ(s.load_status, KVM_CONFIG_LOAD_OK);
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK(memcmp(kvm_config_store_get(&s), &saved, sizeof saved) == 0);
    CHECK(!kvm_config_store_tick(&s, 100000u));
    CHECK_EQ(f.writes, 0);
}

// AT-FAULT-02: a corrupt record leaves the device on factory-safe defaults and working.
static void test_init_recovers_from_corruption(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    uint8_t junk[120];
    memset(junk, 0xA5, sizeof junk);
    fake_nvs_seed(&f, junk, sizeof junk);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK_EQ(s.load_status, KVM_CONFIG_LOAD_RECOVERED);
    CHECK(s.recovered);

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(kvm_config_store_get(&s), &defaults, sizeof defaults) == 0);

    // The broken record is replaced, but through the normal debounce: a boot loop must not turn
    // into a write loop.
    CHECK(kvm_config_store_is_dirty(&s));
    CHECK(!kvm_config_store_tick(&s, 0));
    CHECK_EQ(f.writes, 0);
    CHECK(kvm_config_store_tick(&s, QUIET_MS));
    CHECK_EQ(f.writes, 1);
    CHECK(!kvm_config_store_is_dirty(&s));

    kvm_config_store_t again;
    open_store(&again, &f, 0);
    CHECK_EQ(again.load_status, KVM_CONFIG_LOAD_OK);
    CHECK(!again.recovered);
    CHECK(memcmp(kvm_config_store_get(&again), &defaults, sizeof defaults) == 0);
}

static void test_init_read_error_recovers(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    f.fail_read = true;

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK_EQ(s.load_status, KVM_CONFIG_LOAD_RECOVERED);
    CHECK(s.recovered);
    CHECK(kvm_config_store_is_dirty(&s));
}

static void test_init_repaired_record_is_rewritten(void)
{
    kvm_config_t saved;
    kvm_config_defaults(&saved);
    kvm_config_set_unchecked(&saved, KVM_CFG_F_BRIGHTNESS, 200);

    fake_nvs_t f;
    fake_nvs_reset(&f);
    seed_from_config(&f, &saved);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK_EQ(s.load_status, KVM_CONFIG_LOAD_REPAIRED);
    CHECK_EQ(s.load_repair.field_mask, (uint32_t)1u << KVM_CFG_F_BRIGHTNESS);
    CHECK_EQ(kvm_config_store_get(&s)->brightness, 80);
    CHECK(kvm_config_store_is_dirty(&s));
    CHECK(kvm_config_store_tick(&s, QUIET_MS));

    kvm_config_store_t again;
    open_store(&again, &f, 0);
    CHECK_EQ(again.load_status, KVM_CONFIG_LOAD_OK);
}

// A record from newer firmware: run on defaults and leave it alone until the user decides.
static void test_init_future_schema_holds_writes(void)
{
    kvm_config_t saved;
    kvm_config_defaults(&saved);
    CHECK(kvm_config_set(&saved, KVM_CFG_F_BRIGHTNESS, 42));

    fake_nvs_t f;
    fake_nvs_reset(&f);
    seed_with_schema(&f, &saved, (uint16_t)(KVM_CONFIG_SCHEMA_VERSION + 1u));
    uint8_t before[KVM_CONFIG_RECORD_MAX];
    size_t before_len = f.len;
    memcpy(before, f.blob, before_len);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK_EQ(s.load_status, KVM_CONFIG_LOAD_FUTURE_SCHEMA);
    CHECK(s.future_schema);
    CHECK(s.hold_writes);

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(kvm_config_store_get(&s), &defaults, sizeof defaults) == 0);

    // No tick and no explicit flush may overwrite it.
    CHECK(!kvm_config_store_tick(&s, 1000000u));
    CHECK(!kvm_config_store_flush(&s, 1000000u));
    CHECK_EQ(f.writes, 0);
    CHECK(s.stats.holds >= 2u);
    CHECK_EQ(f.len, before_len);
    CHECK(memcmp(f.blob, before, before_len) == 0);
    CHECK_EQ(kvm_config_store_ms_until_write(&s, 1000000u), UINT32_MAX);

    // The first deliberate change releases the hold; the newer record is then replaced.
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 55, 2000u));
    CHECK(!s.hold_writes);
    CHECK(kvm_config_store_tick(&s, 2000u + QUIET_MS));
    CHECK_EQ(f.writes, 1);

    kvm_config_store_t again;
    open_store(&again, &f, 0);
    CHECK_EQ(again.load_status, KVM_CONFIG_LOAD_OK);
    CHECK_EQ(kvm_config_store_get(&again)->brightness, 55);
}

static void test_store_without_backend(void)
{
    kvm_config_store_t s;
    CHECK_EQ(kvm_config_store_init(&s, NULL, NULL, 0), KVM_CONFIG_LOAD_RECOVERED);
    CHECK_EQ(s.debounce.quiet_ms, 1500u);
    CHECK(kvm_config_store_is_dirty(&s));

    // Writes fail loudly instead of pretending to persist.
    CHECK(!kvm_config_store_tick(&s, 100000u));
    CHECK_EQ(s.stats.write_failures, 1);
    CHECK(kvm_config_store_is_dirty(&s));

    // Settings still work in RAM, which is what keeps LOCAL USB usable.
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 30, 0));
    CHECK_EQ(kvm_config_store_get(&s)->brightness, 30);
    CHECK(!kvm_config_store_factory_reset(&s, 0));
    CHECK_EQ(kvm_config_store_get(&s)->brightness, 80);
}

// ---------------------------------------------------------------------------------------------
// Write debounce (TECH-050: never a flash write per tap or per move)
// ---------------------------------------------------------------------------------------------

static void test_debounce_coalesces_a_burst(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    // 50 changes 10 ms apart, as a finger dragging a slider.
    for (unsigned i = 0; i < 50u; i++) {
        uint64_t t = (uint64_t)i * 10u;
        CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_KEEPAWAKE_INTERVAL_S, 10u + i, t));
        CHECK(!kvm_config_store_tick(&s, t));
    }
    CHECK_EQ(f.writes, 0);
    CHECK_EQ(s.stats.changes, 50);
    CHECK_EQ(s.stats.coalesced, 49);

    uint64_t last_change = 490u;
    CHECK(!kvm_config_store_tick(&s, last_change + QUIET_MS - 1u));
    CHECK(kvm_config_store_tick(&s, last_change + QUIET_MS));
    CHECK_EQ(f.writes, 1);
    CHECK_EQ(s.stats.writes, 1);
    CHECK_EQ(stored_config(&f).keepawake_interval_s, 59);
}

static void test_debounce_bounds_a_long_drag(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    // 30 s of uninterrupted changes, 100 ms apart: 300 changes.
    uint32_t value = 10;
    uint64_t last_write_seen = 0;
    bool first = true;
    for (uint64_t t = 0; t <= 30000u; t += 100u) {
        value = (value >= 300u) ? 10u : value + 1u;
        CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_KEEPAWAKE_INTERVAL_S, value, t));
        if (kvm_config_store_tick(&s, t)) {
            if (!first) {
                CHECK(t - last_write_seen >= INTERVAL_MS);
            }
            last_write_seen = t;
            first = false;
        }
    }
    CHECK_EQ(s.stats.changes, 301);
    // max_defer is measured from the moment the record became dirty again, so 30 s of dragging
    // costs a couple of writes rather than one per sample.
    CHECK(f.writes >= 2u);
    CHECK(f.writes <= 4u);

    // Whatever the debounce decided, the value the user left behind is the one that persists.
    CHECK(kvm_config_store_flush(&s, 40000u));
    CHECK_EQ(stored_config(&f).keepawake_interval_s, value);
}

static void test_min_interval_between_writes(void)
{
    kvm_config_t saved;
    kvm_config_defaults(&saved);
    fake_nvs_t f;
    fake_nvs_reset(&f);
    seed_from_config(&f, &saved);

    kvm_config_store_t s;
    open_store(&s, &f, 0);

    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 30, 0));
    CHECK(kvm_config_store_tick(&s, QUIET_MS));
    CHECK_EQ(f.writes, 1);

    // A second change 100 ms later would settle at 3100 ms, but the minimum interval pushes the
    // write out to 3500 ms.
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 31, 1600u));
    CHECK_EQ(kvm_config_store_ms_until_write(&s, 1600u), 1900u);
    CHECK(!kvm_config_store_tick(&s, 3100u));
    CHECK(!kvm_config_store_tick(&s, 3499u));
    CHECK(kvm_config_store_tick(&s, 3500u));
    CHECK_EQ(f.writes, 2);
}

static void test_noop_change_costs_nothing(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 80, 0)); // already the default
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK_EQ(s.stats.changes, 0);

    CHECK(kvm_config_store_set_layout(&s, "us", 0));
    CHECK(!kvm_config_store_is_dirty(&s));

    kvm_config_t same = *kvm_config_store_get(&s);
    CHECK(kvm_config_store_apply(&s, &same, 0, NULL));
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK(!kvm_config_store_tick(&s, 100000u));
    CHECK_EQ(f.writes, 0);

    // A value outside the field range is refused and changes nothing.
    CHECK(!kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 5, 0));
    CHECK(!kvm_config_store_set_field(&s, KVM_CFG_F_CONTROL_APPROVAL, 9, 0));
    CHECK(!kvm_config_store_set_field(&s, (kvm_config_field_t)KVM_CFG_F_COUNT, 0, 0));
    CHECK(!kvm_config_store_set_layout(&s, "Not A Layout", 0));
    CHECK(!kvm_config_store_is_dirty(&s));
}

static void test_flush_bypasses_debounce(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    CHECK(kvm_config_store_flush(&s, 0)); // nothing pending
    CHECK_EQ(f.writes, 0);

    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_DIAG_OVERLAY, 1, 0));
    CHECK(kvm_config_store_flush(&s, 10u));
    CHECK_EQ(f.writes, 1);
    CHECK_EQ(s.stats.immediate_writes, 1);
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK_EQ(stored_config(&f).diag_overlay, 1);
}

static void test_ms_until_write(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    CHECK_EQ(kvm_config_store_ms_until_write(&s, 0), UINT32_MAX);
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 50, 1000u));
    CHECK_EQ(kvm_config_store_ms_until_write(&s, 1000u), QUIET_MS);
    CHECK_EQ(kvm_config_store_ms_until_write(&s, 1000u + QUIET_MS), 0u);
    CHECK_EQ(kvm_config_store_ms_until_write(&s, 1000000u), 0u);
    CHECK_EQ(kvm_config_store_ms_until_write(NULL, 0), UINT32_MAX);
}

// ---------------------------------------------------------------------------------------------
// Backend faults
// ---------------------------------------------------------------------------------------------

static void test_write_failure_is_retried(void)
{
    kvm_config_t saved;
    kvm_config_defaults(&saved);
    fake_nvs_t f;
    fake_nvs_reset(&f);
    seed_from_config(&f, &saved);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    f.fail_writes = 1;

    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 25, 0));
    CHECK(!kvm_config_store_tick(&s, QUIET_MS));
    CHECK_EQ(s.stats.write_failures, 1);
    CHECK_EQ(s.stats.writes, 0);
    CHECK(kvm_config_store_is_dirty(&s));

    // The retry waits for the minimum interval instead of hammering the flash.
    CHECK(!kvm_config_store_tick(&s, QUIET_MS + 1u));
    CHECK(kvm_config_store_tick(&s, QUIET_MS + INTERVAL_MS));
    CHECK_EQ(s.stats.writes, 1);
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK_EQ(stored_config(&f).brightness, 25);
}

// A power cut during a write: NVS keeps the previous record, so the device comes back with its
// old settings rather than with defaults.
static void test_power_loss_keeps_the_old_record(void)
{
    kvm_config_t saved;
    kvm_config_defaults(&saved);
    CHECK(kvm_config_set(&saved, KVM_CFG_F_BRIGHTNESS, 70));
    fake_nvs_t f;
    fake_nvs_reset(&f);
    seed_from_config(&f, &saved);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 30, 0));
    f.power_loss = true;
    CHECK(!kvm_config_store_tick(&s, QUIET_MS));
    CHECK_EQ(f.write_errors, 1);

    kvm_config_store_t reboot;
    open_store(&reboot, &f, 0);
    CHECK_EQ(reboot.load_status, KVM_CONFIG_LOAD_OK);
    CHECK_EQ(kvm_config_store_get(&reboot)->brightness, 70);

    // The still-running store keeps the change pending and lands it on the next attempt.
    CHECK(kvm_config_store_tick(&s, QUIET_MS + INTERVAL_MS));
    CHECK_EQ(stored_config(&f).brightness, 30);
}

// A backend that is not atomic leaves a half-written record. The CRC turns that into a clean
// recovery instead of a half-applied configuration.
static void test_torn_write_recovers_to_defaults(void)
{
    kvm_config_t saved;
    kvm_config_defaults(&saved);
    CHECK(kvm_config_set(&saved, KVM_CFG_F_BRIGHTNESS, 70));
    fake_nvs_t f;
    fake_nvs_reset(&f);
    seed_from_config(&f, &saved);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 30, 0));
    f.torn_write = true;
    CHECK(!kvm_config_store_tick(&s, QUIET_MS));

    kvm_config_store_t reboot;
    open_store(&reboot, &f, 0);
    CHECK_EQ(reboot.load_status, KVM_CONFIG_LOAD_RECOVERED);
    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(kvm_config_store_get(&reboot), &defaults, sizeof defaults) == 0);
}

static void test_oversized_stored_blob_recovers(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    f.present = true;
    f.len = sizeof f.blob; // a full-size blob whose content is not a record
    memset(f.blob, 0x5C, sizeof f.blob);

    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK_EQ(s.load_status, KVM_CONFIG_LOAD_RECOVERED);
}

// ---------------------------------------------------------------------------------------------
// Pairing register (ADR-022 FR-726, SEC-105)
// ---------------------------------------------------------------------------------------------

static void fill_sha(uint8_t sha[KVM_CONFIG_TOKEN_SHA_LEN], uint8_t base)
{
    for (unsigned i = 0; i < KVM_CONFIG_TOKEN_SHA_LEN; i++) {
        sha[i] = (uint8_t)(base + i);
    }
}

static void test_pairing_changes_are_written_at_once(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    uint8_t sha_a[KVM_CONFIG_TOKEN_SHA_LEN];
    uint8_t sha_b[KVM_CONFIG_TOKEN_SHA_LEN];
    fill_sha(sha_a, 0x10);
    fill_sha(sha_b, 0x90);

    CHECK_EQ(kvm_config_store_pair_add(&s, "desk", sha_a, 1762000000u, false, 0), 0);
    CHECK_EQ(f.writes, 1);
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK_EQ(s.stats.immediate_writes, 1);

    CHECK_EQ(kvm_config_store_pair_add(&s, "laptop", sha_b, 1762000100u, true, 10u), 1);
    CHECK_EQ(f.writes, 2);
    CHECK_EQ(stored_config(&f).pair_count, 2);

    CHECK(kvm_config_store_pair_set_trusted(&s, 0, true, 20u));
    CHECK_EQ(f.writes, 3);
    CHECK_EQ(stored_config(&f).pair[0].trusted, 1);

    // A revoke reaches flash immediately, without waiting for any debounce window.
    CHECK(kvm_config_store_pair_remove(&s, 0, 30u));
    CHECK_EQ(f.writes, 4);
    kvm_config_t on_flash = stored_config(&f);
    CHECK_EQ(on_flash.pair_count, 1);
    CHECK_EQ(kvm_config_pair_find(&on_flash, sha_a), -1);
    CHECK_EQ(kvm_config_pair_find(&on_flash, sha_b), 0);

    CHECK_EQ(kvm_config_store_pair_remove_all(&s, 40u), 1);
    CHECK_EQ(f.writes, 5);
    CHECK_EQ(stored_config(&f).pair_count, 0);

    // Nothing to do means no write at all.
    CHECK_EQ(kvm_config_store_pair_remove_all(&s, 50u), 0);
    CHECK(!kvm_config_store_pair_remove(&s, 0, 50u));
    CHECK(!kvm_config_store_pair_set_trusted(&s, 0, true, 50u));
    CHECK_EQ(kvm_config_store_pair_add(&s, "", sha_a, 1, false, 50u), -1);
    CHECK_EQ(f.writes, 5);
}

// AT-SEC-08 / SEC-105: the device keeps the digest of a token, never the token.
static void test_no_token_reaches_storage(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    // What secure_remote holds in RAM: the 32-byte token and the 6-digit pairing code.
    uint8_t token[32];
    for (unsigned i = 0; i < sizeof token; i++) {
        token[i] = (uint8_t)(0x11u + i);
    }
    static const char code[] = "482913";

    // What it hands to config_store: only the digest.
    uint8_t digest[KVM_CONFIG_TOKEN_SHA_LEN];
    fill_sha(digest, 0xC0);

    CHECK_EQ(kvm_config_store_pair_add(&s, "desk", digest, 1762000000u, false, 0), 0);
    CHECK(f.present);

    CHECK(fake_nvs_contains(&f, digest, sizeof digest));
    CHECK(!fake_nvs_contains(&f, token, sizeof token));
    CHECK(!fake_nvs_contains(&f, token, 8));
    CHECK(!fake_nvs_contains(&f, code, sizeof code - 1u));

    // The register itself holds the digest and nothing resembling a token field.
    const kvm_config_t *cfg = kvm_config_store_get(&s);
    CHECK(memcmp(cfg->pair[0].token_sha, digest, sizeof digest) == 0);
    CHECK_EQ(sizeof cfg->pair[0].token_sha, KVM_CONFIG_TOKEN_SHA_LEN);
}

// AT-FAULT-10: factory reset removes every token and role and returns the safe defaults.
static void test_factory_reset_clears_everything(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    uint8_t sha[3][KVM_CONFIG_TOKEN_SHA_LEN];
    for (unsigned i = 0; i < 3u; i++) {
        fill_sha(sha[i], (uint8_t)(0x20u + i * 0x30u));
        CHECK_EQ(kvm_config_store_pair_add(&s, "client", sha[i], 1000u + i, true, 0), (int)i);
    }
    const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    CHECK(kvm_config_store_set_wifi_cache(&s, bssid, 11, 0));
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_PAIR_REQUIRE_ACCEPT, 0, 0));
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_CONTROL_APPROVAL, KVM_CONTROL_APPROVAL_AUTO, 0));
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_REMOTE_ENABLED, 1, 0));
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 20, 0));
    CHECK(kvm_config_store_flush(&s, 100u));
    CHECK(fake_nvs_contains(&f, sha[1], KVM_CONFIG_TOKEN_SHA_LEN));

    CHECK(kvm_config_store_factory_reset(&s, 200u));
    CHECK_EQ(f.erases, 1);

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(kvm_config_store_get(&s), &defaults, sizeof defaults) == 0);
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK(!s.recovered);
    CHECK(!s.future_schema);

    // Nothing is left in storage: no digest, no Wi-Fi hint, policies back at their strictest.
    for (unsigned i = 0; i < 3u; i++) {
        CHECK(!fake_nvs_contains(&f, sha[i], KVM_CONFIG_TOKEN_SHA_LEN));
    }
    CHECK(!fake_nvs_contains(&f, bssid, sizeof bssid));
    kvm_config_t on_flash = stored_config(&f);
    CHECK(memcmp(&on_flash, &defaults, sizeof defaults) == 0);
    CHECK_EQ(on_flash.pair_count, 0);
    CHECK_EQ(on_flash.wifi.valid, 0);
    CHECK_EQ(on_flash.pair_require_accept, 1);
    CHECK_EQ(on_flash.control_approval, KVM_CONTROL_APPROVAL_ALWAYS);
    CHECK_EQ(on_flash.remote_enabled, 0);

    kvm_config_store_t reboot;
    open_store(&reboot, &f, 0);
    CHECK_EQ(reboot.load_status, KVM_CONFIG_LOAD_OK);
    CHECK(memcmp(kvm_config_store_get(&reboot), &defaults, sizeof defaults) == 0);
}

// An erase that fails is reported, but the pairing register is still gone from storage: the reset
// fails closed rather than leaving the old tokens in place.
static void test_factory_reset_with_failing_erase(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    uint8_t sha[KVM_CONFIG_TOKEN_SHA_LEN];
    fill_sha(sha, 0x70);
    CHECK_EQ(kvm_config_store_pair_add(&s, "client", sha, 1, true, 0), 0);
    CHECK(fake_nvs_contains(&f, sha, sizeof sha));

    f.fail_erase = true;
    CHECK(!kvm_config_store_factory_reset(&s, 100u));
    CHECK_EQ(f.erases, 1);
    CHECK_EQ(s.stats.erases, 0);

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(kvm_config_store_get(&s), &defaults, sizeof defaults) == 0);
    CHECK(!fake_nvs_contains(&f, sha, sizeof sha));
    CHECK_EQ(stored_config(&f).pair_count, 0);
}

// ---------------------------------------------------------------------------------------------
// Remaining setters
// ---------------------------------------------------------------------------------------------

static void test_apply_whole_record(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK(kvm_config_store_set_field(&s, KVM_CFG_F_BRIGHTNESS, 20, 0));
    CHECK(kvm_config_store_flush(&s, 0));

    // A Settings screen that edited a working copy.
    kvm_config_t next = *kvm_config_store_get(&s);
    CHECK(kvm_config_set(&next, KVM_CFG_F_SCROLL_SPEED, 8));
    CHECK(kvm_config_set(&next, KVM_CFG_F_NATURAL_SCROLL, 1));
    kvm_config_repair_t rep;
    CHECK(kvm_config_store_apply(&s, &next, 100u, &rep));
    CHECK(!kvm_config_repair_any(&rep));
    CHECK_EQ(kvm_config_store_get(&s)->scroll_speed, 8);
    CHECK(kvm_config_store_is_dirty(&s));

    // A record arriving entirely corrupt is repaired, not rejected.
    kvm_config_t garbage;
    memset(&garbage, 0xFF, sizeof garbage);
    CHECK(kvm_config_store_apply(&s, &garbage, 200u, &rep));
    CHECK(kvm_config_repair_any(&rep));
    CHECK(rep.layout_repaired);
    CHECK(rep.wifi_repaired);
    CHECK(rep.pairing_repaired);

    kvm_config_t defaults;
    kvm_config_defaults(&defaults);
    CHECK(memcmp(kvm_config_store_get(&s), &defaults, sizeof defaults) == 0);

    CHECK(!kvm_config_store_apply(&s, NULL, 0, NULL));
    CHECK(!kvm_config_store_apply(NULL, &defaults, 0, NULL));
}

static void test_wifi_cache_api(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    const uint8_t good[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t multicast[6] = {0x03, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t zeros[6] = {0, 0, 0, 0, 0, 0};

    CHECK(kvm_config_store_set_wifi_cache(&s, good, 11, 0));
    CHECK_EQ(kvm_config_store_get(&s)->wifi.valid, 1);
    CHECK_EQ(kvm_config_store_get(&s)->wifi.channel, 11);
    CHECK(kvm_config_store_is_dirty(&s));
    CHECK(kvm_config_store_flush(&s, 0));

    // Storing the same hint again is not a change.
    CHECK(kvm_config_store_set_wifi_cache(&s, good, 11, 10u));
    CHECK(!kvm_config_store_is_dirty(&s));

    CHECK(!kvm_config_store_set_wifi_cache(&s, good, 0, 10u));
    CHECK(!kvm_config_store_set_wifi_cache(&s, good, 15, 10u));
    CHECK(!kvm_config_store_set_wifi_cache(&s, multicast, 6, 10u));
    CHECK(!kvm_config_store_set_wifi_cache(&s, zeros, 6, 10u));
    CHECK(!kvm_config_store_set_wifi_cache(&s, NULL, 6, 10u));
    CHECK(!kvm_config_store_set_wifi_cache(NULL, good, 6, 10u));
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK_EQ(kvm_config_store_get(&s)->wifi.channel, 11);

    // A failed association drops the hint (TECH-071).
    kvm_config_store_clear_wifi_cache(&s, 20u);
    CHECK_EQ(kvm_config_store_get(&s)->wifi.valid, 0);
    CHECK(kvm_config_store_is_dirty(&s));
    CHECK(kvm_config_store_flush(&s, 20u));
    kvm_config_store_clear_wifi_cache(&s, 30u); // already clear
    CHECK(!kvm_config_store_is_dirty(&s));
    kvm_config_store_clear_wifi_cache(NULL, 30u);
}

static void test_layout_setter(void)
{
    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);

    CHECK(kvm_config_store_set_layout(&s, "de(neo)", 0));
    CHECK(kvm_config_store_is_dirty(&s));
    CHECK(kvm_config_store_flush(&s, 0));
    CHECK(strcmp(stored_config(&f).host_layout, "de(neo)") == 0);

    CHECK(!kvm_config_store_set_layout(&s, "de(", 10u));
    CHECK(!kvm_config_store_set_layout(&s, NULL, 10u));
    CHECK(!kvm_config_store_set_layout(NULL, "us", 10u));
    CHECK(strcmp(kvm_config_store_get(&s)->host_layout, "de(neo)") == 0);
}

/**
 * ADR-039: the two HOST-layout lists go through the store like the layout id — validated, written
 * whole, debounced, and free when the list does not actually change.
 */
static void test_host_list_setters(void)
{
    static const char fav[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP] = {
        "us", "de(neo)", "fr(oss)", "pl", "gb", "es", "it", "cz(qwerty)"};
    static const char recent[KVM_CONFIG_HOST_RECENT_MAX][KVM_CONFIG_LAYOUT_ID_CAP] = {
        "jp", "kr", "ru(phonetic)", "se", "no"};

    fake_nvs_t f;
    fake_nvs_reset(&f);
    kvm_config_store_t s;
    open_store(&s, &f, 0);
    CHECK_EQ(kvm_config_store_get(&s)->host_fav_count, 1); // the shipped favorites
    CHECK(!kvm_config_store_is_dirty(&s));

    CHECK(kvm_config_store_set_favorites(&s, fav, (uint8_t)KVM_CONFIG_HOST_FAV_MAX, 0));
    CHECK(kvm_config_store_set_recents(&s, recent, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX, 0));
    CHECK(kvm_config_store_is_dirty(&s));
    CHECK_EQ(s.stats.changes, 2u);
    CHECK(kvm_config_store_flush(&s, 0));

    const kvm_config_t stored = stored_config(&f);
    CHECK_EQ(stored.host_fav_count, KVM_CONFIG_HOST_FAV_MAX);
    CHECK_EQ(stored.host_recent_count, KVM_CONFIG_HOST_RECENT_MAX);
    CHECK(strcmp(stored.host_fav[7], "cz(qwerty)") == 0);
    CHECK(strcmp(stored.host_recent[2], "ru(phonetic)") == 0);

    // writing the same lists again costs nothing
    const uint32_t writes = s.stats.writes;
    CHECK(kvm_config_store_set_favorites(&s, fav, (uint8_t)KVM_CONFIG_HOST_FAV_MAX, 100u));
    CHECK(kvm_config_store_set_recents(&s, recent, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX, 100u));
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK_EQ(s.stats.writes, writes);

    // an unusable list is refused and leaves neither the record nor the dirty flag touched
    char bad[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP];
    memset(bad, 0, sizeof bad);
    snprintf(bad[0], KVM_CONFIG_LAYOUT_ID_CAP, "us");
    snprintf(bad[1], KVM_CONFIG_LAYOUT_ID_CAP, "de(");
    CHECK(!kvm_config_store_set_favorites(&s, bad, 2u, 200u));
    CHECK(!kvm_config_store_set_recents(&s, bad, 2u, 200u));
    CHECK(!kvm_config_store_set_favorites(&s, fav, (uint8_t)KVM_CONFIG_HOST_FAV_MAX + 1u, 200u));
    CHECK(!kvm_config_store_set_recents(&s, recent, (uint8_t)KVM_CONFIG_HOST_RECENT_MAX + 1u, 200u));
    CHECK(!kvm_config_store_set_favorites(&s, NULL, 0u, 200u)); // the favorites never go empty
    CHECK(!kvm_config_store_is_dirty(&s));
    CHECK_EQ(kvm_config_store_get(&s)->host_fav_count, KVM_CONFIG_HOST_FAV_MAX);

    // the recents can be emptied; a shorter favorites list clears the slots it no longer uses
    CHECK(kvm_config_store_set_recents(&s, NULL, 0u, 300u));
    CHECK(kvm_config_store_set_favorites(&s, fav, 2u, 300u));
    CHECK(kvm_config_store_flush(&s, 3000u));
    const kvm_config_t after = stored_config(&f);
    CHECK_EQ(after.host_recent_count, 0);
    CHECK_EQ(after.host_fav_count, 2);
    CHECK_EQ(after.host_fav[2][0], 0);

    // the factory reset puts both lists back to the shipped record (AT-FAULT-10)
    CHECK(kvm_config_store_factory_reset(&s, 4000u));
    CHECK_EQ(kvm_config_store_get(&s)->host_fav_count, 1);
    CHECK(strcmp(kvm_config_store_get(&s)->host_fav[0], "us") == 0);
    CHECK_EQ(kvm_config_store_get(&s)->host_recent_count, 0);
    CHECK_EQ(stored_config(&f).host_fav_count, 1);
}

static void test_null_tolerance(void)
{
    CHECK_EQ(kvm_config_store_init(NULL, NULL, NULL, 0), KVM_CONFIG_LOAD_RECOVERED);
    CHECK(!kvm_config_store_set_favorites(NULL, NULL, 0, 0));
    CHECK(!kvm_config_store_set_recents(NULL, NULL, 0, 0));
    CHECK(kvm_config_store_get(NULL) == NULL);
    CHECK(!kvm_config_store_is_dirty(NULL));
    CHECK(!kvm_config_store_set_field(NULL, KVM_CFG_F_BRIGHTNESS, 50, 0));
    CHECK(!kvm_config_store_tick(NULL, 0));
    CHECK(!kvm_config_store_flush(NULL, 0));
    CHECK(!kvm_config_store_factory_reset(NULL, 0));
    CHECK_EQ(kvm_config_store_pair_add(NULL, "x", NULL, 0, false, 0), -1);
    CHECK(!kvm_config_store_pair_remove(NULL, 0, 0));
    CHECK(!kvm_config_store_pair_set_trusted(NULL, 0, true, 0));
    CHECK_EQ(kvm_config_store_pair_remove_all(NULL, 0), 0);
    kvm_config_debounce_defaults(NULL);
}

int main(void)
{
    printf("config_store store tests\n");
    RUN(test_init_first_boot);
    RUN(test_init_reads_back_record);
    RUN(test_init_recovers_from_corruption);
    RUN(test_init_read_error_recovers);
    RUN(test_init_repaired_record_is_rewritten);
    RUN(test_init_future_schema_holds_writes);
    RUN(test_store_without_backend);
    RUN(test_debounce_coalesces_a_burst);
    RUN(test_debounce_bounds_a_long_drag);
    RUN(test_min_interval_between_writes);
    RUN(test_noop_change_costs_nothing);
    RUN(test_flush_bypasses_debounce);
    RUN(test_ms_until_write);
    RUN(test_write_failure_is_retried);
    RUN(test_power_loss_keeps_the_old_record);
    RUN(test_torn_write_recovers_to_defaults);
    RUN(test_oversized_stored_blob_recovers);
    RUN(test_pairing_changes_are_written_at_once);
    RUN(test_no_token_reaches_storage);
    RUN(test_factory_reset_clears_everything);
    RUN(test_factory_reset_with_failing_erase);
    RUN(test_apply_whole_record);
    RUN(test_wifi_cache_api);
    RUN(test_layout_setter);
    RUN(test_host_list_setters);
    RUN(test_null_tolerance);
    TEST_MAIN_END("config_store");
}
