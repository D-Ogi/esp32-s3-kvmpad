#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the SHA256 manifest of the KVMpad specification package (v1.1.0, English edition).

The public documents are in docs/spec and the design references are in reference.
Locally available engineering documents are verified against the same manifest.
Use --full-package to require all files from the original distribution.
"""
from __future__ import annotations

import hashlib
import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = REPO / "docs" / "spec" / "MANIFEST-SHA256.md"
LOCAL_DOCUMENTS = {
    "05-IMPLEMENTATION-PLAN.md", "06-DECISIONS-AND-RISKS.md",
    "AGENTS.md", "AGENTS-UI.md", "README.md",
}
LINE = re.compile(r"^([0-9a-f]{64})\s+(\S.*)$")


def resolve(name: str) -> pathlib.Path | None:
    base = REPO if name.startswith(("AGENTS", "reference/")) else MANIFEST.parent
    candidate = base / name
    return candidate if candidate.is_file() else None



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full-package", action="store_true", help="require all original package files")
    args = parser.parse_args()
    entries = [m.groups() for m in map(LINE.match, MANIFEST.read_text(encoding="utf-8").splitlines()) if m]
    if not entries:
        print(f"FAIL: no entries parsed from {MANIFEST}")
        return 1

    failures = 0
    checked = 0
    for expected, name in entries:
        path = resolve(name)
        if path is None and name in LOCAL_DOCUMENTS and not args.full_package:
            print(f"LOCAL    {name}")
            continue
        checked += 1
        if path is None:
            print(f"MISSING  {name}")
            failures += 1
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        ok = actual == expected
        failures += not ok
        print(f"{'OK      ' if ok else 'MISMATCH'} {path.relative_to(REPO).as_posix()}")

    print(f"\n{checked - failures}/{checked} files verified")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
