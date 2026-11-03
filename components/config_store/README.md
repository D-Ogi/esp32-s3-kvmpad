# `config_store`

> **Status:** `IMPLEMENTED_SW` (TASK-090) — core and NVS backend written, host suite green,
> `NOT_RUN_HW`. The integrator still has to remove `config_store` from `EXCLUDE_COMPONENTS` in the
> root `CMakeLists.txt` and call `config_store_nvs_init()` from the firmware start-up path.

**Role:** NVS: `schema_version`, CRC/validation, migrators, defaults, write debounce, pairing erase.

| | |
|---|---|
| Tasks | TASK-090 |
| Specification | 02 §9 (TECH-050), 01 §8 |
| Tests | AT-FAULT-02/03/10, AT-SEC-08, AT-EXT-18 (host half in `test/config_store`) |

**Rules (MUST):**

- Do not write flash on every tap.
- No storing of keystroke streams.

## Layout

| File | Contents |
|---|---|
| `include/kvm_config.h` | the record: types, defaults, field table, validation, CRC32 serialization, migration, pairing register. Pure C |
| `include/config_store.h` | the store: load, write debounce, pairing operations, factory reset, and the three-call storage backend |
| `include/config_store_nvs.h` | the ESP-IDF entry point |
| `src/config_record.c` | record implementation |
| `src/config_store.c` | store implementation |
| `src/config_store_nvs.c` | the only file that touches ESP-IDF: one NVS blob, plus partition recovery |

`config_record.c` and `config_store.c` are built by the host suite in `test/config_store`. Nothing
here includes TinyUSB, LVGL or a crypto library: the pairing register takes a SHA-256 digest
produced by `secure_remote`, never a token.

## Settings

Every setting has a closed valid range and a default. An unknown or out-of-range value is replaced
by the default, never by a neighboring value (FR-736). The scalar settings are reachable by id
through `kvm_config_field_key()` / `kvm_config_field_range()`, so a Settings screen can render them
without a second table.

| Key | Type | Default | Valid range | Source |
|---|---|---|---|---|
| `mouse.sensitivity` | enum | `normal` | `slow` / `normal` / `fast` | FR-501 |
| `mouse.natural_scroll` | bool | off | on / off | FR-502 |
| `mouse.longpress_right_click` | bool | on | on / off | FR-503 |
| `mouse.two_finger_scroll` | bool | on | on / off | FR-504 |
| `mouse.scroll_speed` | u8 | 3 | 1..10 | TASK-090 |
| `display.brightness` | u8 % | 80 | 10..100 | FR-505 |
| `remote.enabled` | bool | off | on / off | FR-506, TECH-040 |
| `diag.overlay` | bool | off | on / off | FR-509 |
| `pair.require_accept` | bool (policy) | **on** | on / off | FR-732 |
| `control.approval` | enum (policy) | **`always`** | `auto` / `trusted` / `always` | FR-733 |
| `shortcuts.confirm_alt_f4` | bool (policy) | off | on / off | FR-734 |
| `keepawake.enabled` | bool | off | on / off | FR-711 |
| `keepawake.interval_s` | u16 s | 30 | 10..300 | ADR-020 |
| `kbd.unicode_mode` | enum | `host-ime` | `host-ime` / `linux` / `macos` / `windows-hex` / `wincompose` | FR-747 |
| `kbd.host_layout` | id, ≤ 15 chars | `us` | `[a-z0-9_-]+` with an optional `(variant)` | FR-508, FR-741 |
| `kbd.lang_ko` | bool | off | on / off | FR-746, ADR-037 |
| `kbd.lang_zh` | bool | off | on / off | FR-746, ADR-037 |
| `kbd.lang_ja` | bool | off | on / off | FR-746, ADR-037 |
| HOST-layout favorites | up to 8 ids | `us` | each id as `kbd.host_layout`, never empty | FR-741, ADR-039 |
| HOST-layout recents | up to 5 ids | empty | each id as `kbd.host_layout` | FR-741, ADR-039 |
| `wifi` cache | BSSID + channel | absent | channel 1..14, a unicast BSSID | TECH-071 |
| pairing register | up to 8 clients | empty | name ≤ 23 printable ASCII, 32-byte digest, date, `trusted` | FR-726, FR-733 |

`local.takeover` (FR-507) is **not** a setting: ADR-023 lists local priority among the hard
invariants, so it has no key and no toggle.

### HOST-layout lists (ADR-039)

`kvm_config_store_set_favorites()` and `kvm_config_store_set_recents()` replace a whole list in one
step. They check that every entry is a well-formed id and that the count fits the list, and refuse
the call without changing anything otherwise, so a half-written list never reaches flash. The order,
the removal of a duplicate and the truncation belong to the picker; the store stores the list as it
was handed over. An id that the layout tables no longer carry is still a well-formed id and is kept
— falling back to `us` for an entry it cannot resolve is the picker's decision, not the record's.

A stored list is repaired rather than rejected: an entry that is not a usable id is dropped and the
rest keeps its order, a count past the maximum is clamped first, and empty favorites fall back to
`us`, so a damaged list can never fail a load (FR-736).

