#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Turn THIRD_PARTY_NOTICES.md into the C string that `Settings -> About` renders (ADR-025 FR-751).

The repository file stays the human-readable record; this script is the only place that converts it, so the
screen can never drift from it. It is run from CMake at configure time (components/ui_views/notices.cmake) and
writes two files into the build directory:

    ui_notices_generated.h   const char *ui_notices_third_party(void);
                             const char *ui_notices_license(void);
    ui_notices_generated.c   the text, as one C string literal per output line

Usage:
    python -I tools/gen_third_party_notices.py --input THIRD_PARTY_NOTICES.md --out-dir <build dir>
    python -I tools/gen_third_party_notices.py --input ... --out-dir ... --print   # the rendered text only

Rendering rules (deliberately dull, so the output is reproducible byte for byte):
  * the document title (`# ...`) is dropped - the About pane carries its own heading;
  * `## Section` becomes a blank line and the bare section name;
  * a Markdown table becomes one line per data row: the non-empty cells joined with " - ";
    the header row, the `---` separator and placeholder rows (`_none yet_`) are skipped;
  * consecutive prose lines are joined into one paragraph (LVGL wraps it on screen);
  * inline Markdown is reduced to text: backticks, `**bold**`, `_italic_`, `[text](url)` -> `text (url)`.
