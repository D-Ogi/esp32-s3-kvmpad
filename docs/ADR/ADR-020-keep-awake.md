# ADR-020 — Keep-awake (mouse jiggler) as a limited exception to TECH-064
Status: ACCEPTED, PENDING-HW (effectiveness on the OS)
Date: 2026-11-01
Approver: project owner (2026-11-01)

## Context

- Prior art: mouse reports with 0 px movement prevent the target computer from going to sleep.
- The typical KVMpad use is a headless PC or server that should not fall asleep during work.
- **TECH-064:** "a USB device should not generate movement by itself without a fresh event". This feature is by definition an exception,
  so it must be explicit, narrow and visible.
- 03 §3 provides for `KVM_ORIGIN_SYSTEM`; 03 §5.3: no remote wakeup for a suspended HOST.

## Decision

- **FR-711 SHOULD:** a "Keep awake" toggle in SHORTCUTS/Settings, **OFF** by default; the setting is saved in `config_store`.
- **FR-712 MUST:** when the feature is enabled, the topbar permanently shows an indicator (text + icon, not color alone; FR-005).
- **FR-713 MUST:** a keep-alive report only when: HOST `MOUNTED` and not `SUSPENDED`; the pressed-ledger is empty (no key/button DOWN);
  no real input (LOCAL/REMOTE) for the interval (30 s by default, range 10–300 s; the shortest screen blanking
  timeout in Windows is 1 min, so 60 s would be borderline). Every real input resets the counter.
- **FR-714 MUST:** report = mouse with `dx=dy=0`, wheel 0, button mask 0. If AT-EXT-07 shows that 0 px does not reset idleness
  on the target OS, the fallback variant is a +1/−1 px pair (sum 0) — only through an update to this ADR.
- **TECH-076 MUST:** a new type `KVM_HID_KEEPALIVE` (at the end of the enum, without changing existing values) with `origin = KVM_ORIGIN_SYSTEM`,
  through `InputArbiter`. The arbiter rejects it when the ledger is non-empty, during a gesture, OTA/maintenance and an owner change.
  Keep-alive does not change the ledger and does not wake a suspended HOST.
- Keep-alive is a **new HID source** within the meaning of 03 §9 item 4. It therefore requires an update to the `CONTROL_OWNER`/arbiter model
  (an explicit `KVM_ORIGIN_SYSTEM` path without a lease, with the lowest priority) and concurrency tests with LOCAL/REMOTE (AT-EXT-09).
- The exception to TECH-064 applies **only** to this intent when the feature is enabled; apart from that, TECH-064 applies unchanged.

## Consequences

- The user is responsible for compliance with workplace policies (described in the UI/documentation).
- Zero cursor movement: no impact on the user's work.

## Tests

- AT-EXT-07 (HIL): Windows 11 and Linux with a short screen timeout → OFF: sleeps; ON: does not sleep.
- AT-EXT-08 (HIL): OFF → zero keep-alive reports in the trace; ON + a held key/drag → no keep-alive; HOST suspended → no reports.
- AT-EXT-09 (unit): the arbiter rejects `KVM_HID_KEEPALIVE` when the ledger is non-empty, during OTA and on an epoch change.
