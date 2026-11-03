// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Settings record: field table, defaults, validate-or-default, CRC32 serialization, migrators
// (TASK-090, TECH-050). Pure C, no allocation, no I/O.
//
// On-flash layout, little endian throughout:
//
//   offset  len  field
//   0       4    magic "KVCS"
//   4       4    CRC-32/ISO-HDLC over everything from offset 8 to the end of the record
//   8       2    schema_version
//   10      2    payload_len
//   12      N    payload
//
// The CRC starts at offset 8, so a single bit flip anywhere past the magic is caught by the CRC and
// a flip inside the magic is caught by the magic check: every byte of the record is covered.
//
// Payload, schema 4 (KVM_CONFIG_PAYLOAD_FIXED_LEN = 254 bytes, then pair_count entries of 68 bytes):
//
//   0   mouse_sensitivity      8   pair_require_accept    16..31  host_layout[16]
//   1   natural_scroll         9   control_approval       32      wifi.valid
//   2   longpress_right_click  10  confirm_alt_f4         33..38  wifi.bssid[6]
//   3   two_finger_scroll      11  keepawake_enabled      39      wifi.channel
//   4   scroll_speed           12..13 keepawake_interval_s (u16)  40  pair_count
//   5   brightness             14  unicode_mode           41      lang_ko
//   6   remote_enabled         15  host_layout length     42      lang_zh
//   7   diag_overlay                                      43      lang_ja
//
//   44        host_fav_count                 173         host_recent_count
//   45..172   host_fav[8][16]                174..253    host_recent[5][16]
//
// The two ADR-039 lists sit at the end of the fixed part, between the schema-3 settings and the
// pairing entries, so every schema-3 offset — `pair_count` at offset 40 among them — stays where it
// was and only the start of the pairing entries moved. Each list slot is a fixed 16 bytes holding a
// NUL-terminated id; a slot past the count is all zero.
//
// Pairing entry: name[24], token_sha[32], paired_at_unix (u64), trusted (u8), 3 reserved zero bytes.
//
// Payload, schema 3 (V3_PAYLOAD_FIXED_LEN = 44 bytes): the fixed part above without the two ADR-039
// lists, so bytes 0..43 are identical and the pairing entries start 210 bytes earlier. A schema-3
// record migrates forward with the two lists at their defaults (favorites = {"us"}, no recents).
//
// Payload, schema 2 (V2_PAYLOAD_FIXED_LEN = 41 bytes): schema 3 without the three ADR-037 language
// bytes; `pair_count` is at offset 40 in every schema from 2 on, so only the start of the pairing
// entries moves. A schema-2 record migrates forward with the three languages at their default (off).
//
// Payload, schema 1 (synthetic legacy schema, 8 bytes): the first eight bytes above and nothing
// else. It exists so that the migration path is exercised by a test before a real schema bump.

#include "kvm_config.h"

#include <stddef.h>
#include <string.h>

#define RECORD_MAGIC_0 'K'
#define RECORD_MAGIC_1 'V'
#define RECORD_MAGIC_2 'C'
#define RECORD_MAGIC_3 'S'

#define V1_PAYLOAD_LEN 8u
#define V2_PAYLOAD_FIXED_LEN 41u // the fixed part before ADR-037 added the three language bytes
#define V3_PAYLOAD_FIXED_LEN 44u // ... and before ADR-039 appended the two HOST-layout lists

// Payload offsets of the two ADR-039 lists inside the fixed part.
#define HOST_FAV_COUNT_OFF V3_PAYLOAD_FIXED_LEN
#define HOST_FAV_OFF (HOST_FAV_COUNT_OFF + 1u)
#define HOST_RECENT_COUNT_OFF (HOST_FAV_OFF + KVM_CONFIG_HOST_FAV_MAX * KVM_CONFIG_LAYOUT_ID_CAP)
#define HOST_RECENT_OFF (HOST_RECENT_COUNT_OFF + 1u)

#define DEFAULT_LAYOUT_ID "us"

typedef struct {
    const char *key;
    uint16_t offset;
    uint8_t width; // 1 or 2 bytes
    uint8_t is_policy;
    uint32_t min;
    uint32_t max;
    uint32_t def;
} field_desc_t;

