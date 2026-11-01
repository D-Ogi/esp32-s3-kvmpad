# `tools/`

Helper scripts: flash, statistics export, checking of requirement IDs (02 §3.1).

- `verify_spec_manifest.py` — verifies the SHA256 sums of the v1.1.0 specification package at its target paths in the repository.

## Local tooling — never inside the repo

The tools live in the workspace directory **above** the repository (`../.tools/`), never inside the repo. A venv with esptool (v5) for hardware measurements:

```sh
python -m venv ../.tools/venv
../.tools/venv/Scripts/pip install esptool
../.tools/venv/Scripts/python -m esptool --chip esp32s3 --port COM3 flash-id
```

The board's UART port (CH343) reports itself as `USB-Enhanced-SERIAL CH343 (COMx)`.
