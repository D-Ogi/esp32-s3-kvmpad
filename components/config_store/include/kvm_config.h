// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Settings record of the KVMpad device (TASK-090, TECH-050; 01 §8; ADR-017/020/022/023/024).
//
// This header is the data layer: the versioned record, its defaults, the per-field valid range,
// validate-or-default repair, CRC32-protected serialization and migration from an older schema.
// Pure C: no ESP-IDF, no allocation, no I/O, no crypto. The flash side lives in config_store.h.
//
// Invariants that the rest of the firmware relies on (do not weaken without an ADR):
//   * every field has a safe default and a closed valid range; an unknown or corrupt value is
//     replaced by that default, never by a neighboring value (FR-736);
//   * the three approval policies are ordered so that a *higher* numeric value is *stricter*, and
//     a schema migration may only raise a policy, never lower it (ADR-023 FR-736);
//   * the pairing register stores the SHA-256 of a client token, never the token itself
//     (ADR-022 SEC-105). No function in this component accepts a raw token;
//   * the Wi-Fi fast-reconnect cache holds a BSSID and a channel only, never the passphrase
//     (ADR-017 TECH-071);
//   * the two HOST-layout lists are plain id lists: this component checks the shape of an id and
//     the length of a list, and never decides which layouts the picker should offer (ADR-039).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Schema version written into every record. Bump it together with a migrator in config_record.c.
#define KVM_CONFIG_SCHEMA_VERSION 4u

// Oldest schema version this firmware can still migrate from.
#define KVM_CONFIG_SCHEMA_MIN 1u

// Capacity of the keyboard layout id ("us", "de(neo)", ...), including the NUL terminator.
#define KVM_CONFIG_LAYOUT_ID_CAP 16u

// HOST-layout picker lists (ADR-039 FR-741): how many ids the favorites and the recents hold.
#define KVM_CONFIG_HOST_FAV_MAX 8u
#define KVM_CONFIG_HOST_RECENT_MAX 5u

// Capacity of a paired client name, including the NUL terminator.
#define KVM_CONFIG_PAIR_NAME_CAP 24u

// Length of the stored token digest (SHA-256).
#define KVM_CONFIG_TOKEN_SHA_LEN 32u

// Maximum number of paired clients kept in the register (FR-726).
#define KVM_CONFIG_MAX_PAIRED 8u

// Serialized sizes. The record is <header><payload>; the payload length depends on pair_count.
// The fixed part is 44 bytes of schema-3 settings followed by the 210 bytes of the two ADR-039
// HOST-layout lists, so the schema-3 offsets (`pair_count` at payload offset 40 among them) stay
// where they were and only the start of the pairing entries moved.
#define KVM_CONFIG_RECORD_HEADER_LEN 12u
#define KVM_CONFIG_PAYLOAD_FIXED_LEN 254u
#define KVM_CONFIG_PAIR_ENTRY_LEN 68u
#define KVM_CONFIG_RECORD_MAX                                                                      \
    (KVM_CONFIG_RECORD_HEADER_LEN + KVM_CONFIG_PAYLOAD_FIXED_LEN +                                 \
     KVM_CONFIG_MAX_PAIRED * KVM_CONFIG_PAIR_ENTRY_LEN)

// ---------------------------------------------------------------------------------------------
// Enumerations. The numeric values are part of the on-flash format: append, never reorder.
// ---------------------------------------------------------------------------------------------

// FR-501 mouse sensitivity.
typedef enum {
    KVM_MOUSE_SENS_SLOW = 0,
    KVM_MOUSE_SENS_NORMAL = 1,
    KVM_MOUSE_SENS_FAST = 2,
    KVM_MOUSE_SENS_COUNT
} kvm_mouse_sens_t;

// FR-733 control approval policy. Ordered by strictness: AUTO (laxest) < TRUSTED < ALWAYS.
// A migration may only move a value up this scale (ADR-023 FR-736).
typedef enum {
    KVM_CONTROL_APPROVAL_AUTO = 0,
    KVM_CONTROL_APPROVAL_TRUSTED = 1,
    KVM_CONTROL_APPROVAL_ALWAYS = 2,
    KVM_CONTROL_APPROVAL_COUNT
} kvm_control_approval_t;

// FR-747 Unicode delivery mode. HOST_IME is the default because it is the only mode that needs no
// change on the HOST side (no registry key, no extra input source, no extra software).
typedef enum {
    KVM_UNICODE_MODE_HOST_IME = 0,
    KVM_UNICODE_MODE_LINUX = 1,
    KVM_UNICODE_MODE_MACOS = 2,
    KVM_UNICODE_MODE_WINDOWS_HEX = 3,
    KVM_UNICODE_MODE_WINCOMPOSE = 4,
    KVM_UNICODE_MODE_COUNT
} kvm_unicode_mode_t;