No timestamp, no path and no host name reaches the output (reproducible build, TECH-083).
"""

from __future__ import annotations

import argparse
import os
import re
import sys

HEADER_C = (
    "// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0",
    "// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.",
    "// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)",
)

# The three lines of ADR-025, as `Settings -> About` shows them (FR-751). Kept here so the screen, the file
# headers and the string embedded in the image (TECH-084) all state the same thing.
LICENSE_TEXT = (
    "SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0\n"
    "Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.\n"
    "Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)"
)

PLACEHOLDER_CELLS = {"none yet", "-", ""}

# The About pane renders the text with the built-in Montserrat subsets, which hold ASCII and the LVGL
# symbols only. The Markdown record is written for people and uses typographic characters, so they are
# folded to ASCII here; anything unmapped becomes "?" and is reported, never silently dropped.
ASCII_FOLD = {
    "→": "->", "←": "<-", "↔": "<->",
    "–": "-", "—": "-", "−": "-",
    "‘": "'", "’": "'", "“": '"', "”": '"',
    "·": "-", "•": "-", "…": "...", " ": " ",
    "≤": "<=", "≥": ">=", "×": "x", "©": "(c)",
}

LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
BOLD_RE = re.compile(r"\*\*([^*]+)\*\*")
ITALIC_RE = re.compile(r"(?<![A-Za-z0-9_])_([^_]+)_(?![A-Za-z0-9_])")
SEPARATOR_RE = re.compile(r"^\|[\s|:-]+\|$")


def to_ascii(s: str, reported: set[str]) -> str:
    """Fold the typographic characters of the record to ASCII; report whatever has no mapping."""
    out: list[str] = []
    for ch in s:
        if ord(ch) < 128:
            out.append(ch)
        elif ch in ASCII_FOLD:
            out.append(ASCII_FOLD[ch])
        else:
            out.append("?")
            reported.add(ch)
    return "".join(out)


def inline_text(s: str, reported: set[str] | None = None) -> str:
    """Reduce inline Markdown to plain ASCII text."""
    s = LINK_RE.sub(r"\1 (\2)", s)
    s = BOLD_RE.sub(r"\1", s)
    s = ITALIC_RE.sub(r"\1", s)
    s = s.replace("`", "")
    return to_ascii(s, reported if reported is not None else set()).strip()


def split_row(line: str, reported: set[str]) -> list[str]:
    body = line.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|"):
        body = body[:-1]
    return [inline_text(cell, reported) for cell in body.split("|")]


def render(markdown: str, reported: set[str] | None = None) -> str:
    """The plain ASCII text the About pane renders. Deterministic for a given input."""
    if reported is None:
        reported = set()
    out: list[str] = []
    paragraph: list[str] = []
    in_table = False
    table_row = 0

    def flush_paragraph() -> None:
        if paragraph:
            out.append(" ".join(paragraph))
            paragraph.clear()

    for raw in markdown.splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if stripped.startswith("|"):
            flush_paragraph()
            if not in_table:
                in_table = True
                table_row = 0
            table_row += 1
            if table_row == 1 or SEPARATOR_RE.match(stripped):
                continue  # header row and the --- separator carry no notice
            cells = [c for c in split_row(stripped, reported) if c.lower() not in PLACEHOLDER_CELLS]
            if cells:
                out.append(" - ".join(cells))
            continue
        in_table = False
        if not stripped:
            flush_paragraph()
            continue
        if stripped.startswith("#"):
            flush_paragraph()
            level = len(stripped) - len(stripped.lstrip("#"))
            title = inline_text(stripped[level:], reported)
            if level == 1:
                continue  # the document title; the About pane has its own heading
            if out:
                out.append("")
            out.append(title)
            continue
        paragraph.append(inline_text(stripped, reported))
    flush_paragraph()

    # Drop a leading or trailing blank line so the label starts and ends on text.
    while out and out[0] == "":
        out.pop(0)
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out)


def c_literal(line: str, last: bool) -> str:
    """One C string literal for `line`; every line but the last carries the newline that follows it."""
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    return f'    "{escaped}' + ('"' if last else '\\n"')


def c_array(name: str, text: str) -> list[str]:
    """`static const char <name>[] = ...;` as one literal per line of `text`."""
    rows = text.split("\n")
    body = [c_literal(row, i + 1 == len(rows)) for i, row in enumerate(rows)]
    body[-1] = body[-1] + ";"
    return [f"static const char {name}[] =", *body]


def emit_c(text: str, license_text: str, input_name: str) -> str:
    lines = [*HEADER_C, "//"]
    lines.append(f"// Generated from {input_name} by tools/gen_third_party_notices.py (ADR-025 FR-751).")
    lines.append("// Do not edit: change the Markdown file and rebuild.")
    lines.append('#include "ui_notices_generated.h"')
    lines.append("")
    lines.extend(c_array("k_third_party", text))
    lines.append("")
    lines.extend(c_array("k_license", license_text))
    lines.append("")
    lines.append("const char *ui_notices_third_party(void)")
    lines.append("{")
    lines.append("    return k_third_party;")
    lines.append("}")
    lines.append("")
    lines.append("const char *ui_notices_license(void)")
    lines.append("{")
    lines.append("    return k_license;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def emit_h(input_name: str) -> str:
    lines = [*HEADER_C, "//"]
    lines.append(f"// Generated from {input_name} by tools/gen_third_party_notices.py (ADR-025 FR-751).")
    lines.append("// Do not edit: change the Markdown file and rebuild.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append('extern "C" {')
    lines.append("#endif")
    lines.append("")
    lines.append("/** Third-party notices as plain text, rendered by `Settings -> About` (FR-751). */")
    lines.append("const char *ui_notices_third_party(void);")
    lines.append("")
    lines.append("/** The three ADR-025 license lines of this build (SPDX id, copyright, Required Notice). */")
    lines.append("const char *ui_notices_license(void);")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def write_if_changed(path: str, content: str) -> bool:
    """Write only on a real change, so a reconfigure does not force a rebuild of everything."""
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", newline="") as f:
            if f.read() == content:
                return False
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    return True


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate the About notices string from THIRD_PARTY_NOTICES.md")
    ap.add_argument("--input", required=True, help="path to THIRD_PARTY_NOTICES.md")
    ap.add_argument("--out-dir", required=True, help="directory for ui_notices_generated.{c,h}")
    ap.add_argument("--print", action="store_true", dest="print_text", help="print the rendered text and exit")
    args = ap.parse_args(argv)

    try:
        with open(args.input, "r", encoding="utf-8") as f:
            markdown = f.read()
    except OSError as exc:
        print(f"gen_third_party_notices: cannot read {args.input}: {exc}", file=sys.stderr)
        return 2

    unmapped: set[str] = set()
    text = render(markdown, unmapped)
    for ch in sorted(unmapped):
        print(f"gen_third_party_notices: U+{ord(ch):04X} has no ASCII mapping; rendered as '?'",
              file=sys.stderr)
    if not text:
        print(f"gen_third_party_notices: {args.input} rendered to nothing", file=sys.stderr)
        return 2
    if args.print_text:
        print(text)
        return 0

    os.makedirs(args.out_dir, exist_ok=True)
    name = os.path.basename(args.input)
    changed = write_if_changed(os.path.join(args.out_dir, "ui_notices_generated.h"), emit_h(name))
    changed = write_if_changed(os.path.join(args.out_dir, "ui_notices_generated.c"),
                               emit_c(text, LICENSE_TEXT, name)) or changed
    print(f"gen_third_party_notices: {len(text)} characters from {name}"
          f"{'' if changed else ' (unchanged)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
