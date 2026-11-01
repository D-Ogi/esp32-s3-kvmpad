# Working in the KVMpad repository

## Where to start

1. `AGENTS.md` and `AGENTS-UI.md` — the working contract that binds **every** contributor (prohibitions, checklists, report format).
2. `docs/spec/README.md` — reading order of the specification and the precedence rule between documents.
3. `docs/REPO-LAYOUT.md` — directory map, dependency rules, repository boundary, known gaps.
4. `docs/ADR/README.md` and `docs/EXTENSIONS.md` — decisions and requirements added **after** the specification was frozen.

## Language

Everything in this repository is written in **English** (American spelling): code, comments, documentation, the specification,
ADRs, test reports, commit messages, pull requests. The specification package v1.1.0 in `docs/spec/` is the English edition
of the original; it is normative and frozen (see Rules).

## Path mapping

The `AGENTS*.md` documents refer to specification files without a directory. In this repository:

- `01-…` … `07-…` and the specification `README.md` → `docs/spec/`
- `reference/…` → `reference/` (root)
- new ADRs → `docs/ADR/` (ADR-001..013 live in `docs/spec/06-DECISIONS-AND-RISKS.md`)

## Decisions that override the specification (summary; full text in `docs/ADR/`)

| ADR | Decision |
|---|---|
| 014 | No UI generators (EEZ Studio, LVGL Pro Editor). TASK-061 cancelled, AT-UIFW-06 = N/A. UI = LVGL 9.6 + Observer + KVMpad UI Core, hand-written C |
| 017 | Wi-Fi: modem sleep off (after measurement), fast reconnect from cached BSSID/channel |
| 018 | Consumer Control as a 3rd HID interface; `AC Pan` only in report protocol. ESP32-S3 has 4 usable IN endpoints → no CDC on the OTG port |
| 019 | "Type text" only on the CLIENT side, through the existing `keyboard.key` |
| 020 | Keep-awake — the only permitted exception to TECH-064, default OFF |
| 021 | ADR-002 → variant A: the only v1 CLIENT is a native Windows application with certificate pinning |
| 022 | Pairing: mDNS + 6-digit code + PAKE; the user types in only 6 digits; the whole window ≤ 180 s |
| 023 | Approval policies (accept on pairing, control approval, Alt+F4 confirmation) configurable locally in Settings; hard security invariants have no toggle; the CLIENT cannot change settings |
| 024 | Keyboard layouts generated from xkeyboard-config ("Host layout" mirroring: legends, ANSI/ISO/JIS/ABNT2/KR geometry, IME keys); on-device IME for Japanese / Simplified Chinese / Korean in the main release, Unicode delivered through per-OS input modes |
| 025 | Three-line license header (SPDX + copyright + Required Notice) in every authored source file, verified by `tools/check_license_headers.py` in CI; the notice embedded in the firmware image and the CLIENT binary; `Settings → About` with third-party notices |

Treat the new requirement IDs (`FR-311`, `FR-7xx`, `TECH-07x`, `SEC-10x`) and test IDs (`AT-EXT-xx`) as part of the specification.

## Rules

- `docs/spec/**`, `AGENTS.md`, `AGENTS-UI.md`, `reference/**` are **frozen** (SHA256 manifest). Do not edit them —
  record changes as ADRs. Check: `python -I tools/verify_spec_manifest.py` must report 12/12.
- `CMakeLists.txt`, `idf_component.yml`, `dependencies.lock`, `sdkconfig.defaults` are created only in TASK-000 from a working
  `idf.py build` (ESP-IDF v6.1). Do not create them without a build.
- One PR = one `TASK-xxx`. Describe the PR with the template (`.github/pull_request_template.md`): requirement IDs
  (`FR-/TECH-/SEC-`), test IDs (`AT-`) and the commands that were **actually** run. Without a physical board, hardware results
  are always `NOT_RUN_HW`.
- **License header (ADR-025).** Every authored source file — C/C++/C#, Python, CMake, YAML, shell, PowerShell, HTML/XML,
  `sdkconfig.defaults`, `partitions.csv`, generated files included — starts with these three lines in the file's comment syntax
  (after a shebang, if any):

  ```c
  // SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
  // Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
  // Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
  ```

  Exempt: the frozen package, Markdown, JSON/lock files, binary assets, dotfiles. Third-party code keeps its original header
  untouched and is listed in `THIRD_PARTY_NOTICES.md` (path, upstream, version, license). Check before every PR:
  `python -I tools/check_license_headers.py` must report 0 missing and 0 unlisted (`--fix` inserts missing headers).
  The firmware and the CLIENT embed the same notice (TECH-084) and show it in About (FR-751).
- Install tooling (virtual environments, esptool, ESP-IDF) in the workspace directory **above** the repository (`../.tools/`),
  never inside the repository.
- Raw logs, screenshots and packet captures go to `../evidence/` outside the repository; the records (`hardware/boards.md`,
  `test/reports/`) keep the key lines and the SHA256 of each file. Logs never contain typed keystrokes, tokens or pairing secrets.
- Hardware: no `burn_*` operations on eFuses, no secure boot / flash encryption without the owner's decision and a recovery plan.

## Contribution license

The project is released under PolyForm Noncommercial 1.0.0 and the author (D-Ogi) alone retains the right to license it
commercially. By submitting a contribution (pull request, patch) you grant the author a non-exclusive, perpetual license
to use, modify and relicense it — including commercially — and you confirm that you are entitled to do so.
Contributions without this consent cannot be accepted.
