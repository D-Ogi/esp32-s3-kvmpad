# `update_manager`

**Role:** Signed OTA, maintenance mode, health check, rollback and recovery.

| Reference | Details |
|---|---|
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
- **No remote path.** This component exposes no entry point a network parser can reach (TECH-045, AT-SEC-07). Start an update from the local device console using `ota url`.
- **No eFuse.** The firmware leaves Secure Boot and Flash Encryption eFuses untouched. `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` is refused at compile time because it burns the secure-version
  field; the anti-rollback comparison is in `update_image.c` instead.
- Confirm the physical flash size before using the partition table. [Board measurement procedure](../../hardware/boards.md)

## Integration

`main/app_main.c` starts the update manager and registers `remote_glue_drop_all_for_maintenance` as the session revoke callback. The local `ota` console command reads the manager's status. See the [OTA guide](../../docs/OTA.md) for image signing, verification and rollback.