// ---------------------------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------------------------

// One paired client (ADR-022 FR-726, FR-733). `token_sha` is the SHA-256 of the client token,
// computed by secure_remote; the token itself never reaches this component.
typedef struct {
    char name[KVM_CONFIG_PAIR_NAME_CAP];
    uint8_t token_sha[KVM_CONFIG_TOKEN_SHA_LEN];
    uint64_t paired_at_unix;
    uint8_t trusted; // 0/1, FR-733 `trusted` mode
} kvm_config_pair_t;

// Wi-Fi fast-reconnect cache (ADR-017 TECH-071). No passphrase, no IP, no SSID.
typedef struct {
    uint8_t valid;    // 0/1
    uint8_t bssid[6];
    uint8_t channel;  // 1..14 when valid
} kvm_wifi_cache_t;

typedef struct {
    // 01 §8 — input and display
    uint8_t mouse_sensitivity;     // kvm_mouse_sens_t, FR-501
    uint8_t natural_scroll;        // 0/1, FR-502
    uint8_t longpress_right_click; // 0/1, FR-503
    uint8_t two_finger_scroll;     // 0/1, FR-504
    uint8_t scroll_speed;          // 1..10 notches per gesture unit, TASK-090
    uint8_t brightness;            // 10..100 percent, FR-505
    uint8_t remote_enabled;        // 0/1, FR-506 / TECH-040 (off on first boot)
    uint8_t diag_overlay;          // 0/1, FR-509

    // ADR-023 — approval policies. Higher value = stricter.
    uint8_t pair_require_accept; // 0/1, FR-732
    uint8_t control_approval;    // kvm_control_approval_t, FR-733
    uint8_t confirm_alt_f4;      // 0/1, FR-734

    // ADR-020 — keep awake
    uint8_t keepawake_enabled;      // 0/1, FR-711
    uint16_t keepawake_interval_s;  // 10..300 s

    // ADR-024 — keyboard
    uint8_t unicode_mode;                      // kvm_unicode_mode_t, FR-747
    char host_layout[KVM_CONFIG_LAYOUT_ID_CAP]; // xkeyboard-config id, FR-508/FR-741

    // ADR-037 — input languages the language key of the KEYBOARD view cycles through (FR-746).
    // English is always available and has no field: these three only say which of the composed
    // languages the user turned on. Default off, so a fresh device types English alone.
    uint8_t lang_ko; // 0/1, Korean 2-set
    uint8_t lang_zh; // 0/1, Simplified Chinese pinyin
    uint8_t lang_ja; // 0/1, Japanese romaji / kana-kanji

    // ADR-039 — the two lists of the HOST-layout picker (FR-741). Each entry is an
    // xkeyboard-config id of the same shape as `host_layout`, NUL-terminated; the slots past the
    // count are all zero. Order is meaning: the favorites are shown in the order the user put them
    // in and the recents newest first. The ordering, the truncation and the removal of a duplicate
    // are computed by the UI; this component validates an id and a count and stores the list as it
    // was handed over. An id that no layout table resolves any more is still a valid id and is kept
    // — falling back to `us` for an unresolvable entry is the picker's decision, not the record's.
    char host_fav[KVM_CONFIG_HOST_FAV_MAX][KVM_CONFIG_LAYOUT_ID_CAP];
    uint8_t host_fav_count;
    char host_recent[KVM_CONFIG_HOST_RECENT_MAX][KVM_CONFIG_LAYOUT_ID_CAP];
    uint8_t host_recent_count;

    // ADR-017 — Wi-Fi fast reconnect
    kvm_wifi_cache_t wifi;

    // ADR-022 — paired clients
    uint8_t pair_count;
    /**
     * `pair` holds a uint64_t, so it is 8-byte aligned and the bytes before it must be accounted
     * for. They are named here rather than left as implicit padding: kvm_config_validate() zeroes
     * them, so two records that agree field by field also agree byte for byte — which is what
     * kvm_config_store_apply() and the record tests compare. Never serialized. The count depends on
     * where `pair_count` lands, so adding a field above means recounting it; a static assertion in
     * config_record.c fails when it no longer adds up.
     */
    uint8_t reserved[3];
    kvm_config_pair_t pair[KVM_CONFIG_MAX_PAIRED];
} kvm_config_t;

// ---------------------------------------------------------------------------------------------
// Scalar field table. Used by the Settings UI, by validation and by the tests, so that a new
// setting cannot be added without a range and a default.
// ---------------------------------------------------------------------------------------------