### Approval policies

The three policy fields are ordered so that a higher numeric value is stricter, and
`kvm_config_policy_apply_floor()` can only raise them. A migrator starts from the defaults, which
are the strictest values, and applies the floor of the record it came from, so moving to a newer
schema cannot silently relax an approval the user had in force (FR-736).

## On-flash format

One NVS blob under `settings` in the `kvmpad_cfg` namespace, little endian:

| Offset | Length | Field |
|---|---|---|
| 0 | 4 | magic `KVCS` |
| 4 | 4 | CRC-32/ISO-HDLC over everything from offset 8 to the end |
| 8 | 2 | `schema_version` (currently 4) |
| 10 | 2 | `payload_len` |
| 12 | 254 + 68·n | payload: the settings, then n pairing entries |

The fixed part is the 44 bytes of the schema-3 settings (`pair_count` at payload offset 40 among
them), then `host_fav_count` at 44, `host_fav[8][16]` at 45..172, `host_recent_count` at 173 and
`host_recent[5][16]` at 174..253. Each list slot is a fixed 16 bytes holding a NUL-terminated id and
a slot past the count is all zero. The two lists sit between the settings and the pairing entries,
so no schema-3 offset moved and only the start of the pairing entries did.

A record is 266 bytes with no paired clients and 810 bytes with all eight. The CRC starts at offset
8 and the magic covers the first four bytes, so every byte of the record is protected: the host
suite flips each of the bits of a full record one at a time and each flip ends in the factory
defaults (AT-FAULT-02). Schema 1 is a synthetic eight-byte legacy payload kept so the migration
path is exercised by a test before a real schema bump. Schema 3 is the previous real layout: the
same fixed part without the two ADR-039 lists, so a schema-3 record is read with its pairing entries
210 bytes earlier and the two lists at their defaults. Schema 2 is schema 3 without the three
ADR-037 language bytes at offsets 41..43, so it is read with its pairing entries 213 bytes earlier
and the three languages at their default (off) as well.

## Write policy (TECH-050)

A settings change only marks the record dirty. `kvm_config_store_tick()` writes once the record has
been quiet for `quiet_ms` (1500 by default), or at the latest `max_defer_ms` (10000) after the
record became dirty, and never sooner than `min_interval_ms` (2000) after the previous attempt.
Thirty seconds of continuous dragging therefore costs two or three writes, not three hundred. A
failed write keeps the record dirty and is retried on a later tick. `kvm_config_store_flush()`
writes immediately, for shutdown, OTA and mode changes.

Pairing changes and the factory reset bypass the debounce: a revoke has to take effect at once
(FR-726). Their write result is reported by the `bool` returning calls; `pair_add` and
`pair_remove_all` return the index and the count, and leave a failed write pending for the next
tick.

One serialization buffer of 810 bytes lives on the stack of whichever task calls `tick()` or
`flush()`. Size that task accordingly.

## Recovery

| Stored record | Result |
|---|---|
| absent (first boot) | defaults, nothing written until the user changes something |
| valid, current schema | loaded as written |
| valid, older schema | migrated, then rewritten through the normal debounce |
| values out of range | repaired to the defaults, then rewritten |
| bad magic / length / CRC, or an unreadable backend | factory defaults, `recovered` set, the broken record replaced through the normal debounce so a boot loop cannot become a write loop |
| newer schema | factory defaults, `future_schema` set, and the stored record left untouched until the first user change |

`config_store_nvs_init()` handles `ESP_ERR_NVS_NO_FREE_PAGES` and `ESP_ERR_NVS_NEW_VERSION_FOUND`
by erasing the partition and initializing it again. It returns `ESP_OK` for every recovery path,
because a device that lost its settings must still work on LOCAL USB; only a failure to bring NVS
up at all is an error.

Factory reset (PROC-010) erases the namespace, returns every setting to its default, clears the
pairing register and the Wi-Fi cache and writes the defaults back at once (AT-FAULT-10).

## Secrets

The pairing register holds a client name, the **SHA-256 of the client token**, the pairing date and
the `trusted` flag. No function in this component accepts a raw token, and `config_store_nvs.c`
logs only the load status, the keys of the settings it had to reset and how many pairing entries
were dropped — never a value, a name or a digest (SEC-105, AT-SEC-08). The Wi-Fi cache holds a
BSSID and a channel, never the passphrase (TECH-071).

## Integration (`main/app_config.c`)

The firmware keeps one store behind a mutex (the component itself is not thread-safe). A writer task
(priority 2, core 0, internal stack because it writes flash) sleeps until
`kvm_config_store_ms_until_write()` says a write is due or a mutation wakes it. `net_status` gets the
ADR-017 cache hooks from it; `reboot` flushes a pending record first. The console `config` command prints
the load status, the write counters and the scalar settings; it never prints the BSSID or a pairing entry.

## Tests

`test/config_store` (host, clang, ASan and UBSan). See that directory's `README.md` for the
commands and the test-to-requirement map. Hardware halves of AT-FAULT-02/03/10 and AT-EXT-18 are
`NOT_RUN_HW`.

Governing documents: `AGENTS.md`.
