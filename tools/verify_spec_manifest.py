#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the SHA256 manifest of the KVMpad specification package (v1.1.0, English edition).

The package lives in several repository locations, so the manifest's flat paths
are resolved against a fixed search list:

    docs/spec/<name>   - documents 01..07 and the package README
    <repo>/<name>      - AGENTS.md, AGENTS-UI.md (agent contracts at repo root)
    <repo>/<name>      - reference/* (UX mockup at repo root)

Files are hashed byte-for-byte (LF line endings, UTF-8 without BOM). Any edit to
the package must go through an ADR and a regenerated manifest.

Usage:  python -I tools/verify_spec_manifest.py
Exit code 0 = all files match, 1 = mismatch or missing file.
"""
from __future__ import annotations

import hashlib
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = REPO / "docs" / "spec" / "MANIFEST-SHA256.md"
SEARCH_DIRS = (REPO / "docs" / "spec", REPO)
LINE = re.compile(r"^([0-9a-f]{64})\s+(\S.*)$")


def resolve(name: str) -> pathlib.Path | None:
    for base in SEARCH_DIRS:
        candidate = base / name
        if candidate.is_file():
            return candidate
    return None


def main() -> int:
    entries = [m.groups() for m in map(LINE.match, MANIFEST.read_text(encoding="utf-8").splitlines()) if m]
    if not entries:
        print(f"FAIL: no entries parsed from {MANIFEST}")
        return 1

    failures = 0
    for expected, name in entries:
        path = resolve(name)
        if path is None:
            print(f"MISSING  {name}")
            failures += 1
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        ok = actual == expected
        failures += not ok
        print(f"{'OK      ' if ok else 'MISMATCH'} {path.relative_to(REPO).as_posix()}")

    print(f"\n{len(entries) - failures}/{len(entries)} files verified")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
