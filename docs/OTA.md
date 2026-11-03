# Signed updates — operator procedure

Decision and rationale: [`ADR/ADR-031-ota-source-and-signing.md`](ADR/ADR-031-ota-source-and-signing.md).
Component: [`../components/update_manager/`](../components/update_manager/README.md). Requirements: TECH-051..056,
TECH-095..100, PROC-009. Tests: AT-FAULT-07/08, AT-PERF-09, AT-SEC-07/09.

This document is for the person holding the board. Everything here is deliberate and local: there is no remote command
that updates, resets or flashes a KVMpad, and there never will be one (TECH-045, AT-SEC-07).

## What happens during an update

1. **PREPARE.** The remote session is closed, maintenance is raised — `RELEASE_ALL` with reason `maintenance`, every
   intent refused — and the update waits for the HID state to be confirmed neutral. Nothing is written before that, so
   an update started while a key or a drag lock is held releases it first (AT-FAULT-07). If the HID state never becomes
   neutral the update is abandoned and input stays blocked until it does.
2. **RECEIVE.** The image is fetched over HTTPS into the inactive slot in bounded chunks. The first 288 bytes are held
   back and checked before anything is flashed: image magic, app descriptor, chip, silicon revision, project name and
   the `secure_version` anti-rollback counter. A wrong-chip, wrong-project or downgrade image therefore never touches
   the flash at all.
3. **VERIFY.** `esp_ota_end()` verifies the image and its appended signature. The header is then read back from flash
   and re-checked, so a mis-ordered write cannot slip through.
4. **SET_BOOT.** One `esp_ota_set_boot_partition()` call. This is the only moment the boot slot changes.
5. **REBOOT.** The device restarts with input still blocked. Nothing held before the update is restored (TECH-053).
6. **HEALTH_CHECK** on the next boot. The image boots as `PENDING_VERIFY`, input stays blocked, and the USB stack, HID
   scheduler, LVGL owner task and touch reader must each be seen alive on three consecutive sweeps within 20 s. Then the
   image is marked valid and input is released. Otherwise the device rolls back to the previous slot and reboots.

At any failure before step 4 the previous image keeps the boot partition, and input is released only once the HID state
is neutral again.

## One-time setup: the signing key

The private key never goes into the repository, a build directory inside it, a log or a chat message.

```sh
# A development key, outside the repository (workspace rules 2 and 3):
espsecure generate-signing-key --version 2 --scheme rsa3072 ../keys/kvmpad_ota_dev.pem
```

The release key is generated the same way, kept offline, and copied once to separate physical custody. Point
`CONFIG_SECURE_BOOT_SIGNING_KEY` at a path **outside** the repository (it is evaluated relative to the project
directory, so `../keys/…` is the workspace directory). Add the `sdkconfig.defaults` block from ADR-031.

Lose the key and the devices already in the field can no longer be updated. Leak it and the signature stops meaning
anything.

## The step that cannot be undone remotely: the first signed flash

> **Flash the first signed image over the wire.** With signature verification on update enabled, the running app must
> itself be signed: ESP-IDF's `check_signature_on_update_check()` calls `abort()` when the running app has no signature
> block, because a device that cannot verify any update would be stuck. So the transition from an unsigned to a signed
> build is a wired `idf.py flash`, not an OTA.

The trust anchor is the running image's own signature block (ADR-031): an OTA is accepted only when it is signed with
the same key. Signing with a new key therefore needs a wired flash, or a transition release carrying both signatures.

## Normal use

Over the UART console (`kvmpad>`; the `ota` command is registered by the integrator — see
`update_manager_esp.h`):

```text
ota status                      # phase, slots, whether this build verifies signatures, progress
ota url https://host/kvmpad.bin # start a locally triggered update
ota cancel                      # abandon it; the previous image still boots
ota mark-valid                  # confirm the running image by hand while the health check is pending
```

`ota status` answers the two questions that matter before trusting an update:

- `signed ota yes` — this build verifies image signatures. `NO - bring-up build only` means it does not, and such an
  image must never be released (the release checklist in `04 §13` rejects it).
