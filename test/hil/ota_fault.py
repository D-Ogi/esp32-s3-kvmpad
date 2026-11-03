# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""One run of the signed-OTA board plan (docs/OTA.md, AT-FAULT-08): start `ota url`, reset at a chosen point.

  --at receive50   EN reset once the RECEIVE progress reaches 50 %             (row 1: power cut while writing)
  --at verify      EN reset as soon as VERIFY starts (before the switch)      (row 2: power cut before the switch)
  --at switched    EN reset right after `boot partition switched`            (row 3: the switch is atomic)
  --at pending     no reset during the update; EN reset when the new image's health check starts (row 5)
  --at none        no reset at all: follow the update, the new boot and its health check (rows 3/4/6 follow-up)

Every run then captures the next boot(s) for up to --settle seconds and ends with `ota status` and `info`.
One EN reset per run at most (the reset budget of the plan). Raw logs go to ../evidence/hardware.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import kvm_serial as ks  # noqa: E402

PROGRESS = re.compile(r"\b(prepare|receive|verify|set_boot|reboot) (\d+)%")


def follow(s, until, timeout: float) -> tuple[str, bool]:
    """Read lines until `until(line)` is true for one of them."""
    buf, text = "", ""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        chunk = s.read(4096)
        if not chunk:
            continue
        buf += chunk.decode("utf-8", errors="replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = ks.ANSI.sub("", line).rstrip("\r")
            text += line + "\n"
            if until(line):
                return text, True
    return text + buf, False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--evidence", default=str(pathlib.Path(__file__).resolve().parents[3] / "evidence" / "hardware"))
    ap.add_argument("--url", required=True)
    ap.add_argument("--at", required=True, choices=["receive50", "verify", "switched", "pending", "none"])
    ap.add_argument("--settle", type=float, default=45.0, help="seconds to follow the boots after the trigger")
    ap.add_argument("--name", default="ota")
    args = ap.parse_args()

    s = ks.open_port(args.port)
    log = []
    try:
        s.write(b"\r\n")
        ks.read_until(s, re.escape(ks.PROMPT), 2.0)
        s.reset_input_buffer()
        s.write(f"ota url {args.url}\r\n".encode())
        s.flush()
        t0 = time.monotonic()

        def trigger(line: str) -> bool:
            m = PROGRESS.search(line)
            if "refused:" in line or re.search(r"\bfailed\b|last reason", line):
                return True
            if args.at == "receive50":
                return bool(m and m.group(1) == "receive" and int(m.group(2)) >= 50)
            if args.at == "verify":
                return bool(m and m.group(1) == "verify")
            if args.at == "switched":
                return "boot partition switched" in line
            return "restarting into the new image" in line  # pending / none

        text, hit = follow(s, trigger, 180.0)
        log.append(f"### ota url (trigger {args.at}) after {time.monotonic() - t0:.1f} s, hit={hit}\n{text}")
        last = text.strip().splitlines()[-1] if text.strip() else ""
        print(f"trigger {args.at}: hit={hit} at {time.monotonic() - t0:.1f} s: {last}")
        if not hit or "refused:" in last:
            return 1

        if args.at in ("receive50", "verify", "switched"):
            ks.reset(s)
            log.append(f"### EN reset at {time.monotonic() - t0:.1f} s\n")
        elif args.at == "pending":
            # The health check of a good image confirms it within about 0.6 s (three clean sweeps), so the reset
            # goes out as soon as the new image reports the check started - before `image confirmed`.
            text, ok = follow(s, lambda ln: "ota: health_check 0%" in ln, 40.0)
            log.append(f"### new image boot\n{text}")
            ks.reset(s)
            log.append("### EN reset at the start of the new image's health check (PENDING_VERIFY)\n")

        text, _ = follow(s, lambda ln: False, args.settle)
        log.append(f"### following the boots for {args.settle:.0f} s\n{text}")
        for ln in text.splitlines():
            if re.search(r"boot #|running ota_|ota state|image confirmed|health|rollback|OTA TEST|rst:|"
                         r"signatures|esp_ota|BOOT_OK", ln) and not PROGRESS.search(ln):
                print("  " + ln[:200])
        s.write(b"\r\n")
        ks.read_until(s, re.escape(ks.PROMPT), 3.0)
        for cmd in ("ota status", "info"):
            out, _ = ks.run_cmd(s, cmd, 10.0)
            log.append(f"### {cmd}\n{out}\n")
            for ln in out.splitlines():
                if re.search(r"state|running|next|signed|rollback|reason|input|secure|reset_reason|boot_count|"
                             r"confirmed|health|version|app ", ln):
                    print("  " + ln.strip()[:200])
    finally:
        ks.save(pathlib.Path(args.evidence), args.name, "".join(log))
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