// The order must match kvm_config_field_t exactly; a static assertion below checks the count.
static const field_desc_t k_fields[KVM_CFG_F_COUNT] = {
    [KVM_CFG_F_MOUSE_SENSITIVITY] = {"mouse.sensitivity", offsetof(kvm_config_t, mouse_sensitivity),
                                     1, 0, 0, KVM_MOUSE_SENS_COUNT - 1u, KVM_MOUSE_SENS_NORMAL},
    [KVM_CFG_F_NATURAL_SCROLL] = {"mouse.natural_scroll", offsetof(kvm_config_t, natural_scroll), 1,
                                  0, 0, 1, 0},
    [KVM_CFG_F_LONGPRESS_RIGHT_CLICK] = {"mouse.longpress_right_click",
                                         offsetof(kvm_config_t, longpress_right_click), 1, 0, 0, 1,
                                         1},
    [KVM_CFG_F_TWO_FINGER_SCROLL] = {"mouse.two_finger_scroll",
                                     offsetof(kvm_config_t, two_finger_scroll), 1, 0, 0, 1, 1},
    [KVM_CFG_F_SCROLL_SPEED] = {"mouse.scroll_speed", offsetof(kvm_config_t, scroll_speed), 1, 0, 1,
                                10, 3},
    [KVM_CFG_F_BRIGHTNESS] = {"display.brightness", offsetof(kvm_config_t, brightness), 1, 0, 10,
                              100, 80},
    [KVM_CFG_F_REMOTE_ENABLED] = {"remote.enabled", offsetof(kvm_config_t, remote_enabled), 1, 0, 0,
                                  1, 0},
    [KVM_CFG_F_DIAG_OVERLAY] = {"diag.overlay", offsetof(kvm_config_t, diag_overlay), 1, 0, 0, 1, 0},
    [KVM_CFG_F_PAIR_REQUIRE_ACCEPT] = {"pair.require_accept",
                                       offsetof(kvm_config_t, pair_require_accept), 1, 1, 0, 1, 1},
    [KVM_CFG_F_CONTROL_APPROVAL] = {"control.approval", offsetof(kvm_config_t, control_approval), 1,
                                    1, 0, KVM_CONTROL_APPROVAL_COUNT - 1u,
                                    KVM_CONTROL_APPROVAL_ALWAYS},
    [KVM_CFG_F_CONFIRM_ALT_F4] = {"shortcuts.confirm_alt_f4",
                                  offsetof(kvm_config_t, confirm_alt_f4), 1, 1, 0, 1, 0},
    [KVM_CFG_F_KEEPAWAKE_ENABLED] = {"keepawake.enabled", offsetof(kvm_config_t, keepawake_enabled),
                                     1, 0, 0, 1, 0},
    [KVM_CFG_F_KEEPAWAKE_INTERVAL_S] = {"keepawake.interval_s",
                                        offsetof(kvm_config_t, keepawake_interval_s), 2, 0, 10, 300,
                                        30},
    [KVM_CFG_F_UNICODE_MODE] = {"kbd.unicode_mode", offsetof(kvm_config_t, unicode_mode), 1, 0, 0,
                                KVM_UNICODE_MODE_COUNT - 1u, KVM_UNICODE_MODE_HOST_IME},
    [KVM_CFG_F_LANG_KO] = {"kbd.lang_ko", offsetof(kvm_config_t, lang_ko), 1, 0, 0, 1, 0},
    [KVM_CFG_F_LANG_ZH] = {"kbd.lang_zh", offsetof(kvm_config_t, lang_zh), 1, 0, 0, 1, 0},
    [KVM_CFG_F_LANG_JA] = {"kbd.lang_ja", offsetof(kvm_config_t, lang_ja), 1, 0, 0, 1, 0},
};

_Static_assert(KVM_CFG_F_COUNT <= 32, "the repair bitmask holds 32 fields");
_Static_assert(KVM_CONFIG_RECORD_MAX == 810u, "record layout changed without a schema bump");
_Static_assert(HOST_RECENT_OFF + KVM_CONFIG_HOST_RECENT_MAX * KVM_CONFIG_LAYOUT_ID_CAP ==
                   KVM_CONFIG_PAYLOAD_FIXED_LEN,
               "the ADR-039 lists must fill the fixed part exactly");
// No implicit padding before `pair`: kvm_config_validate() can only zero bytes that have a name.
_Static_assert(offsetof(kvm_config_t, pair) ==
                   offsetof(kvm_config_t, reserved) + sizeof(((kvm_config_t *)0)->reserved),
               "kvm_config_t: adjust `reserved` so no implicit padding sits before `pair`");

// ---------------------------------------------------------------------------------------------
// Field table access
// ---------------------------------------------------------------------------------------------

static const field_desc_t *desc_of(kvm_config_field_t field)
{
    if ((unsigned)field >= (unsigned)KVM_CFG_F_COUNT) {
        return NULL;
    }
    return &k_fields[field];
}

const char *kvm_config_field_key(kvm_config_field_t field)
{
    const field_desc_t *d = desc_of(field);
    return d ? d->key : "";
}

bool kvm_config_field_range(kvm_config_field_t field, uint32_t *out_min, uint32_t *out_max,
                            uint32_t *out_default)
{
    const field_desc_t *d = desc_of(field);
    if (!d) {
        return false;
    }
    if (out_min) {
        *out_min = d->min;
    }
    if (out_max) {
        *out_max = d->max;
    }
    if (out_default) {
        *out_default = d->def;
    }
    return true;
}

bool kvm_config_field_is_policy(kvm_config_field_t field)
{
    const field_desc_t *d = desc_of(field);
    return d && d->is_policy != 0u;
}

static uint32_t field_read(const kvm_config_t *cfg, const field_desc_t *d)
{
    const uint8_t *base = (const uint8_t *)cfg;
    if (d->width == 1u) {
        uint8_t v = 0;
        memcpy(&v, base + d->offset, sizeof v);
        return v;
    }
    uint16_t v = 0;
    memcpy(&v, base + d->offset, sizeof v);
    return v;
}

static void field_write(kvm_config_t *cfg, const field_desc_t *d, uint32_t value)
{
    uint8_t *base = (uint8_t *)cfg;
    if (d->width == 1u) {
        uint8_t v = (uint8_t)(value & 0xFFu);
        memcpy(base + d->offset, &v, sizeof v);
        return;
    }
    uint16_t v = (uint16_t)(value & 0xFFFFu);
    memcpy(base + d->offset, &v, sizeof v);
}

uint32_t kvm_config_get(const kvm_config_t *cfg, kvm_config_field_t field)
{
    const field_desc_t *d = desc_of(field);
    if (!cfg || !d) {
        return 0;
    }
    return field_read(cfg, d);
}

bool kvm_config_set(kvm_config_t *cfg, kvm_config_field_t field, uint32_t value)
{
    const field_desc_t *d = desc_of(field);
    if (!cfg || !d || value < d->min || value > d->max) {
        return false;
    }
    field_write(cfg, d, value);
    return true;
}

