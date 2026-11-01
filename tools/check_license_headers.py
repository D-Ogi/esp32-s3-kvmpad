#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check (or insert) the KVMpad license header in every authored source file (ADR-025, TECH-082/083).

Usage:
    python -I tools/check_license_headers.py            # report, exit 1 on any finding
    python -I tools/check_license_headers.py --fix      # insert the header where it is missing (// and # files)
    python -I tools/check_license_headers.py --list     # print every checked file with its verdict

Rules (ADR-025):
  * the first comment lines of a file (after an optional shebang / encoding line) are the three header lines;
  * files that carry a different SPDX identifier are third-party and must be listed in THIRD_PARTY_NOTICES.md;
  * the frozen specification package, Markdown documentation, JSON / lock files, binary assets and dotfiles are exempt.
The checker needs only the standard library and runs the same way on Windows, Linux and in CI.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import subprocess
import sys

SPDX_ID = "PolyForm-Noncommercial-1.0.0"
HEADER_LINES = (
    f"SPDX-License-Identifier: {SPDX_ID}",
    "Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.",
    "Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)",
)

# comment style by extension / file name
SLASH = {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".cs", ".js", ".mjs", ".ts", ".tsx", ".jsx", ".java", ".kt", ".rs", ".go"}
HASH = {".py", ".pyi", ".cmake", ".yml", ".yaml", ".sh", ".bash", ".ps1", ".psm1", ".toml", ".csv", ".lf", ".ini", ".cfg",
        ".mk", ".txt_cmake"}
HASH_NAMES = {"CMakeLists.txt", "Kconfig", "Kconfig.projbuild", "Makefile", "sdkconfig.defaults", "partitions.csv",
              "requirements.txt"}
XML = {".html", ".htm", ".xml", ".svg", ".xaml", ".csproj", ".props", ".targets", ".resx", ".manifest"}

EXEMPT_GLOBS = (
    "docs/spec/*", "reference/*", "AGENTS.md", "AGENTS-UI.md",
    "LICENSE.md", "THIRD_PARTY_NOTICES.md",
    "*.md", "*.json", "*.lock", "dependencies.lock",
    "*.png", "*.jpg", "*.jpeg", "*.gif", "*.ico", "*.bin", "*.pdf", "*.ttf", "*.otf", "*.woff", "*.woff2", "*.zip",
    ".gitattributes", ".gitignore", ".editorconfig", ".gitmodules",
    "sdkconfig", "sdkconfig.old", "build/*", "managed_components/*",
)

SPDX_RE = re.compile(r"SPDX-License-Identifier:\s*([A-Za-z0-9.+\-]+(?:\s+(?:OR|AND|WITH)\s+[A-Za-z0-9.+\-]+)*)")
SHEBANG_RE = re.compile(r"^#!")
ENCODING_RE = re.compile(r"^#.*coding[:=]")


def repo_root() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(here)


def tracked_files(root: str) -> list[str]:
    """Files under version control plus untracked-but-not-ignored ones (so a new file is checked before it is added)."""
    try:
        out = subprocess.run(["git", "-C", root, "ls-files", "-co", "--exclude-standard", "-z"],
                             capture_output=True, check=True).stdout
        files = [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]
    except (subprocess.CalledProcessError, FileNotFoundError):
        files = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in {".git", "build", "managed_components"}]
            for name in filenames:
                files.append(os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/"))
    return sorted(f.replace(os.sep, "/") for f in files)


def third_party_paths(root: str) -> list[str]:
    """Path globs from the first column of the tables in THIRD_PARTY_NOTICES.md (code spans)."""
    path = os.path.join(root, "THIRD_PARTY_NOTICES.md")
    globs: list[str] = []
    if not os.path.exists(path):
        return globs
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("|"):
                continue
            first = line.split("|")[1].strip()
            m = re.match(r"`([^`]+)`", first)
            if m and m.group(1) not in {"Path", "path"}:
                globs.append(m.group(1))
    return globs


def style_of(rel: str) -> str | None:
    name = os.path.basename(rel)
    ext = os.path.splitext(name)[1].lower()
    if name in HASH_NAMES:
        return "#"
    if ext in SLASH:
        return "//"
    if ext in HASH:
        return "#"
    if ext in XML:
        return "xml"
    return None


def matches_any(rel: str, globs) -> bool:
    for g in globs:
        if fnmatch.fnmatch(rel, g) or fnmatch.fnmatch(os.path.basename(rel), g):
            return True
        if g.endswith("/*") and rel.startswith(g[:-1]):
            return True
    return False


def read_head(path: str, n: int = 12) -> list[str]:
    try:
        with open(path, encoding="utf-8-sig") as fh:
            return [next(fh).rstrip("\r\n") for _ in range(n)]
    except StopIteration:
        with open(path, encoding="utf-8-sig") as fh:
            return fh.read().splitlines()
    except (UnicodeDecodeError, OSError):
        return []


def header_present(lines: list[str]) -> bool:
    text = "\n".join(lines)
    return all(h in text for h in HEADER_LINES)


def foreign_spdx(lines: list[str]) -> str | None:
    for line in lines:
        m = SPDX_RE.search(line)
        if m and m.group(1) != SPDX_ID:
            return m.group(1)
    return None


def insert_header(path: str, style: str) -> bool:
    if style == "xml":
        return False
    with open(path, encoding="utf-8-sig", newline="") as fh:
        content = fh.read()
    nl = "\r\n" if "\r\n" in content else "\n"
    lines = content.split(nl)
    idx = 0
    if lines and SHEBANG_RE.match(lines[0]):
        idx = 1
    if style == "#" and len(lines) > idx and ENCODING_RE.match(lines[idx]):
        idx += 1
    header = [f"{style} {h}" for h in HEADER_LINES]
    new_lines = lines[:idx] + header + lines[idx:]
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(nl.join(new_lines))
    return True


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fix", action="store_true", help="insert the header where it is missing (// and # files only)")
    ap.add_argument("--list", action="store_true", help="print every checked file with its verdict")
    ap.add_argument("--root", default=repo_root(), help="repository root (default: parent of tools/)")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    tp_globs = third_party_paths(root)
    missing: list[str] = []
    unlisted: list[str] = []
    fixed: list[str] = []
    checked = 0
    for rel in tracked_files(root):
        if matches_any(rel, EXEMPT_GLOBS):
            continue
        style = style_of(rel)
        if style is None:
            continue
        full = os.path.join(root, rel)
        if not os.path.isfile(full):
            continue
        checked += 1
        head = read_head(full)
        verdict = "ok"
        if header_present(head):
            pass
        elif (fid := foreign_spdx(head)) is not None:
            if matches_any(rel, tp_globs):
                verdict = f"third-party ({fid}, listed)"
            else:
                verdict = f"THIRD-PARTY NOT LISTED ({fid})"
                unlisted.append(f"{rel}: SPDX {fid} but not in THIRD_PARTY_NOTICES.md")
        elif matches_any(rel, tp_globs):
            verdict = "third-party (listed)"
        else:
            if args.fix and insert_header(full, style):
                verdict = "FIXED"
                fixed.append(rel)
            else:
                verdict = "MISSING"
                missing.append(rel)
        if args.list:
            print(f"{verdict:36} {rel}")

    for rel in missing:
        print(f"MISSING header: {rel}")
    for item in unlisted:
        print(f"UNLISTED third-party: {item}")
    for rel in fixed:
        print(f"FIXED: {rel}")
    print(f"{checked} files checked, {len(missing)} missing, {len(unlisted)} unlisted third-party, {len(fixed)} fixed")
    return 1 if (missing or unlisted) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