typedef enum {
    KVM_CFG_F_MOUSE_SENSITIVITY = 0,
    KVM_CFG_F_NATURAL_SCROLL,
    KVM_CFG_F_LONGPRESS_RIGHT_CLICK,
    KVM_CFG_F_TWO_FINGER_SCROLL,
    KVM_CFG_F_SCROLL_SPEED,
    KVM_CFG_F_BRIGHTNESS,
    KVM_CFG_F_REMOTE_ENABLED,
    KVM_CFG_F_DIAG_OVERLAY,
    KVM_CFG_F_PAIR_REQUIRE_ACCEPT,
    KVM_CFG_F_CONTROL_APPROVAL,
    KVM_CFG_F_CONFIRM_ALT_F4,
    KVM_CFG_F_KEEPAWAKE_ENABLED,
    KVM_CFG_F_KEEPAWAKE_INTERVAL_S,
    KVM_CFG_F_UNICODE_MODE,
    KVM_CFG_F_LANG_KO,
    KVM_CFG_F_LANG_ZH,
    KVM_CFG_F_LANG_JA,
    KVM_CFG_F_COUNT
} kvm_config_field_t;

// Stable setting id shown in the UI and in diagnostics, e.g. "control.approval".
const char *kvm_config_field_key(kvm_config_field_t field);

// Closed valid range and default of a scalar field. Returns false for an out-of-table index.
bool kvm_config_field_range(kvm_config_field_t field, uint32_t *out_min, uint32_t *out_max,
                            uint32_t *out_default);

// True for the three ADR-023 approval policies, whose value may only be raised by a migration.
bool kvm_config_field_is_policy(kvm_config_field_t field);

// Read a scalar field. Returns 0 for an out-of-table index.
uint32_t kvm_config_get(const kvm_config_t *cfg, kvm_config_field_t field);

// Write a scalar field, rejecting a value outside the valid range (the record is left unchanged).
bool kvm_config_set(kvm_config_t *cfg, kvm_config_field_t field, uint32_t value);

// Write a scalar field without the range check, truncated to the field width. Only for building
// corrupt records in tests and for the deserializer; production code uses kvm_config_set().
void kvm_config_set_unchecked(kvm_config_t *cfg, kvm_config_field_t field, uint32_t value);

// ---------------------------------------------------------------------------------------------
// Defaults, validation, repair
// ---------------------------------------------------------------------------------------------

// What a validate-or-default pass had to repair. An empty struct means the record was intact.
typedef struct {
    uint32_t field_mask;    // bit (1u << kvm_config_field_t) per repaired scalar field
    bool layout_repaired;   // host_layout was not a usable id
    bool wifi_repaired;     // the BSSID/channel cache was inconsistent and was dropped
    bool pairing_repaired;  // at least one pairing entry was unusable
    uint8_t pairs_dropped;  // how many pairing entries were discarded
    bool host_fav_repaired;    // the favorites list was compacted or reset to the default
    bool host_recent_repaired; // the recents list was compacted
    uint8_t host_ids_dropped;  // how many entries of the two lists together were discarded
} kvm_config_repair_t;

// Factory-safe record: every setting at its default, no paired clients, no Wi-Fi cache.
void kvm_config_defaults(kvm_config_t *cfg);

// Replace every unknown or out-of-range value with its default (FR-736). Also normalizes the
// layout id, the Wi-Fi cache and the pairing register. Returns what had to be repaired.
kvm_config_repair_t kvm_config_validate(kvm_config_t *cfg);

// True when the repair report is not empty.
bool kvm_config_repair_any(const kvm_config_repair_t *repair);

// A layout id is a short lowercase xkeyboard-config name, optionally with a variant in
// parentheses: [a-z0-9_-] plus one "(variant)" group, 1..KVM_CONFIG_LAYOUT_ID_CAP-1 characters.
bool kvm_config_layout_id_valid(const char *id);

// Set host_layout, rejecting an id that does not pass kvm_config_layout_id_valid().
bool kvm_config_set_layout_id(kvm_config_t *cfg, const char *id);

// Replace the whole favorites / recents list (ADR-039 FR-741). Every entry must pass
// kvm_config_layout_id_valid() and `count` must fit the list, otherwise the record is left exactly
// as it was and the call returns false. `ids` may be NULL when `count` is 0, which empties the
// recents; an empty favorites list is refused, because the list always offers at least one layout.
bool kvm_config_set_favorites(kvm_config_t *cfg, const char ids[][KVM_CONFIG_LAYOUT_ID_CAP],
                              uint8_t count);
bool kvm_config_set_recents(kvm_config_t *cfg, const char ids[][KVM_CONFIG_LAYOUT_ID_CAP],
                            uint8_t count);