void kvm_config_set_unchecked(kvm_config_t *cfg, kvm_config_field_t field, uint32_t value)
{
    const field_desc_t *d = desc_of(field);
    if (cfg && d) {
        field_write(cfg, d, value);
    }
}

// ---------------------------------------------------------------------------------------------
// Layout id
// ---------------------------------------------------------------------------------------------

static bool layout_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool kvm_config_layout_id_valid(const char *id)
{
    if (!id || id[0] == '\0') {
        return false;
    }
    size_t n = 0;
    while (n < KVM_CONFIG_LAYOUT_ID_CAP && id[n] != '\0') {
        n++;
    }
    if (n == 0u || n >= KVM_CONFIG_LAYOUT_ID_CAP) {
        return false; // empty, or no terminator inside the field
    }

    size_t i = 0;
    while (i < n && layout_char_ok(id[i])) {
        i++;
    }
    if (i == 0u) {
        return false; // must start with a base-name character
    }
    if (i == n) {
        return true; // base name only, e.g. "us"
    }
    if (id[i] != '(' || id[n - 1u] != ')') {
        return false;
    }
    size_t v = i + 1u;
    if (v >= n - 1u) {
        return false; // "us()" has an empty variant
    }
    for (size_t k = v; k < n - 1u; k++) {
        if (!layout_char_ok(id[k])) {
            return false;
        }
    }
    return true;
}

bool kvm_config_set_layout_id(kvm_config_t *cfg, const char *id)
{
    if (!cfg || !kvm_config_layout_id_valid(id)) {
        return false;
    }
    memset(cfg->host_layout, 0, sizeof cfg->host_layout);
    size_t n = strlen(id);
    memcpy(cfg->host_layout, id, n);
    return true;
}

// ---------------------------------------------------------------------------------------------
// HOST-layout lists (ADR-039 FR-741)
// ---------------------------------------------------------------------------------------------

// Write one list in a single step: either every id passes and the whole list is replaced, or
// nothing is touched. `min_count` is 1 for the favorites, which never go empty, and 0 for the
// recents. The caller has already decided the order and removed any duplicate.
static bool set_host_list(char (*slots)[KVM_CONFIG_LAYOUT_ID_CAP], uint8_t *slot_count,
                          uint8_t capacity, const char ids[][KVM_CONFIG_LAYOUT_ID_CAP],
                          uint8_t count, uint8_t min_count)
{
    if (count > capacity || count < min_count) {
        return false;
    }
    if (count > 0u && !ids) {
        return false;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (!kvm_config_layout_id_valid(ids[i])) {
            return false;
        }
    }
    memset(slots, 0, (size_t)capacity * KVM_CONFIG_LAYOUT_ID_CAP);
    for (uint8_t i = 0; i < count; i++) {
        memcpy(slots[i], ids[i], strlen(ids[i]));
    }
    *slot_count = count;
    return true;
}

bool kvm_config_set_favorites(kvm_config_t *cfg, const char ids[][KVM_CONFIG_LAYOUT_ID_CAP],
                              uint8_t count)
{
    if (!cfg) {
        return false;
    }
    return set_host_list(cfg->host_fav, &cfg->host_fav_count, (uint8_t)KVM_CONFIG_HOST_FAV_MAX, ids,
                         count, 1u);
}

bool kvm_config_set_recents(kvm_config_t *cfg, const char ids[][KVM_CONFIG_LAYOUT_ID_CAP],
                            uint8_t count)
{
    if (!cfg) {
        return false;
    }
    return set_host_list(cfg->host_recent, &cfg->host_recent_count,
                         (uint8_t)KVM_CONFIG_HOST_RECENT_MAX, ids, count, 0u);
}

bool kvm_config_pin_toggle(kvm_config_t *cfg, const char *id)
{
    if (!cfg || !kvm_config_layout_id_valid(id)) {
        return false;
    }
    char list[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP] = {{0}};
    uint8_t n = 0;
    bool pinned = false;
    const uint8_t have = cfg->host_fav_count <= KVM_CONFIG_HOST_FAV_MAX ? cfg->host_fav_count
                                                                        : (uint8_t)KVM_CONFIG_HOST_FAV_MAX;
    for (uint8_t i = 0; i < have; i++) {
        if (strncmp(cfg->host_fav[i], id, KVM_CONFIG_LAYOUT_ID_CAP) == 0) {
            pinned = true; // unpin: the copy simply leaves it out, so the order of the rest survives
            continue;
        }
        memcpy(list[n++], cfg->host_fav[i], strlen(cfg->host_fav[i]));
    }
    if (!pinned) {
        if (n >= KVM_CONFIG_HOST_FAV_MAX) {
            return false; // the eight are the user's: the device never throws one of them away
        }
        memcpy(list[n++], id, strlen(id));
    }
    return kvm_config_set_favorites(cfg, list, n); // refuses an empty list: the last favorite stays
}

bool kvm_config_recent_push(kvm_config_t *cfg, const char *id)
{
    if (!cfg || !kvm_config_layout_id_valid(id)) {
        return false;
    }
    char list[KVM_CONFIG_HOST_RECENT_MAX][KVM_CONFIG_LAYOUT_ID_CAP] = {{0}};
    uint8_t n = 0;
    memcpy(list[n++], id, strlen(id)); // newest first, and never twice
    const uint8_t have = cfg->host_recent_count <= KVM_CONFIG_HOST_RECENT_MAX
                             ? cfg->host_recent_count
                             : (uint8_t)KVM_CONFIG_HOST_RECENT_MAX;
    for (uint8_t i = 0; i < have && n < KVM_CONFIG_HOST_RECENT_MAX; i++) {
        if (strncmp(cfg->host_recent[i], id, KVM_CONFIG_LAYOUT_ID_CAP) == 0) {
            continue;
        }
        memcpy(list[n++], cfg->host_recent[i], strlen(cfg->host_recent[i]));
    }
    return kvm_config_set_recents(cfg, list, n); // the oldest one past the fifth slot falls off here
}

