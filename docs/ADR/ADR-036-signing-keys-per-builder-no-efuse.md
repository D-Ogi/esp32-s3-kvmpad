# ADR-036 — OTA signing keys: one per builder, kept offline; no eFuse is ever burned

Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner ("keep it offline; everyone who uses the repository generates their own key and signs their own
builds with it; we do not burn any eFuse")
Amends: ADR-012 (06 §13, production security scope), ADR-031 (key custody left open)

## Context

ADR-031 signs app images with an RSA-3072 key (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, no eFuse) and left the
production key custody to ADR-012. ADR-012 is the reference scope of the specification: Secure Boot v2, Flash
Encryption release mode and a per-device token, with eFuse writes only on the operator's decision and with a key
escrow plan.

KVMpad is an open, noncommercial project. Each person who builds it flashes their own boards. There is no central
vendor that ships signed images to devices it does not own.

## Decision

1. **No eFuse is burned, by any tool of this repository, in any release.** Secure Boot v2 and Flash Encryption are out of
   scope (ADR-012 is narrowed accordingly). `AGENTS.md` §2 item 6 already forbids it without the owner; this ADR makes
   the project's answer "never". Scripts, documentation and CI must not contain `espefuse burn_*` commands or enable
   `CONFIG_SECURE_BOOT` / `CONFIG_SECURE_FLASH_ENC_ENABLED`.
2. **One signing key per builder.** Whoever builds and flashes KVMpad generates their own RSA-3072 key
   (`espsecure generate-signing-key --version 2 --scheme rsa3072 <file>`), keeps it outside the repository and signs their
   own builds with it. The repository never contains a private key and never names a project-wide production key.
3. **Keys are kept offline.** The private key lives on removable or otherwise offline storage of its owner, with a backup
   copy. It is never committed, never written to logs, CI artifacts or chat, and never uploaded to a CI secret store.
   CI builds are unsigned or signed with a throwaway key generated in the job; such images are for testing only.
4. **Changing keys.** A device accepts an update only when it is signed with the key of the image it runs (ADR-031).
   Moving a board to another key, or recovering from a lost key, is done by flashing over the cable (`idf.py flash`),
   which needs physical access and no eFuse. The `ime_data` partition and NVS are unaffected.

## Consequences

- Physical access to a board allows reflashing it with any image; signed OTA protects the network path only. This is
  stated in the security documentation and is accepted for this product: the device sits on the owner's desk.
- A lost key costs a cable flash, never a dead device. A leaked key lets an attacker on the network offer an update to
  boards that trust that key; the owner re-keys by cable.
- The release gates keep signed-OTA verification (AT-SEC-09, AT-FAULT-08 with the builder's key); AT tests that need
  Secure Boot or Flash Encryption are N/A.
- The development key of board #1 (`../keys/kvmpad_ota_dev.pem`, outside the repository) stays as it is; it is the
  owner's key in the sense of item 2.
