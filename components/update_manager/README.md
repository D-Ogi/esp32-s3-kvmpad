# `update_manager`

> **Status:** `IMPLEMENTED_SW / READY_FOR_HIL` (TASK-110). Host tests in `test/update_manager`. Integrated in the firmware
> (steps 1, 3 and 4 below, app rollback on; image signing waits for the owner's key decision, ADR-031). On board #1: boot
> state, http refusal and image-header rejection `PASS_HW`; full update, power cuts and rollback `NOT_RUN_HW`
> (`test/reports/2026-11-02_M8_TASK-110_d36ab0a.yaml`).

**Role:** Signed OTA, maintenance mode, health check, rollback and recovery.

| | |
|---|---|
| Tasks | TASK-110 |
| Specification | 02 §9 (TECH-050..056), 01 PROC-009, ADR-003/004/012, **ADR-031** (signing scheme, image source, `ime_data`), ADR-024 TECH-081 |
| Tests | AT-FAULT-07/08, AT-PERF-09, AT-SEC-07/09 |
| Operator procedure | [`docs/OTA.md`](../../docs/OTA.md) |

## Layout

| File | What it is |
|---|---|
| `include/update_manager.h`, `src/update_manager.c` | the state machine: pure C, no RTOS, no allocation, no ESP-IDF |
| `include/update_image.h`, `src/update_image.c` | candidate-image inspection: header, chip, silicon revision, project, `secure_version` |
| `include/update_health.h`, `src/update_health.c` | the post-update health evaluator (injected probes, consecutive sweeps, timeout) |
| `include/update_manager_esp.h`, `src/update_platform_esp.c` | the ESP-IDF adapter: `esp_ota_*`, the HID interlock, the probes, the update task |
| `include/update_transport.h`, `src/update_transport_https.c` | the locally triggered HTTPS image source |
| `src/update_console.c` | the `ota` console command, registered by the integrator |

The first three are the only ones the host suite builds; the last three are the only ones that touch ESP-IDF.

## The phases

```text
IDLE -> PREPARE -> RECEIVE -> VERIFY -> SET_BOOT -> REBOOT ..reset..
                                                    |
(next boot) PENDING_VERIFY -> HEALTH_CHECK -> CONFIRMED | ROLLBACK
```

Any failure before `SET_BOOT` goes to `ABORTING` and then `FAILED`, with the previous image still owning the boot
partition.

## Rules (MUST)

- **Before OTA:** revoke the remote session, `RELEASE_ALL`, block input, show the `Updating` state — and wait for the
  HID-neutral confirmation before a single byte is written (TECH-053).
- **One boot switch per update,** in `SET_BOOT`, after the signature has verified. Every abort path leaves the old slot
  bootable.
- **Input is released only after the HID state is neutral again.** If it never becomes neutral, input stays blocked and
  the status says so; that is the safe direction.
- **Nothing held is restored after the reboot.** A fresh image that comes up with HID state held is rolled back.
- **No remote path.** This component exposes no entry point a network parser can reach (TECH-045, AT-SEC-07). An update
  is started from the device console or the local Settings UI.
- **No eFuse.** Secure Boot / Flash Encryption remain an operator decision (ADR-012, TECH-054, `AGENTS.md`
  prohibition 6). `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` is refused at compile time because it burns the secure-version
  field; the anti-rollback comparison is in `update_image.c` instead.
- The partition table only after the flash size has been confirmed (ADR-009, `hardware/boards.md`).

## Integration

1. Remove `update_manager` from `EXCLUDE_COMPONENTS` in the root `CMakeLists.txt`.
2. Add the `sdkconfig.defaults` block from ADR-031 (signed apps, signed data partition, app rollback).
3. In `main/app_main.c`, after `hid_dispatch_start()`, `usb_hid_device_start()`, `touch_input_start()` and (when built)
   `ui_platform_start()`, and before the console: `update_manager_start();`.
4. In `main/kvm_console.c`, add the table entry documented at the bottom of `update_manager_esp.h`.
5. Once `remote_protocol` / `secure_remote` are linked in, call
   `update_manager_set_session_revoke(<close every session>, ctx)`.
6. Optional: project `update_manager_get_status()` into the UI's maintenance subject for the `Updating` screen.

Governing documents: `AGENTS.md`, `CONTRIBUTING.md`.