// Validate-or-default repair of one list: an entry that is not a usable id is dropped and the rest
// of the list keeps its order, a count past the capacity is clamped first, and the slots past the
// surviving count are zeroed so a validated record is byte-for-byte canonical. Never fails a load.
static bool repair_host_list(char (*slots)[KVM_CONFIG_LAYOUT_ID_CAP], uint8_t *slot_count,
                             uint8_t capacity, uint8_t *dropped)
{
    bool repaired = false;
    if (*slot_count > capacity) {
        *slot_count = capacity;
        repaired = true;
    }
    uint8_t kept = 0;
    for (uint8_t i = 0; i < *slot_count; i++) {
        if (!kvm_config_layout_id_valid(slots[i])) {
            repaired = true;
            if (*dropped < 255u) {
                (*dropped)++;
            }
            continue;
        }
        if (kept != i) {
            memcpy(slots[kept], slots[i], KVM_CONFIG_LAYOUT_ID_CAP);
        }
        kept++;
    }
    *slot_count = kept;
    for (uint8_t i = kept; i < capacity; i++) {
        memset(slots[i], 0, KVM_CONFIG_LAYOUT_ID_CAP);
    }
    return repaired;
}

// ---------------------------------------------------------------------------------------------
// Defaults and validation
// ---------------------------------------------------------------------------------------------

void kvm_config_defaults(kvm_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        field_write(cfg, &k_fields[f], k_fields[f].def);
    }
    memcpy(cfg->host_layout, DEFAULT_LAYOUT_ID, sizeof DEFAULT_LAYOUT_ID);
    // ADR-039: the picker always has something to show, so the shipped favorites hold the shipped
    // layout and nothing else; nothing has been used yet, so the recents are empty.
    memcpy(cfg->host_fav[0], DEFAULT_LAYOUT_ID, sizeof DEFAULT_LAYOUT_ID);
    cfg->host_fav_count = 1u;
    cfg->host_recent_count = 0u;
    cfg->wifi.valid = 0;
    cfg->pair_count = 0;
}

static bool pair_sha_is_zero(const uint8_t sha[KVM_CONFIG_TOKEN_SHA_LEN])
{
    uint8_t acc = 0;
    for (unsigned i = 0; i < KVM_CONFIG_TOKEN_SHA_LEN; i++) {
        acc = (uint8_t)(acc | sha[i]);
    }
    return acc == 0u;
}

static bool pair_name_ok(const char name[KVM_CONFIG_PAIR_NAME_CAP])
{
    unsigned i = 0;
    while (i < KVM_CONFIG_PAIR_NAME_CAP && name[i] != '\0') {
        if (name[i] < 0x20 || name[i] > 0x7E) {
            return false;
        }
        i++;
    }
    return i > 0u && i < KVM_CONFIG_PAIR_NAME_CAP;
}

// A client entry is usable only when both the name and the digest are intact. A half-readable
// entry is dropped rather than patched up: losing a pairing forces a re-pair (ADR-022), while
// keeping a damaged one could grant control to the wrong digest.
static bool pair_entry_ok(const kvm_config_pair_t *p)
{
    return pair_name_ok(p->name) && !pair_sha_is_zero(p->token_sha);
}

static bool wifi_cache_ok(const kvm_wifi_cache_t *w)
{
    if (w->valid > 1u) {
        return false;
    }
    if (w->valid == 0u) {
        return w->channel == 0u; // a cleared cache must be fully cleared
    }
    if (w->channel < 1u || w->channel > 14u) {
        return false;
    }
    if ((w->bssid[0] & 0x01u) != 0u) {
        return false; // group/broadcast address is never a BSSID
    }
    uint8_t acc = 0;
    for (unsigned i = 0; i < 6u; i++) {
        acc = (uint8_t)(acc | w->bssid[i]);
    }
    return acc != 0u;
}

