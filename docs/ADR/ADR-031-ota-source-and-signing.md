# ADR-031 — OTA image source, signing scheme and the update path

Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)

## Context

TECH-052 asks for a signed OTA with verification of the source chain *and* of the image signature, a health check at
startup, rollback on an unresponsive USB/GUI and protection against power loss. TECH-054 and ADR-012 put hardware Secure
Boot v2 and Flash Encryption behind an explicit operator decision, because the eFuses are irreversible, and `AGENTS.md`
prohibition 6 forbids the implementer from programming them at all. TECH-045 and AT-SEC-07 forbid any flashing path that
a remote, unauthenticated or merely authenticated CLIENT could reach.

Three questions had to be answered before the state machine could be written:

1. **What signs the image, and does that burn an eFuse?**
2. **Where does the image come from, and what is the trust anchor?**
3. **How does `ime_data` (TECH-081) get updated, since both OTA slots share it?**

The 16 MiB partition table (`partitions.csv`, ADR-009) already provides `ota_0`/`ota_1` of 4 MiB each, `otadata`, and the
6 MiB `ime_data` partition with subtype `data, undefined`.

## Options — signing

| | Option | Assessment |
|---|---|---|
| A | Hardware Secure Boot v2 (`CONFIG_SECURE_BOOT`) | REJECTED for v1 — burns the key-digest eFuse and disables JTAG; ADR-012/TECH-054 reserve this for the operator after a recovery test and key escrow. The implementer may not do it. |
| B | **Signed app images without hardware secure boot** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, RSA-3072 secure-boot-v2 scheme) | **CHOSEN** — the same signature format and the same verification code as hardware secure boot, enforced by `esp_ota_end()`, and no eFuse is written. |
| C | Our own detached ECDSA-P256/SHA-256 check in `update_manager` over mbedTLS/PSA, public key compiled in | REJECTED — hand-written signature verification where the pinned tree already has an audited implementation; it would also have to re-implement image hashing and the "verify before the boot switch" ordering. Kept as the documented fallback if B ever becomes unavailable. |

### Evidence that option B burns no eFuse

Quoted from the pinned tree, `O:\Praca\KVMpad\.tools\esp-idf-v6.1` (IDF v6.1):

`components/bootloader/Kconfig.projbuild`, the option itself:

```text
    config SECURE_SIGNED_APPS_NO_SECURE_BOOT
        bool "Require signed app images"
        depends on !SECURE_BOOT
        help
            Require apps to be signed to verify their integrity.

            This option uses the same app signature scheme as hardware secure boot, but unlike hardware secure boot it
            does not prevent the bootloader from being physically updated. This means that the device can be secured
            against remote network access, but not physical access. Compared to using hardware Secure Boot this option
            is much simpler to implement.
```

and the verification-on-update switch (default `y` once the option above is set):

```text
    config SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
        bool "Verify app signature on update"
        default y
        depends on SECURE_SIGNED_APPS_NO_SECURE_BOOT
        help
            If this option is set, any OTA updated apps will have the signature verified before being considered valid.

            When enabled, the signature is automatically checked whenever the esp_ota_ops.h APIs are used for OTA
            updates, or esp_image_format.h APIs are used to verify apps.

            If hardware secure boot is enabled, this option is always enabled and cannot be disabled.
            If hardware secure boot is not enabled, this option still adds significant security against network-based
            attackers by preventing spoofing of OTA updates.
```

`depends on !SECURE_BOOT` is what makes the two mutually exclusive, so selecting the signed-apps option cannot turn on
the hardware feature. In the sources, every eFuse write in the secure boot path is inside `#ifdef CONFIG_SECURE_BOOT`:

