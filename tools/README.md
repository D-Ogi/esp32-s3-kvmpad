# `tools/`

Helper scripts: flash, statistics export, checking of requirement IDs (02 §3.1).

- `verify_spec_manifest.py` — verifies the SHA256 sums of the v1.1.0 specification package at its target paths in the repository.
- `check_license_headers.py` (ADR-025) — verifies the three-line license header in every authored source file and that files
  with a foreign SPDX id are listed in `THIRD_PARTY_NOTICES.md`; `--fix` inserts missing headers, `--list` shows every verdict.
  Blocking in CI from TASK-000.
- `kbd_layouts/gen_layouts.py` (TASK-071, ADR-024) — generates `components/kbd_layouts/generated/` and the CLIENT layout tables
  from a pinned xkeyboard-config release and keycodemapdb commit. Runs on a Linux host (CI, WSL or a container — it needs
  libxkbcommon); the outputs are committed, CI regenerates them and requires an empty diff.

## Local tooling — never inside the repo

The tools live in the workspace directory **above** the repository (`../.tools/`), never inside the repo. A venv with esptool (v5) for hardware measurements:

```sh
python -m venv ../.tools/venv
../.tools/venv/Scripts/pip install esptool
../.tools/venv/Scripts/python -m esptool --chip esp32s3 --port COM3 flash-id
```

The board's UART port (CH343) reports itself as `USB-Enhanced-SERIAL CH343 (COMx)`.
