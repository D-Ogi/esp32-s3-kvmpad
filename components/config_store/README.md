# `config_store`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** NVS: `schema_version`, CRC/validation, migrators, defaults, write debounce, pairing erase.

| | |
|---|---|
| Tasks | TASK-090 |
| Specification | 02 §9 (TECH-050) |
| Tests | AT-FAULT-02/03/10, AT-SEC-08 |

**Rules (MUST):**

- Do not write flash on every tap.
- No storing of keystroke streams.

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-017: cache of the AP BSSID + channel, without secrets (TECH-071).
- ADR-020: persistent Keep awake setting, OFF by default (FR-711).
- ADR-024: `kbd.host_layout` (an id from the generated list, default `us`) and `kbd.unicode_mode` (`linux` / `macos` /
  `windows-hex` / `wincompose` / `host-ime`); an unknown value → the default (FR-741/747).
- ADR-022: register of paired clients (name, SHA-256 of the token, date) — never the token itself; Remove/Remove all;
  factory reset clears it (FR-726, SEC-105).
- ADR-023: confirmation policies as NVS keys with safe defaults; an unknown/corrupted value → the default; a migration never
  relaxes a policy (FR-731/736). A `trusted` flag per paired client (FR-733).

Governing documents: `AGENTS.md`.