kvm_config_repair_t kvm_config_validate(kvm_config_t *cfg)
{
    kvm_config_repair_t rep;
    memset(&rep, 0, sizeof rep);
    if (!cfg) {
        return rep;
    }

    for (unsigned f = 0; f < (unsigned)KVM_CFG_F_COUNT; f++) {
        const field_desc_t *d = &k_fields[f];
        uint32_t v = field_read(cfg, d);
        if (v < d->min || v > d->max) {
            field_write(cfg, d, d->def);
            rep.field_mask |= (uint32_t)1u << f;
        }
    }

    // Not a setting: the named alignment bytes before `pair`. Zeroing them keeps a validated record
    // byte-for-byte canonical, so the dirty check of kvm_config_store_apply() compares fields only.
    memset(cfg->reserved, 0, sizeof cfg->reserved);

    if (!kvm_config_layout_id_valid(cfg->host_layout)) {
        memset(cfg->host_layout, 0, sizeof cfg->host_layout);
        memcpy(cfg->host_layout, DEFAULT_LAYOUT_ID, sizeof DEFAULT_LAYOUT_ID);
        rep.layout_repaired = true;
    }

    if (repair_host_list(cfg->host_fav, &cfg->host_fav_count, (uint8_t)KVM_CONFIG_HOST_FAV_MAX,
                         &rep.host_ids_dropped)) {
        rep.host_fav_repaired = true;
    }
    if (cfg->host_fav_count == 0u) {
        memcpy(cfg->host_fav[0], DEFAULT_LAYOUT_ID, sizeof DEFAULT_LAYOUT_ID);
        cfg->host_fav_count = 1u;
        rep.host_fav_repaired = true;
    }
    if (repair_host_list(cfg->host_recent, &cfg->host_recent_count,
                         (uint8_t)KVM_CONFIG_HOST_RECENT_MAX, &rep.host_ids_dropped)) {
        rep.host_recent_repaired = true;
    }

    if (!wifi_cache_ok(&cfg->wifi)) {
        memset(&cfg->wifi, 0, sizeof cfg->wifi);
        rep.wifi_repaired = true;
    }

    if (cfg->pair_count > KVM_CONFIG_MAX_PAIRED) {
        cfg->pair_count = (uint8_t)KVM_CONFIG_MAX_PAIRED;
        rep.pairing_repaired = true;
    }
    uint8_t kept = 0;
    for (uint8_t i = 0; i < cfg->pair_count; i++) {
        if (!pair_entry_ok(&cfg->pair[i])) {
            rep.pairing_repaired = true;
            if (rep.pairs_dropped < 255u) {
                rep.pairs_dropped++;
            }
            continue;
        }
        if (cfg->pair[i].trusted > 1u) {
            cfg->pair[i].trusted = 0u; // unknown flag -> not trusted (the stricter side)
            rep.pairing_repaired = true;
        }
        if (kept != i) {
            cfg->pair[kept] = cfg->pair[i];
        }
        kept++;
    }
    cfg->pair_count = kept;
    for (unsigned i = cfg->pair_count; i < KVM_CONFIG_MAX_PAIRED; i++) {
        memset(&cfg->pair[i], 0, sizeof cfg->pair[i]);
    }
    return rep;
}

bool kvm_config_repair_any(const kvm_config_repair_t *repair)
{
    if (!repair) {
        return false;
    }
    return repair->field_mask != 0u || repair->layout_repaired || repair->wifi_repaired ||
           repair->pairing_repaired || repair->host_fav_repaired || repair->host_recent_repaired;
}

// ---------------------------------------------------------------------------------------------
// Policy floor
// ---------------------------------------------------------------------------------------------

kvm_config_policy_rank_t kvm_config_policy_rank(const kvm_config_t *cfg)
{
    kvm_config_policy_rank_t r = {0, 0, 0};
    if (!cfg) {
        return r;
    }
    r.pair_require_accept = (uint8_t)kvm_config_get(cfg, KVM_CFG_F_PAIR_REQUIRE_ACCEPT);
    r.control_approval = (uint8_t)kvm_config_get(cfg, KVM_CFG_F_CONTROL_APPROVAL);
    r.confirm_alt_f4 = (uint8_t)kvm_config_get(cfg, KVM_CFG_F_CONFIRM_ALT_F4);
    return r;
}

static bool raise_to(kvm_config_t *cfg, kvm_config_field_t field, uint8_t floor)
{
    uint32_t cur = kvm_config_get(cfg, field);
    if (cur >= floor) {
        return false;
    }
    return kvm_config_set(cfg, field, floor);
}

bool kvm_config_policy_apply_floor(kvm_config_t *cfg, kvm_config_policy_rank_t floor)
{
    if (!cfg) {
        return false;
    }
    bool raised = false;
    raised |= raise_to(cfg, KVM_CFG_F_PAIR_REQUIRE_ACCEPT, floor.pair_require_accept);
    raised |= raise_to(cfg, KVM_CFG_F_CONTROL_APPROVAL, floor.control_approval);
    raised |= raise_to(cfg, KVM_CFG_F_CONFIRM_ALT_F4, floor.confirm_alt_f4);
    return raised;
}

// ---------------------------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------------------------

