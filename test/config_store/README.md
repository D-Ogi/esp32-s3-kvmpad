# `test/config_store` — host tests of the settings store (TASK-090)

Pure-C tests of `components/config_store` with real clang sanitizers and no ESP-IDF. Only
`src/config_store_nvs.c` is left out: it needs the IDF build.

## Commands

```sh
cmake -S test/config_store -B ../build-host-config -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-host-config
ctest --test-dir ../build-host-config --output-on-failure
```

The build directory is outside the repository. Flags: `-Wall -Wextra -Werror -Wshadow -Wconversion
-Wno-sign-conversion -g -O1` plus AddressSanitizer and UndefinedBehaviorSanitizer with
`-fno-sanitize-recover=all` (`-DKVM_SANITIZE=OFF` turns the sanitizers off).

Expected result:

```text
Test project O:/Praca/KVMpad/build-host-config
    Start 1: test_config_record
1/2 Test #1: test_config_record ...............   Passed    0.10 sec
    Start 2: test_config_store
2/2 Test #2: test_config_store ................   Passed    0.08 sec

100% tests passed, 0 tests failed out of 2
```

`test_config_record` runs 3437 checks and `test_config_store` 704; both binaries print the counts
and exit non-zero on the first failure.

## Files

| File | What it holds |
|---|---|
| `test_config_record.c` | the record: defaults, field table, validate-or-default, CRC32, serialization, corruption, migration |
| `test_config_store.c` | the store: load and recovery paths, write debounce on a fake clock, backend faults, pairing, factory reset |
| `fake_nvs.h` | in-memory backend with fault injection (`fail_read`, `fail_writes`, `fail_erase`, `power_loss`, `torn_write`) and a byte scanner for the stored record |

## Coverage map

| Test | Requirement |
|---|---|
| `test_defaults`, `test_field_table` | 01 §8 defaults; ADR-023 policy defaults are the strictest value of their range |
| `test_every_field_out_of_range_falls_back`, `test_all_fields_corrupt_at_once` | FR-736: an unknown value falls back to its default, per field and all at once |
| `test_every_single_bit_flip_recovers` | AT-FAULT-02: all 6480 single-bit corruptions of a full record end in the factory defaults |
| `test_truncated_and_oversized`, `test_header_rejects`, `test_pair_count_mismatch` | a truncated, oversized, mis-stamped or internally inconsistent record is rejected |
| `test_future_schema`, `test_init_future_schema_holds_writes` | a record from newer firmware is neither parsed nor overwritten until the user changes a setting |
| `test_migrate_v1_to_v2`, `test_migrate_v1_repairs_and_rejects`, `test_policy_floor` | the migrator, and FR-736's rule that a migration never relaxes a policy |
| `test_debounce_coalesces_a_burst`, `test_debounce_bounds_a_long_drag`, `test_min_interval_between_writes`, `test_noop_change_costs_nothing` | TECH-050 / AT-FAULT-03: no flash write per tap or per move |
| `test_write_failure_is_retried`, `test_power_loss_keeps_the_old_record`, `test_torn_write_recovers_to_defaults` | backend faults: a failed write is retried, a power cut keeps the previous record, a torn record recovers |
| `test_pairing_changes_are_written_at_once`, `test_pairing_register`, `test_pairing_validation` | FR-726: list, remove, remove all, and a revoke that reaches storage immediately |
| `test_no_token_reaches_storage` | AT-SEC-08 / SEC-105: the stored record holds the digest, never the token or the pairing code |
| `test_factory_reset_clears_everything`, `test_factory_reset_with_failing_erase` | AT-FAULT-10: every client and the Wi-Fi hint are gone, policies back at their strictest |
| `test_wifi_cache_api`, `test_layout_ids`, `test_layout_setter` | TECH-071 (no secret in the cache) and FR-741 layout id validation |
| `test_host_lists_defaults`, `test_host_list_setters` (record and store), `test_host_lists_roundtrip` | ADR-039 / FR-741: the favorites and recents of the HOST-layout picker, validated whole and stored as handed over |
| `test_host_lists_repaired`, `test_host_list_keeps_an_unresolvable_id` | FR-736 for the two lists: a corrupt entry is dropped and the order kept, empty favorites fall back to `us`, and an id no table carries is still valid |
| `test_migrate_v3_to_v4` | the schema 3 -> 4 migration: the two lists take their defaults and every schema-3 value, pairing entries included, survives |

## Not run here

`AT-FAULT-02/03/10` and `AT-EXT-18` also have a hardware half (a corrupted NVS partition on the
board, a write under RGB stress, the Settings screen). Those are `NOT_RUN_HW`: this directory only
proves the logic on the host.