- `rollback enabled` — a bad image can return to the previous slot by itself. `DISABLED in sdkconfig` means it cannot.

The HTTPS server must send `200` with a `Content-Length`; a chunked response is refused, because without a declared
length a truncated transfer looks exactly like a complete one. The certificate chain is verified against the ESP-IDF
certificate bundle, or against a pinned certificate installed with `update_transport_https_set_cert()`. Plain `http://`
is refused.

## Recovery

| Situation | What to do |
|---|---|
| New image boots but a subsystem is dead | Nothing. The health check rolls back within 20 s and the device returns to the previous image. |
| New image does not boot at all, or resets in a loop | Nothing. `PENDING_VERIFY` is not confirmed, so the bootloader returns to the previous slot on the next reset. |
| `ota status` shows `UNCONFIRMED` and the device is clearly fine | `ota mark-valid`. Until then the next reset rolls back. |
| Both slots unusable | Wired recovery: `idf.py -p COM3 flash`. Hardware Secure Boot is **not** enabled (ADR-031), so the ROM download path is intact. This is the reason TECH-055 asks for a recovery plan before any eFuse is programmed. |
| Update refused with `downgrade` | Expected: the candidate's `secure_version` is lower than the running image's. Build the image with a `secure_version` at least equal to the running one. |
| Update refused with `signature` | The image was not signed with the running image's key, or it is corrupt. Do not work around it. |

## Hardware tests on the board

`AT-FAULT-08` ("power cut during OTA: the previous slot is bootable; no NVS corruption") needs physical resets. Every
reset makes a USB connect/disconnect sound on the HOST, so the plan below uses **six** in total — that is the whole
budget, and the sequence is designed so each reset answers a different question. A reset through `esptool`/RTS counts as
a power cut for everything except the PSRAM and the panel, which this test does not exercise.

Prerequisites: a signed image on the device, a second signed image with a higher `secure_version` on an HTTPS server
reachable from the board, and `ota status` reporting `signed ota yes` / `rollback enabled`.

| # | Reset at | Command before the reset | Expected after the reset | Covers |
|---|---|---|---|---|
| 1 | mid-`RECEIVE` (about 50 %, from `ota status`) | `ota url …` | boots the **old** image; `ota status` → `running ota_0 state=valid` (`last reason none`: a power cut clears the reason, which lives in RAM); local touch and USB HID work | AT-FAULT-08 power cut while writing |
| 2 | after `VERIFY`, before the restart (the log line `boot partition switched` has **not** appeared) | `ota url …` | boots the **old** image; otadata untouched | AT-FAULT-08 power cut before the switch |
| 3 | immediately after `boot partition switched`, before the device restarts by itself | `ota url …` | boots the **new** image as `PENDING_VERIFY`; the health check confirms it; `ota status` → `confirmed` | the switch is atomic; health check passes |
| 4 | during the health check of a deliberately broken image | flash an image whose touch reader is disabled, over OTA | the rollback fires, the device boots the **old** image, `last reason health_timeout`, and the line `rolled back health_timeout, health missing 0x… (<probe>)` names the probe (an RTC record the failing image writes before its software reset) | AT-FAULT-08 rollback, TECH-099 |
| 5 | during `PENDING_VERIFY`, before the health check finishes, with a good image | `ota url …` then reset as soon as the new image logs `health_check 0%` (a good image is confirmed about 0.6 s later) | the bootloader rolls back by itself (`PENDING_VERIFY` → `ABORTED`); the old image boots | rollback without firmware help |
| 6 | normal reset after a confirmed update | — | boots the new image; `state=valid`; `nvs` intact (`config` command shows the settings record) | no NVS corruption |

Record each run in `test/reports/` with the console transcript (raw logs go to `../evidence/`, never into the
repository), and note `AT-PERF-09` alongside: during `RECEIVE` the panel may stutter while flash is written (TECH-021)
but the scan-out must recover, and input must stay blocked until the health check ends.

Until these runs exist, every hardware row of AT-FAULT-07/08, AT-PERF-09 and AT-SEC-09 is `NOT_RUN_HW`.
