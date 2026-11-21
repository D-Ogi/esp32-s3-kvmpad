# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Sample completed magnifier frames from the running release over UART, without resetting it."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import time

import kvm_serial


STATUS = re.compile(r"^UI_HOST attached=([01]) link=([012]) sharing=([01]) live_panel=([01]) "
                    r"lcd_frames=(\d+) fps_x10=(\d+) rx_to_lcd_ms=(\d+) capture=([a-z0-9_]+)\r?$", re.M)
FIELDS = ("attached", "link", "sharing", "live_panel", "lcd_frames", "fps_x10", "rx_to_lcd_ms")


def observe(port, output, seconds, duration=5):
    first_count = last_count = None
    window_start = last_advance = 0.0
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        data, ok = kvm_serial.run_cmd(port, "ui host-status", 2)
        now = time.monotonic()
        status = STATUS.search(data) if ok else None
        row = {"utc": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
               "elapsed_s": round(now - start, 3)}
        if status:
            row.update(zip(FIELDS, map(int, status.groups()[:-1])))
            row["capture"] = status.group(8)
        else:
            row["capture"] = "no_response"
        output.write(json.dumps(row) + "\n")
        output.flush()
        active = (status and row["attached"] and row["link"] == 2 and row["sharing"]
                  and row["live_panel"] and row["fps_x10"] > 0)
        if active:
            count = row["lcd_frames"]
            if last_count is None or count < last_count:
                first_count = count
                window_start = last_advance = now
            elif count > last_count:
                if now - last_advance > 3:
                    first_count = count
                    window_start = now
                last_advance = now
            last_count = count
            advanced = count - first_count
            # Only time bounded by completed pictures qualifies toward the run.
            elapsed = last_advance - window_start
            if advanced >= 10 and elapsed >= duration:
                print(f"{advanced} additional LCD frames over {elapsed:.1f} s "
                      f"({advanced / elapsed:.2f} fps); latest RX to LCD {row['rx_to_lcd_ms']} ms.")
                return True
        else:
            last_count = None
        time.sleep(min(1.0, max(0.0, seconds - (time.monotonic() - start))))
    print("Observation ended before a sustained stream was measured.")
    return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True, help="New JSON Lines file for metadata samples")
    parser.add_argument("--seconds", type=int, default=120, help="total timeout including connection setup")
    parser.add_argument("--duration", type=int, default=5, help="required continuous stream duration in seconds")
    args = parser.parse_args()
    if args.duration < 1 or args.seconds < 1:
        parser.error("--duration and --seconds must be positive")
    with kvm_serial.open_port(args.port) as uart, args.output.open("x", encoding="ascii") as log:
        raise SystemExit(0 if observe(uart, log, args.seconds, args.duration) else 1)