- `components/bootloader_support/src/secure_boot.c` — `esp_efuse_batch_write_begin/commit`, `esp_efuse_set_key_dis_read`
  and `esp_efuse_set_key_dis_write` live in `secure_boot_v1_check()` / `secure_boot_v2_check()` / `update_efuses()`,
  all inside `#ifdef CONFIG_SECURE_BOOT`. With it unset, `esp_secure_boot_init_checks()` runs only
  `check_signature_on_update_check()`, which *reads* the running app's signature blocks and writes nothing.
- `components/efuse/src/esp_efuse_startup.c` — the writing branches are guarded by `CONFIG_SECURE_FLASH_ENC_ENABLED`,
  `SOC_EFUSE_ECDSA_USE_HARDWARE_K` (not an ESP32-S3 capability — absent from
  `components/soc/esp32s3/include/soc/Kconfig.soc_caps.in`), `CONFIG_SECURE_DISABLE_ROM_DL_MODE`,
  `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE` and `ROM_LOG_MODE`, which the source comments as
  "For CONFIG_BOOT_ROM_LOG_ALWAYS_ON (default) […] leave ROM_LOG_MODE undefined". None of those is set by this decision.

Measured on 2026-11-02 with an isolated build of the component (`O:\Praca\KVMpad\build-fw-ota`, outside the repository):
the generated `sdkconfig` contains `CONFIG_SECURE_SIGNED_APPS=y`, `CONFIG_SECURE_SIGNED_ON_UPDATE=y`,
`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y` and **no** `CONFIG_SECURE_BOOT`; the build appends exactly one 4096-byte
signature sector (655 360 → 659 456 bytes) and `flash_args` lists only bootloader, partition table, `ota_data_initial`
and the app — no eFuse step. `espsecure verify-signature --version 2` reports
`Signature block 0 verification successful using the supplied key (RSA)`.

### What the trust anchor actually is

Without hardware secure boot the trusted key digest does not come from an eFuse. From
`components/bootloader_support/src/secure_boot_v2/secure_boot_signatures_app.c`:

```c
static esp_err_t get_secure_boot_key_digests(esp_image_sig_public_key_digests_t *public_key_digests)
{
    if (!esp_secure_boot_enabled()) { // CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
        // Gets key digests from running app
        ESP_LOGI(TAG, "Take trusted digest key(s) from running app");
        return esp_secure_boot_get_signature_blocks_for_running_app(true, public_key_digests);
```

So **the running image's own signature block is the root of trust**: an update is accepted only when it carries a
signature from the same key as the image currently executing, and that image got onto the device by a physical, wired
flash. The consequences, which are the honest limits of this scheme:

- A network attacker cannot push firmware without the private key. This is what TECH-052 asks for.
- Someone with physical access can reflash the bootloader and the app over UART/USB and replace the anchor. That is
  exactly the gap hardware Secure Boot closes, and it stays open until the operator decides otherwise (ADR-012).
- The first signed image must be flashed by wire. Enabling signature verification on a device whose running app is
  unsigned is a brick: `check_signature_on_update_check()` calls `abort()` when the running app carries no signature
  block. See `docs/OTA.md`.
- Rotating the signing key needs a transition release signed with **both** the old and the new key (the signature sector
  holds up to three blocks), and only the first block is used to verify the next update on this configuration.
- On ESP32-S3 the scheme is RSA-3072: `SECURE_SIGNED_APPS_ECDSA_SCHEME` needs `SECURE_BOOT_V1_SUPPORTED` and
  `SECURE_SIGNED_APPS_ECDSA_V2_SCHEME` needs `SECURE_BOOT_V2_ECC_SUPPORTED`; `Kconfig.soc_caps.in` for the ESP32-S3
  declares `SOC_SECURE_BOOT_V2_RSA` only. The bootloader cannot be made to verify signatures on this target either
  (`SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT` depends on the ECDSA/V1 scheme), so verification happens in the app, on update.

### Anti-rollback