// The algebra of the two lists, so that one copy of it serves the device, the simulator and the
// tests (ADR-039 section 9). `kvm_config_pin_toggle` pins `id` or, if it is pinned already, unpins
// it, keeping the order of the others; false when the id is malformed, when the eight slots are
// taken, or when unpinning would leave the list empty. `kvm_config_recent_push` puts `id` at the
// head of the recents, removes it from further down, and drops whatever no longer fits.
bool kvm_config_pin_toggle(kvm_config_t *cfg, const char *id);
bool kvm_config_recent_push(kvm_config_t *cfg, const char *id);

// ---------------------------------------------------------------------------------------------
// Approval policy floor (ADR-023 FR-736)
// ---------------------------------------------------------------------------------------------

// Strictness of the three policies, read straight off the record (higher = stricter).
typedef struct {
    uint8_t pair_require_accept;
    uint8_t control_approval;
    uint8_t confirm_alt_f4;
} kvm_config_policy_rank_t;

kvm_config_policy_rank_t kvm_config_policy_rank(const kvm_config_t *cfg);

// Raise every policy of `cfg` to at least `floor`. Never lowers a policy. Used by the migrators so
// that moving to a newer schema cannot relax an approval the user had in force.
bool kvm_config_policy_apply_floor(kvm_config_t *cfg, kvm_config_policy_rank_t floor);

// ---------------------------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------------------------

// Outcome of reading a record. Ordered by severity: a worse outcome wins over a milder one.
typedef enum {
    KVM_CONFIG_LOAD_OK = 0,      // record read as written
    KVM_CONFIG_LOAD_REPAIRED,    // record read, some values were out of range -> defaults
    KVM_CONFIG_LOAD_MIGRATED,    // older schema, migrated forward
    KVM_CONFIG_LOAD_DEFAULTS,    // no record at all (first boot) -> defaults
    KVM_CONFIG_LOAD_RECOVERED,   // record unreadable (magic/CRC/length) -> defaults, AT-FAULT-02
    KVM_CONFIG_LOAD_FUTURE_SCHEMA // written by newer firmware -> defaults, do not overwrite yet
} kvm_config_load_status_t;

// Human-readable status, safe to log (carries no record content).
const char *kvm_config_load_status_str(kvm_config_load_status_t status);

// CRC-32/ISO-HDLC (reflected 0xEDB88320, init and final xor 0xFFFFFFFF) over `len` bytes.
uint32_t kvm_config_crc32(const void *data, size_t len);

// Serialized length of `cfg`. Always <= KVM_CONFIG_RECORD_MAX.
size_t kvm_config_record_len(const kvm_config_t *cfg);

// Write `cfg` as a record into `buf`. Returns the number of bytes written, or 0 if `cap` is too
// small or `cfg` is malformed. The caller is expected to have validated `cfg` already.
size_t kvm_config_serialize(const kvm_config_t *cfg, void *buf, size_t cap);

// Read a record. `out` always ends up holding a usable record: on any failure it is the factory
// default. `out_repair` may be NULL. Never reads outside [buf, buf+len).
kvm_config_load_status_t kvm_config_deserialize(const void *buf, size_t len, kvm_config_t *out,
                                                kvm_config_repair_t *out_repair);

// Schema version stated by a record, or 0 when the header is not readable at all. Does not
// validate the CRC, so a caller must not trust the value for anything but diagnostics.
uint16_t kvm_config_record_schema(const void *buf, size_t len);

// ---------------------------------------------------------------------------------------------
// Pairing register (ADR-022 FR-726, SEC-105)
// ---------------------------------------------------------------------------------------------

// Add or refresh a client. `token_sha` is a SHA-256 digest; an entry with the same digest is
// updated in place. Returns the entry index, or -1 when the register is full or an argument is
// unusable. There is deliberately no overload taking a raw token.
int kvm_config_pair_add(kvm_config_t *cfg, const char *name,
                        const uint8_t token_sha[KVM_CONFIG_TOKEN_SHA_LEN],
                        uint64_t paired_at_unix, bool trusted);

// Index of the client holding `token_sha`, or -1.
int kvm_config_pair_find(const kvm_config_t *cfg,
                         const uint8_t token_sha[KVM_CONFIG_TOKEN_SHA_LEN]);

// Remove one client (the register keeps its order). False for an out-of-range index.
bool kvm_config_pair_remove(kvm_config_t *cfg, uint8_t index);

// Remove every client and wipe the entry storage. Returns how many were removed.
uint8_t kvm_config_pair_remove_all(kvm_config_t *cfg);

// Set or clear the FR-733 `trusted` flag of one client.
bool kvm_config_pair_set_trusted(kvm_config_t *cfg, uint8_t index, bool trusted);

#ifdef __cplusplus
}
#endif
