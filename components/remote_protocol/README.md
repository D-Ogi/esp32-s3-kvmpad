# `remote_protocol`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** CLIENT protocol v1: envelope, versioning, parser, validation, session management.

| | |
|---|---|
| Tasks | TASK-100 |
| Specification | 03 §6 (6.1–6.6), §7; 02 §8 |
| Tests | AT-CONN-04..09, AT-SEC-01..08, AT-FAULT-06; fuzz in `test/fuzz/` |

**Rules (MUST):**

- **Blocked** until: a working `release_all` + epoch, USB HIL (05 §4). ADR-002 resolved (ADR-021: native Windows CLIENT, variant A).
- Message ≤ 1 KiB; raw JSON never reaches HID without validation.

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-018: media keys exclusively as `input/shortcut` with `command_id` (TECH-075).
- ADR-019: Type text is implemented by the CLIENT through the existing `keyboard.key` — **without** a new command in the protocol.
- ADR-022: new types `pair.hello/round1/round2/confirm/accepted/rejected` — whitelist, validation, fuzz (AT-EXT-17);
  outside an open pairing window, only `ERR_PAIRING_EXPIRED` (SEC-103).
- ADR-023: no `settings.*` exists in protocol v1; such frames → `ERR_BAD_FRAME` + counter; the snapshot does not contain policies (SEC-107).
- ADR-024: the status snapshot (03 §6.6) gains the read-only field `host_layout` — an enumerated id from the generated list,
  validated and fuzzed (AT-EXT-25); it is not settable from the CLIENT (FR-745, 03 §9 item 3).

Governing documents: `AGENTS.md`.