uint32_t kvm_config_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    if (!p) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (unsigned b = 0; b < 8u; b++) {
            uint32_t mask = (uint32_t)0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------------------------

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

size_t kvm_config_record_len(const kvm_config_t *cfg)
{
    uint8_t count = 0;
    if (cfg) {
        count = cfg->pair_count > KVM_CONFIG_MAX_PAIRED ? (uint8_t)KVM_CONFIG_MAX_PAIRED
                                                        : cfg->pair_count;
    }
    return (size_t)KVM_CONFIG_RECORD_HEADER_LEN + KVM_CONFIG_PAYLOAD_FIXED_LEN +
           (size_t)count * KVM_CONFIG_PAIR_ENTRY_LEN;
}

size_t kvm_config_serialize(const kvm_config_t *cfg, void *buf, size_t cap)
{
    if (!cfg || !buf || cfg->pair_count > KVM_CONFIG_MAX_PAIRED) {
        return 0;
    }
    size_t total = kvm_config_record_len(cfg);
    if (cap < total) {
        return 0;
    }

    uint8_t *out = (uint8_t *)buf;
    memset(out, 0, total);
    out[0] = RECORD_MAGIC_0;
    out[1] = RECORD_MAGIC_1;
    out[2] = RECORD_MAGIC_2;
    out[3] = RECORD_MAGIC_3;
    put_u16(out + 8, (uint16_t)KVM_CONFIG_SCHEMA_VERSION);
    put_u16(out + 10, (uint16_t)(total - KVM_CONFIG_RECORD_HEADER_LEN));

    uint8_t *pl = out + KVM_CONFIG_RECORD_HEADER_LEN;
    pl[0] = cfg->mouse_sensitivity;
    pl[1] = cfg->natural_scroll;
    pl[2] = cfg->longpress_right_click;
    pl[3] = cfg->two_finger_scroll;
    pl[4] = cfg->scroll_speed;
    pl[5] = cfg->brightness;
    pl[6] = cfg->remote_enabled;
    pl[7] = cfg->diag_overlay;
    pl[8] = cfg->pair_require_accept;
    pl[9] = cfg->control_approval;
    pl[10] = cfg->confirm_alt_f4;
    pl[11] = cfg->keepawake_enabled;
    put_u16(pl + 12, cfg->keepawake_interval_s);
    pl[14] = cfg->unicode_mode;

    size_t layout_len = 0;
    while (layout_len < KVM_CONFIG_LAYOUT_ID_CAP && cfg->host_layout[layout_len] != '\0') {
        layout_len++;
    }
    if (layout_len >= KVM_CONFIG_LAYOUT_ID_CAP) {
        return 0; // unterminated id: the caller skipped kvm_config_validate()
    }
    pl[15] = (uint8_t)layout_len;
    memcpy(pl + 16, cfg->host_layout, layout_len);

    pl[32] = cfg->wifi.valid;
    memcpy(pl + 33, cfg->wifi.bssid, sizeof cfg->wifi.bssid);
    pl[39] = cfg->wifi.channel;
    pl[40] = cfg->pair_count;
    pl[41] = cfg->lang_ko;
    pl[42] = cfg->lang_zh;
    pl[43] = cfg->lang_ja;

    if (cfg->host_fav_count > KVM_CONFIG_HOST_FAV_MAX ||
        cfg->host_recent_count > KVM_CONFIG_HOST_RECENT_MAX) {
        return 0; // over-long list: the caller skipped kvm_config_validate()
    }
    pl[HOST_FAV_COUNT_OFF] = cfg->host_fav_count;
    memcpy(pl + HOST_FAV_OFF, cfg->host_fav, sizeof cfg->host_fav);
    pl[HOST_RECENT_COUNT_OFF] = cfg->host_recent_count;
    memcpy(pl + HOST_RECENT_OFF, cfg->host_recent, sizeof cfg->host_recent);

    uint8_t *e = pl + KVM_CONFIG_PAYLOAD_FIXED_LEN;
    for (uint8_t i = 0; i < cfg->pair_count; i++) {
        const kvm_config_pair_t *p = &cfg->pair[i];
        memcpy(e, p->name, sizeof p->name);
        memcpy(e + 24, p->token_sha, sizeof p->token_sha);
        put_u64(e + 56, p->paired_at_unix);
        e[64] = p->trusted;
        e += KVM_CONFIG_PAIR_ENTRY_LEN;
    }

    put_u32(out + 4, kvm_config_crc32(out + 8, total - 8u));
    return total;
}

uint16_t kvm_config_record_schema(const void *buf, size_t len)
{
    const uint8_t *in = (const uint8_t *)buf;
    if (!in || len < KVM_CONFIG_RECORD_HEADER_LEN) {
        return 0;
    }
    return get_u16(in + 8);
}

// Schema 1 -> current. The eight legacy settings are copied as-is (validation repairs anything out
// of range); every field introduced later takes its default, which for the ADR-023 policies is the
// strictest value. The policy floor is applied explicitly so the FR-736 rule stays visible here
// even if a later migrator starts carrying policy bytes over.
static void migrate_v1(const uint8_t *pl, kvm_config_t *out)
{
    kvm_config_defaults(out);
    kvm_config_policy_rank_t floor = kvm_config_policy_rank(out);

    kvm_config_set_unchecked(out, KVM_CFG_F_MOUSE_SENSITIVITY, pl[0]);
    kvm_config_set_unchecked(out, KVM_CFG_F_NATURAL_SCROLL, pl[1]);
    kvm_config_set_unchecked(out, KVM_CFG_F_LONGPRESS_RIGHT_CLICK, pl[2]);
    kvm_config_set_unchecked(out, KVM_CFG_F_TWO_FINGER_SCROLL, pl[3]);
    kvm_config_set_unchecked(out, KVM_CFG_F_SCROLL_SPEED, pl[4]);
    kvm_config_set_unchecked(out, KVM_CFG_F_BRIGHTNESS, pl[5]);
    kvm_config_set_unchecked(out, KVM_CFG_F_REMOTE_ENABLED, pl[6]);
    kvm_config_set_unchecked(out, KVM_CFG_F_DIAG_OVERLAY, pl[7]);

    kvm_config_policy_apply_floor(out, floor);
}

// Schema 2, 3 or 4. Each schema only appended to the fixed part, so the three differ in the length
// of that part and therefore in where the pairing entries start: a schema-2 record leaves the three
// ADR-037 languages at their default (all off) and a schema-2 or schema-3 record leaves the two
// ADR-039 HOST-layout lists at theirs (favorites = {"us"}, no recents), which is the same answer a
// factory-fresh device gives.
static void parse_v2_to_v4(const uint8_t *pl, uint8_t count, uint16_t schema, kvm_config_t *out)
{
    kvm_config_defaults(out);

    kvm_config_set_unchecked(out, KVM_CFG_F_MOUSE_SENSITIVITY, pl[0]);
    kvm_config_set_unchecked(out, KVM_CFG_F_NATURAL_SCROLL, pl[1]);
    kvm_config_set_unchecked(out, KVM_CFG_F_LONGPRESS_RIGHT_CLICK, pl[2]);
    kvm_config_set_unchecked(out, KVM_CFG_F_TWO_FINGER_SCROLL, pl[3]);
    kvm_config_set_unchecked(out, KVM_CFG_F_SCROLL_SPEED, pl[4]);
    kvm_config_set_unchecked(out, KVM_CFG_F_BRIGHTNESS, pl[5]);
    kvm_config_set_unchecked(out, KVM_CFG_F_REMOTE_ENABLED, pl[6]);
    kvm_config_set_unchecked(out, KVM_CFG_F_DIAG_OVERLAY, pl[7]);
    kvm_config_set_unchecked(out, KVM_CFG_F_PAIR_REQUIRE_ACCEPT, pl[8]);
    kvm_config_set_unchecked(out, KVM_CFG_F_CONTROL_APPROVAL, pl[9]);
    kvm_config_set_unchecked(out, KVM_CFG_F_CONFIRM_ALT_F4, pl[10]);
    kvm_config_set_unchecked(out, KVM_CFG_F_KEEPAWAKE_ENABLED, pl[11]);
    kvm_config_set_unchecked(out, KVM_CFG_F_KEEPAWAKE_INTERVAL_S, get_u16(pl + 12));
    kvm_config_set_unchecked(out, KVM_CFG_F_UNICODE_MODE, pl[14]);

    uint8_t layout_len = pl[15];
    memset(out->host_layout, 0, sizeof out->host_layout);
    if (layout_len < KVM_CONFIG_LAYOUT_ID_CAP) {
        memcpy(out->host_layout, pl + 16, layout_len);
    }

    out->wifi.valid = pl[32];
    memcpy(out->wifi.bssid, pl + 33, sizeof out->wifi.bssid);
    out->wifi.channel = pl[39];

    size_t fixed = V2_PAYLOAD_FIXED_LEN;
    if (schema >= 3u) {
        kvm_config_set_unchecked(out, KVM_CFG_F_LANG_KO, pl[41]);
        kvm_config_set_unchecked(out, KVM_CFG_F_LANG_ZH, pl[42]);
        kvm_config_set_unchecked(out, KVM_CFG_F_LANG_JA, pl[43]);
        fixed = V3_PAYLOAD_FIXED_LEN;
    }
    if (schema >= 4u) {
        out->host_fav_count = pl[HOST_FAV_COUNT_OFF];
        memcpy(out->host_fav, pl + HOST_FAV_OFF, sizeof out->host_fav);
        out->host_recent_count = pl[HOST_RECENT_COUNT_OFF];
        memcpy(out->host_recent, pl + HOST_RECENT_OFF, sizeof out->host_recent);
        fixed = KVM_CONFIG_PAYLOAD_FIXED_LEN;
    }

    out->pair_count = count;
    const uint8_t *e = pl + fixed;
    for (uint8_t i = 0; i < count; i++) {
        kvm_config_pair_t *p = &out->pair[i];
        memcpy(p->name, e, sizeof p->name);
        memcpy(p->token_sha, e + 24, sizeof p->token_sha);
        p->paired_at_unix = get_u64(e + 56);
        p->trusted = e[64];
        e += KVM_CONFIG_PAIR_ENTRY_LEN;
    }
}

kvm_config_load_status_t kvm_config_deserialize(const void *buf, size_t len, kvm_config_t *out,
                                                kvm_config_repair_t *out_repair)
{
    kvm_config_repair_t rep;
    memset(&rep, 0, sizeof rep);
    if (!out) {
        if (out_repair) {
            *out_repair = rep;
        }
        return KVM_CONFIG_LOAD_RECOVERED;
    }
    kvm_config_defaults(out);

    const uint8_t *in = (const uint8_t *)buf;
    if (!in || len < KVM_CONFIG_RECORD_HEADER_LEN || len > KVM_CONFIG_RECORD_MAX) {
        if (out_repair) {
            *out_repair = rep;
        }
        return KVM_CONFIG_LOAD_RECOVERED;
    }
    if (in[0] != RECORD_MAGIC_0 || in[1] != RECORD_MAGIC_1 || in[2] != RECORD_MAGIC_2 ||
        in[3] != RECORD_MAGIC_3) {
        if (out_repair) {
            *out_repair = rep;
        }
        return KVM_CONFIG_LOAD_RECOVERED;
    }

    uint32_t stored_crc = get_u32(in + 4);
    uint16_t schema = get_u16(in + 8);
    uint16_t payload_len = get_u16(in + 10);
    if ((size_t)payload_len + KVM_CONFIG_RECORD_HEADER_LEN != len) {
        if (out_repair) {
            *out_repair = rep;
        }
        return KVM_CONFIG_LOAD_RECOVERED; // truncated or oversized
    }
    if (kvm_config_crc32(in + 8, len - 8u) != stored_crc) {
        if (out_repair) {
            *out_repair = rep;
        }
        return KVM_CONFIG_LOAD_RECOVERED; // AT-FAULT-02
    }

    if (schema > KVM_CONFIG_SCHEMA_VERSION) {
        if (out_repair) {
            *out_repair = rep;
        }
        return KVM_CONFIG_LOAD_FUTURE_SCHEMA;
    }
    if (schema < KVM_CONFIG_SCHEMA_MIN) {
        if (out_repair) {
            *out_repair = rep;
        }
        return KVM_CONFIG_LOAD_RECOVERED;
    }

    const uint8_t *pl = in + KVM_CONFIG_RECORD_HEADER_LEN;
    bool migrated = false;

    if (schema == 1u) {
        if (payload_len != V1_PAYLOAD_LEN) {
            if (out_repair) {
                *out_repair = rep;
            }
            return KVM_CONFIG_LOAD_RECOVERED;
        }
        migrate_v1(pl, out);
        migrated = true;
    } else {
        size_t fixed = (size_t)V2_PAYLOAD_FIXED_LEN;
        if (schema >= 4u) {
            fixed = (size_t)KVM_CONFIG_PAYLOAD_FIXED_LEN;
        } else if (schema >= 3u) {
            fixed = (size_t)V3_PAYLOAD_FIXED_LEN;
        }
        if ((size_t)payload_len < fixed) {
            if (out_repair) {
                *out_repair = rep;
            }
            return KVM_CONFIG_LOAD_RECOVERED;
        }
        uint8_t count = pl[40];
        size_t expect = fixed + (size_t)count * KVM_CONFIG_PAIR_ENTRY_LEN;
        if (count > KVM_CONFIG_MAX_PAIRED || expect != (size_t)payload_len) {
            if (out_repair) {
                *out_repair = rep;
            }
            return KVM_CONFIG_LOAD_RECOVERED;
        }
        parse_v2_to_v4(pl, count, schema, out);
        migrated = schema < KVM_CONFIG_SCHEMA_VERSION;
    }

    rep = kvm_config_validate(out);
    if (out_repair) {
        *out_repair = rep;
    }
    if (migrated) {
        return KVM_CONFIG_LOAD_MIGRATED;
    }
    return kvm_config_repair_any(&rep) ? KVM_CONFIG_LOAD_REPAIRED : KVM_CONFIG_LOAD_OK;
}

const char *kvm_config_load_status_str(kvm_config_load_status_t status)
{
    switch (status) {
    case KVM_CONFIG_LOAD_OK:
        return "ok";
    case KVM_CONFIG_LOAD_REPAIRED:
        return "repaired";
    case KVM_CONFIG_LOAD_MIGRATED:
        return "migrated";
    case KVM_CONFIG_LOAD_DEFAULTS:
        return "defaults";
    case KVM_CONFIG_LOAD_RECOVERED:
        return "recovered";
    case KVM_CONFIG_LOAD_FUTURE_SCHEMA:
        return "future-schema";
    default:
        return "unknown";
    }
}

// ---------------------------------------------------------------------------------------------
// Pairing register
// ---------------------------------------------------------------------------------------------

int kvm_config_pair_find(const kvm_config_t *cfg,
                         const uint8_t token_sha[KVM_CONFIG_TOKEN_SHA_LEN])
{
    if (!cfg || !token_sha) {
        return -1;
    }
    uint8_t count = cfg->pair_count > KVM_CONFIG_MAX_PAIRED ? (uint8_t)KVM_CONFIG_MAX_PAIRED
                                                            : cfg->pair_count;
    for (uint8_t i = 0; i < count; i++) {
        if (memcmp(cfg->pair[i].token_sha, token_sha, KVM_CONFIG_TOKEN_SHA_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int kvm_config_pair_add(kvm_config_t *cfg, const char *name,
                        const uint8_t token_sha[KVM_CONFIG_TOKEN_SHA_LEN],
                        uint64_t paired_at_unix, bool trusted)
{
    if (!cfg || !name || !token_sha || name[0] == '\0') {
        return -1;
    }
    size_t n = 0;
    while (n < KVM_CONFIG_PAIR_NAME_CAP && name[n] != '\0') {
        if (name[n] < 0x20 || name[n] > 0x7E) {
            return -1;
        }
        n++;
    }
    if (n == 0u || n >= KVM_CONFIG_PAIR_NAME_CAP) {
        return -1; // name does not fit with its terminator
    }
    if (pair_sha_is_zero(token_sha)) {
        return -1;
    }

    int at = kvm_config_pair_find(cfg, token_sha);
    if (at < 0) {
        if (cfg->pair_count >= KVM_CONFIG_MAX_PAIRED) {
            return -1;
        }
        at = (int)cfg->pair_count;
        cfg->pair_count = (uint8_t)(cfg->pair_count + 1u);
    }

    kvm_config_pair_t *p = &cfg->pair[at];
    memset(p, 0, sizeof *p);
    memcpy(p->name, name, n);
    memcpy(p->token_sha, token_sha, KVM_CONFIG_TOKEN_SHA_LEN);
    p->paired_at_unix = paired_at_unix;
    p->trusted = trusted ? 1u : 0u;
    return at;
}

bool kvm_config_pair_remove(kvm_config_t *cfg, uint8_t index)
{
    if (!cfg || index >= cfg->pair_count || cfg->pair_count > KVM_CONFIG_MAX_PAIRED) {
        return false;
    }
    for (uint8_t i = index; i + 1u < cfg->pair_count; i++) {
        cfg->pair[i] = cfg->pair[i + 1u];
    }
    cfg->pair_count = (uint8_t)(cfg->pair_count - 1u);
    memset(&cfg->pair[cfg->pair_count], 0, sizeof cfg->pair[cfg->pair_count]);
    return true;
}

uint8_t kvm_config_pair_remove_all(kvm_config_t *cfg)
{
    if (!cfg) {
        return 0;
    }
    uint8_t had = cfg->pair_count > KVM_CONFIG_MAX_PAIRED ? (uint8_t)KVM_CONFIG_MAX_PAIRED
                                                          : cfg->pair_count;
    memset(cfg->pair, 0, sizeof cfg->pair);
    cfg->pair_count = 0;
    return had;
}

bool kvm_config_pair_set_trusted(kvm_config_t *cfg, uint8_t index, bool trusted)
{
    if (!cfg || index >= cfg->pair_count || cfg->pair_count > KVM_CONFIG_MAX_PAIRED) {
        return false;
    }
    cfg->pair[index].trusted = trusted ? 1u : 0u;
    return true;
}
