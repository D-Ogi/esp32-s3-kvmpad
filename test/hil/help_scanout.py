# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check a complete 'ui help-cycle N' UART trace from the 7B diagnostic image."""
import argparse
import json
from pathlib import Path
import re


EDGE = re.compile(
    r"^UI_HELP cycle=(\d+) edge=(idle|open|close) reached=(\d+) vsync=(\d+) "
    r"frames=(\d+) missed=(-?\d+) bounce=(\d+) hold_us=(\d+) internal_free=(\d+)", re.M)
RESULT = re.compile(r"^UI_HELP result=(PASS|MISSED) cycles=(\d+)", re.M)
STAGES = ("idle", "open", "close")


def summarize(trace, cycles):
    if not 1 <= cycles <= 20:
        raise ValueError("cycles must be in 1..20")
    entries = EDGE.findall(trace)
    final = RESULT.findall(trace)
    if len(entries) != cycles * len(STAGES) or len(final) != 1 or int(final[0][1]) != cycles:
        raise ValueError("Incomplete or duplicate run")
    if re.search(r"BOOT_OK|Guru Meditation|stack overflow|Rebooting\.\.\.", trace, re.I):
        raise ValueError("Device restarted during measurement")
    bounce = int(entries[0][6])
    summary = {s: dict(vsync=0, frames=0, missed=0, disturbed_edges=0) for s in STAGES}
    clean = True
    for index, (cycle, stage, reached, vsync, frames, missed, lines, hold, free) in enumerate(entries):
        vsync, frames, missed, hold = map(int, (vsync, frames, missed, hold))
        if (int(cycle) != index // 3 + 1 or stage != STAGES[index % 3] or reached != "1"
                or int(lines) != bounce or int(free) <= 0 or min(vsync, frames) <= 0
                or vsync - frames != missed or (hold != 0 if stage == "idle" else hold < 90000)):
            raise ValueError(f"Invalid measurements at cycle {cycle}, {stage}")
        stats = summary[stage]
        stats["vsync"] += vsync
        stats["frames"] += frames
        stats["missed"] += missed
        stats["disturbed_edges"] += int(missed != 0)
        clean = clean and missed == 0
    if (final[0][0] == "PASS") != clean:
        raise ValueError("Device summary differs from the individual measurements")
    return dict(cycles=cycles, bounce_lines=bounce, result=final[0][0], stages=summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--cycles", type=int, required=True)
    args = parser.parse_args()
    result = summarize(args.trace.read_text(encoding="utf-8-sig"), args.cycles)
    print(json.dumps(result, indent=2))
    return int(result["result"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