`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` is **not** used. Its own help text says the security version "is recorded in the
eFuse field as the number of set ones", which is an irreversible write and therefore out of bounds for the implementer
(prohibition 6, ADR-012). Instead `update_manager` compares `esp_app_desc_t.secure_version` of the candidate against the
running image in software and refuses a lower value (`KVM_OTA_R_DOWNGRADE`), before a single byte reaches the slot. This
stops a *remote* downgrade, which is the threat TECH-052 names; it does not survive a physical reflash, and neither does
the signature anchor above. If the owner later accepts the eFuse cost, enabling the IDF option adds bootloader-level
enforcement on top without changing this component.

## Options — image source

| | Option | Assessment |
|---|---|---|
| A | **Locally triggered HTTPS pull**: the operator starts the update on the device (console `ota url <https-url>`, later Settings), the device fetches the image over TLS with a CA-verified or pinned certificate | **CHOSEN for v1** |
| B | Push over the CLIENT protocol | REJECTED — AT-SEC-07 and TECH-045 forbid a remote flashing path; the remote protocol has no OTA command and this component exposes no network entry point |
| C | microSD / USB mass storage | DEFERRED — no additional trust problem (the signature is the anchor either way), but it needs an SD stack and a file picker; the transport sits behind `kvm_ota_source_t`, so adding it later touches one file |
| D | Operator-side `idf.py flash` only, no OTA | REJECTED — TECH-051/052 require working OTA slots with rollback |

The TLS chain is verified (`esp_crt_bundle_attach`, or a pinned PEM through `update_transport_https_set_cert()`), never
skipped, and plain `http://` is refused in two places (`update_manager_begin_https()` and the transport itself). But TLS
is **not** the trust anchor: a compromised or misconfigured server still cannot produce an image the device will boot,
because the signature is checked after the write and before the boot switch. A response without `Content-Length`, or a
chunked one, is refused: without a declared length a truncated transfer is indistinguishable from a complete one.

## Decision

- **TECH-095 MUST — signing.** Release images are signed with the secure-boot-v2 RSA-3072 scheme and verified on update
  (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y`, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y`). No eFuse is
  programmed by firmware or by firmware tooling. Hardware Secure Boot v2 and Flash Encryption remain an operator
  decision under ADR-012/TECH-054, and this decision is designed so that enabling them later changes configuration, not
  code.
- **TECH-096 MUST — private key custody.** The signing key never enters the repository, the build directory inside the
  repository, a log or a backup that leaves the owner's control. The development key for TASK-110 lives at
  `O:\Praca\KVMpad\keys\kvmpad_ota_dev.pem` (outside the repository, workspace rule 2/3) and is *not* the release key.
  The release key is generated once with `espsecure generate-signing-key --version 2 --scheme rsa3072`, stored offline
  with a second copy in separate physical custody, and `CONFIG_SECURE_BOOT_SIGNING_KEY` points at a path outside the
  repository. Losing it ends the ability to ship updates to devices already in the field; leaking it ends the value of
  the whole scheme. A release build records the public key's SHA-256 digest in the test report, never the key file.
- **TECH-097 MUST — source.** The only v1 image source is a locally triggered HTTPS pull with a verified certificate
  chain and a declared length. The CLIENT protocol has no OTA, reset or exec command (TECH-045, AT-SEC-07), and
  `update_manager` exposes no entry point reachable from a network parser.
- **TECH-098 MUST — ordering.** The boot partition is switched exactly once per update, after the signature has
  verified, and the HID state is neutral before anything is written: revoke the remote session → maintenance
  (`RELEASE_ALL` with `KVM_RA_MAINTENANCE`) → wait for the neutral confirmation → write → verify → switch → reboot.
  Input is released only after the HID state is neutral again, and after a reboot nothing held is restored (TECH-053).
