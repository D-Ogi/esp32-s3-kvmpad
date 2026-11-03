# `test/update_manager` — host tests of the OTA state machine (TASK-110)

Pure-C tests of `components/update_manager` with real clang sanitizers and no ESP-IDF. Only
`src/update_platform_esp.c`, `src/update_transport_https.c` and `src/update_console.c` are left out: they need the IDF
build.

## Commands

```sh
cmake -S test/update_manager -B ../build-host-ota -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-host-ota
ctest --test-dir ../build-host-ota --output-on-failure
```

The build directory is outside the repository. Flags: `-Wall -Wextra -Werror -Wshadow -Wconversion
-Wno-sign-conversion -g -O1` plus AddressSanitizer and UndefinedBehaviorSanitizer with
`-fno-sanitize-recover=all` (`-DKVM_SANITIZE=OFF` turns the sanitizers off).

Expected result:

```text
Test project O:/Praca/KVMpad/build-host-ota
    Start 1: test_update_image
1/3 Test #1: test_update_image ................   Passed    0.14 sec
    Start 2: test_update_health
2/3 Test #2: test_update_health ...............   Passed    0.14 sec
    Start 3: test_update_sm
3/3 Test #3: test_update_sm ...................   Passed    0.10 sec

100% tests passed, 0 tests failed out of 3
```

`test_update_image` runs 1242 checks, `test_update_health` 68 and `test_update_sm` 478; each binary prints its counts
and exits non-zero on the first failure.

## Files

| File | What it holds |
|---|---|
| `test_update_image.c` | image parsing at every truncation length, non-images, string sanitizing, and the chip / revision / project / downgrade matrix |
| `test_update_health.c` | the probe evaluator: consecutive sweeps, a flapping subsystem, a dead required probe, optional probes, deadline edges |
| `test_update_sm.c` | the state machine: the normal path, every abort path, a power cut at each flash operation, cancel, start refusals, health check and rollback |
| `fake_platform.h` | the doubles: a flash slot with a two-copy otadata double and power-cut injection, a byte-stream image source with fault injection, a synthetic image builder, the HID/maintenance recorder and the probe set |

The otadata double is the part AT-FAULT-08 rests on: ESP-IDF keeps two copies with their own CRC and boots the highest
sequence number whose CRC is valid, so `fake_slot_boot_slot()` answers "what would the bootloader pick" after a cut at
any point, and `fake_otadata_consistent()` answers "is the record still unambiguous".

## Coverage map

| Test | Requirement |
|---|---|
| `test_happy_path`, `test_progress_is_monotonic` | PROC-009 order; one boot switch; input blocked across the reset |
| `test_at_fault_07_ota_while_drag_lock` | **AT-FAULT-07**: an update started with a drag lock held releases it before anything is flashed |
| `test_neutral_timeout_leaves_the_slot_untouched`, `test_cancel_in_prepare` | TECH-053: no write without the neutral confirmation; input released only after neutral |
| `test_wrong_chip_never_reaches_flash`, `test_wrong_chip_revision_never_reaches_flash`, `test_downgrade_never_reaches_flash`, `test_wrong_project_never_reaches_flash`, `test_not_an_image_never_reaches_flash`, `test_image_shorter_than_the_header` | the software half of VERIFY (ADR-031 TECH-095): a refusable image never reaches the flash |
| `test_source_open_failure`, `test_size_unknown_is_refused`, `test_declared_size_larger_than_the_slot`, `test_more_bytes_than_declared`, `test_truncated_image`, `test_truncated_inside_the_prefix`, `test_read_error_mid_transfer`, `test_a_source_that_stops_short_is_truncated_not_stalled` | TECH-052 source failures; a truncated image is detectable only because a length is required |
| `test_bad_signature_keeps_the_old_slot` | **AT-SEC-09** software half: a failed signature never reaches the boot partition |
| `test_flash_begin_failure`, `test_flash_write_failure`, `test_readback_mismatch_keeps_the_old_slot`, `test_readback_failure_keeps_the_old_slot` | flash faults at each step; the independent read-back check |
| `test_set_boot_failure_leaves_otadata_alone`, `test_torn_otadata_write_keeps_the_old_slot` | **AT-FAULT-08**: the switch is the only otadata write, and a torn copy keeps the old slot |
| `test_at_fault_08_power_cut_at_every_flash_operation` | **AT-FAULT-08** exhaustively: a cut before each of the ~17 flash operations of a full update; after every one the record is consistent and the device boots either the old slot or a fully switched new one |
| `test_power_cut_before_the_switch_never_boots_the_new_slot` | the written-and-verified-but-not-switched window |
| `test_cancel_in_receive`, `test_cancel_is_refused_once_the_reboot_is_committed`, `test_cancel_in_idle_does_nothing` | operator cancel and its limits |
| `test_start_is_refused_while_busy`, `test_start_is_refused_without_a_source`, `test_start_is_refused_while_the_running_image_is_unconfirmed`, `test_a_second_update_after_a_failure` | start refusals; a refusal never raises the HID interlock; `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` is refused before the interlock |
| `test_health_check_passes_and_marks_valid`, `test_a_subsystem_that_comes_up_late_still_passes` | **AT-PERF-09** software half: input blocked until the health check decides; three clean sweeps, not one sample |
| `test_health_check_timeout_rolls_back`, `test_each_subsystem_alone_can_force_a_rollback` | TECH-052 rollback on an unresponsive USB/GUI, per subsystem |
| `test_held_hid_state_after_boot_rolls_back` | TECH-053: nothing held is restored after a reboot — if it is, the image is rolled back |
| `test_operator_can_confirm_before_the_health_check_ends`, `test_operator_confirm_is_refused_outside_the_health_check`, `test_operator_confirm_reports_a_failed_mark` | `ota mark-valid`: one exit from the health check, refused elsewhere, and a failed mark leaves the image pending and input blocked |
| `test_mark_valid_failure_leaves_the_image_pending` | a confirmation that fails leaves the image pending, which fails safe |
| `test_normal_boot_states_need_no_health_check` | an ordinary boot neither blocks input nor touches otadata |
| `test_null_arguments_are_survivable`, `test_default_config_is_used_when_none_is_given`, `test_state_and_reason_names`, `test_a_small_chunk_size_still_completes` | API hygiene; the hold-back logic at a 64-byte chunk size writes the image byte-for-byte in order |

## Not run here

The signature itself is ESP-IDF's `esp_image_verify()` /
`esp_secure_boot_verify_sbv2_signature_block()`; these tests inject its verdict rather than re-implementing RSA. The
hardware halves of AT-FAULT-07/08, AT-PERF-09 and AT-SEC-07/09 — real power cuts, a real rollback, the panel under a
flash write, and the absence of a remote OTA endpoint on the wire — are `NOT_RUN_HW`. The board plan is in
[`docs/OTA.md`](../../docs/OTA.md) and needs at most six resets.
