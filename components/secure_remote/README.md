# `secure_remote`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** TLS/WSS, pairing with physical acceptance, permissions, leases.

| | |
|---|---|
| Tasks | TASK-100 |
| Specification | 02 §8, §8.1; ADR-002, ADR-012 |
| Tests | AT-SEC-*, AT-CONN-* |

**Rules (MUST):**

- No "temporary ws://" in the release build.
- No secrets/tokens in the repository or in logs; per-device keys (TECH-056).
- Nobody changes ADR-002/012 without the owner's consent. ADR-002 was resolved by ADR-021: CLIENT = a native Windows application
  with pinning of the device certificate established during physical pairing; a browser CLIENT is outside v1.
- ADR-022: pairing — mDNS `_kvmpad._tcp`, a 6-digit one-time code (120 s; the whole window ≤ 180 s, a compile-time constant), PAKE (candidate EC-JPAKE P-256 from mbedTLS)
  with channel binding to the TLS certificate, Accept/Reject on the DEVICE, 3 attempts/window + lockout (FR-721..727, SEC-101..106).
  Unverified TLS **only** for `pair.*` within an open window — never status, HID or tokens.
- ADR-023: `control.approval` (always/trusted/auto) and `pair.require_accept` are read from `config_store`; no CLIENT changes
  the settings (SEC-107); local priority (ADR-011), lease and epoch work independently of the policy.

Governing documents: `AGENTS.md`.