- **TECH-099 MUST — health check and rollback.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, so a fresh image boots as
  `ESP_OTA_IMG_PENDING_VERIFY`. Input stays blocked until the health check decides (AT-PERF-09). The image is confirmed
  with `esp_ota_mark_app_valid_cancel_rollback()` only after the USB stack, the HID scheduler, the LVGL owner task and
  the touch reader have each been seen alive on three consecutive sweeps within 20 s; otherwise
  `esp_ota_mark_app_invalid_rollback_and_reboot()` returns the device to the previous slot. A fresh image that comes up
  with HID state held is rolled back as well.
- **TECH-100 SHOULD — `ime_data` (design only; TECH-081).** The shared dictionary partition is updated through the same
  signed path and never through a separate downloader. `esp_ota_begin()`/`write`/`end` accept a data partition of
  subtype `data, undefined`, and with `CONFIG_SECURE_SIGNED_DATA_PARTITION=y` `esp_ota_end()` verifies a detached
  signature appended to the blob (`ota_verify_partition()` → `ota_verify_data_partition_signature()` in
  `components/app_update/esp_ota_ops.c`). Because both OTA slots read the same partition, a dictionary update is **not**
  atomic with respect to a rollback: the rule is that a dictionary is only ever replaced by one that the previous
  firmware can also read, and the IME treats a missing or unreadable dictionary as "language unavailable", never as a
  fatal error. The implementation is a separate task; this ADR fixes the mechanism and the constraint. Verified today:
  the configuration builds on the pinned tree.

### Required `sdkconfig.defaults` lines

Owned by the integrator, not by this task:

```ini
# ---- Signed OTA (TASK-110, ADR-031 TECH-095/099). No eFuse is programmed: hardware Secure Boot
# (CONFIG_SECURE_BOOT) stays off and is the operator's decision under ADR-012/TECH-054. -------------
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_SIGNING_KEY="../keys/kvmpad_ota_dev.pem"
# Dictionary partition updates through the same signed path (TECH-081, TECH-100)
CONFIG_SECURE_SIGNED_DATA_PARTITION=y
# PENDING_VERIFY on the first boot of a new image, so the health check can roll back (bootloader
# rebuild only, no eFuse — see Kconfig.app_rollback)
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
# CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK is deliberately NOT set: it burns the eFuse secure-version
# field. The anti-rollback comparison lives in components/update_manager instead.
```

`CONFIG_SECURE_BOOT_SIGNING_KEY` is evaluated relative to the project directory, so `../keys/…` resolves to the
workspace directory above the repository. Until these lines are added the firmware logs
`image signatures are NOT verified in this build` at every boot and `ota status` reports `signed ota NO`; a build in that
state is for bring-up only and the release checklist (`04 §13`, TASK-130) must reject it.

## Consequences

- An unsigned bring-up build keeps working, visibly degraded, so TASK-110 does not block M2–M7.
- Enabling the lines above makes the next wired flash mandatory: the running app must be signed before signature
  verification on update can work (see `docs/OTA.md`).
- Each image grows by one 4096-byte signature sector. `ota_0`/`ota_1` have 4 MiB each; there is no budget concern.
- Signature verification on update reads the whole staged image through SHA-256 plus an RSA-3072 PSS verification, so
  `esp_ota_end()` takes measurably longer than an unsigned update. To be measured on board #1 (`NOT_RUN_HW`).
- The release procedure gains two steps: sign with the release key on the release host, and record the public key digest
  in the report. TASK-130 owns the checklist.

## Follow-ups

- Owner decision on hardware Secure Boot v2 + Flash Encryption for production units, with the recovery and key-escrow
  plan ADR-012 requires. Not a prerequisite for TASK-110.
- `ime_data` update task (TECH-100), after the IME engines land.
- Wire `update_manager_console_cmd()` into `main/kvm_console.c` and the "Updating" state into the UI
  (`update_manager_get_status()` → the maintenance subject the UI already has).
- The board test plan in `docs/OTA.md` (AT-FAULT-08, at most 6 resets) is the gate for this ADR moving to ACCEPTED.
