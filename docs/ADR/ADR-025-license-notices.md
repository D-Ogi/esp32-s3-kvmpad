# ADR-025 — License notices in every source file and in the shipped binaries
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02: the existing one-line SPDX rule is too weak; the notice must travel with every file and with the firmware)

## Context

- The project is source-available under PolyForm Noncommercial 1.0.0. Its *Notices* clause requires that anyone who receives
  **any part** of the software also receives the license terms (or their URL) and every plain-text line beginning with
  `Required Notice:` that the licensor provided. Our Required Notice is
  `Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)` (`LICENSE.md`, `README.md`).
- Until now `CONTRIBUTING.md` asked only for `// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0` in new source files.
  Nothing verified it, the only existing script lacked it, the PR template did not mention it, and the Required Notice lived
  only in `LICENSE.md` and `README.md`. A copied directory or a single file carried no notice at all.
- A license cannot technically stop a commercial clone. What it can do is make the terms unmissable at every granularity
  (file, directory, binary, screen), remove the "I did not know" defense and make a takedown request on a hosting platform
  straightforward. That is the goal of this decision; a trademark on the name is a separate, business-level matter.
- Third-party code (ESP-IDF, LVGL, TinyUSB, xkeyboard-config data, AOSP IME engines, Noto fonts) keeps its own license and must
  not receive our header; the attribution file `THIRD_PARTY_NOTICES.md` is referenced by ADR-024 and `CONTRIBUTING.md` but did
  not exist yet.

## Options

| | Option | Assessment |
|---|---|---|
| A | Keep the one-line SPDX header, rely on `LICENSE.md` | REJECTED — the Required Notice does not travel with a file; no enforcement |
| B | Three-line header (SPDX + copyright + Required Notice) in every authored source file, a checker in CI and in the PR checklist, the notice embedded in the firmware image, the CLIENT binary and an About screen | **CHOSEN** |
| C | Full license text (or a long summary) at the top of every file | REJECTED — noise in every diff, no additional legal effect over B |

## Decision

- **TECH-082 MUST — license header.** Every authored source file starts with these three lines, in the comment syntax of the file:

  ```c
  // SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
  // Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
  // Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
  ```

  `//` for C/C++/C#/JavaScript/TypeScript/Kconfig-free headers; `#` for Python, CMake, YAML, shell, PowerShell, TOML,
  `sdkconfig.defaults`, `partitions.csv`, linker fragments; `<!-- … -->` for HTML/XML/SVG. The header follows a shebang and an
  encoding line when present; a Python docstring comes after it. Generated files receive the same header from their generator
  together with a provenance line (ADR-024 TECH-077).
  Exempt: `docs/spec/**`, `reference/**`, `AGENTS*.md` (frozen package), Markdown documentation (covered by `LICENSE.md`),
  JSON and lock files (no comment syntax: `*.json`, `dependencies.lock`), binary assets, dotfiles (`.gitattributes`,
  `.editorconfig`, `.gitignore`), `LICENSE.md`, `THIRD_PARTY_NOTICES.md`, and every path listed in `THIRD_PARTY_NOTICES.md`.
- **TECH-083 MUST — enforcement.** `tools/check_license_headers.py` verifies the header (and that a file with a foreign SPDX
  identifier is listed in `THIRD_PARTY_NOTICES.md`); `--fix` inserts the header into `//`/`#` files. It runs in CI from TASK-000
  (blocking) and is a line in the PR checklist. Third-party files keep their original header untouched and are listed in
  `THIRD_PARTY_NOTICES.md` with path, upstream, version/commit and license.
- **TECH-084 MUST — notice in the binaries.** The firmware image contains the Required Notice and the SPDX identifier as a
  retained string (`main/`, `.rodata`, `__attribute__((used))`) so that `strings app.bin` finds it in every OTA image; the
  Windows CLIENT carries it in the version resource (`LegalCopyright`) and in its About dialog.
- **FR-751 MUST — About screen.** `Settings → About` shows the firmware version, the license name with the Required Notice and a
  scrollable list of third-party notices generated from `THIRD_PARTY_NOTICES.md` at build time (no hand-maintained copy).
  It is a plain view: no HID, no network, legible at 800×480 (FR-604).

## Consequences

- Every new file in the repository carries the notice; a copied fragment carries it too. The checker makes a missing header a
  build failure, not a review nitpick.
- `THIRD_PARTY_NOTICES.md` becomes the single attribution list; the About screen renders it, so attribution for shipped
  third-party code is satisfied on the device as well as in the repository.
- Nothing here restricts the permitted noncommercial uses listed in `README.md`; the author alone keeps the right to license
  commercially (`CONTRIBUTING.md`).

## Tests

- **AT-EXT-28 (CI):** `python -I tools/check_license_headers.py` reports 0 missing / 0 unlisted third-party files on every PR.
- **AT-EXT-29 (SDL + HIL):** `strings` on the built `app.bin` contains `Required Notice: Copyright 2026 D-Ogi`; the About screen
  shows the notice and the third-party list on the SDL simulator and on board #1 (`NOT_RUN_HW` until the UI milestone).

## Migration

- 2026-11-02: header added to `tools/verify_spec_manifest.py`; checker and `THIRD_PARTY_NOTICES.md` created; `CONTRIBUTING.md`,
  the PR template and `README.md` updated. The frozen package is not touched (its license is stated in `docs/spec/README.md`
  and `LICENSE.md`).
- TASK-000 wires the checker into CI and adds the retained string in `main/`; TASK-070C adds the About screen; TASK-105 the
  CLIENT resource.
