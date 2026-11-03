# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""HIL helper for the UART debug port (CH343, USB-to-UART) of board #1.

Subcommands:
  boot                      reset the board through RTS and capture the boot log until BOOT_OK
  cmd "<line>" [...]        run console commands, capture each answer until the prompt
  bootloop N                N resets; each boot must reach BOOT_OK with EXIO5 verified and GT911 found

Raw logs go to an evidence directory OUTSIDE the repository (default ../evidence/hardware);
the script prints the path and SHA256 of every file so records keep only key lines + hashes.
Requires pyserial (from the workspace venv: ../.tools/venv).
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys
import time

import serial  # type: ignore[import-not-found]

PROMPT = "kvmpad>"
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def open_port(port: str) -> serial.Serial:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.05
    # Keep IO0 high (DTR false) and EN released (RTS false) when the port opens.
    s.dtr = False
    s.rts = False
    s.open()
    return s


def reset(s: serial.Serial) -> None:
    s.dtr = False  # IO0 high: normal boot, not download mode
    s.rts = True   # EN low
    time.sleep(0.12)
    s.rts = False  # EN high
    s.reset_input_buffer()


def read_until(s: serial.Serial, pattern: str, timeout: float) -> tuple[str, bool]:
    buf = ""
    end = time.monotonic() + timeout
    rx = re.compile(pattern)
    while time.monotonic() < end:
        chunk = s.read(4096)
        if chunk:
            buf += chunk.decode("utf-8", errors="replace")
            if rx.search(ANSI.sub("", buf)):
                # drain what is still in flight
                time.sleep(0.05)
                buf += s.read(65536).decode("utf-8", errors="replace")
                return ANSI.sub("", buf), True
    return ANSI.sub("", buf), False


def run_cmd(s: serial.Serial, line: str, timeout: float) -> tuple[str, bool]:
    s.reset_input_buffer()
    s.write((line + "\r\n").encode())
    s.flush()
    # The REPL echoes the line, prints the answer, then the next prompt. The prompt is matched at
    # the start of any line, not only at the end of the buffer: asynchronous log lines (USB, touch)
    # can arrive after it and must not turn a finished command into a timeout.
    out, ok = read_until(s, r"(?m)^" + re.escape(PROMPT), timeout)
    return out, ok


def save(evidence: pathlib.Path, name: str, text: str) -> pathlib.Path:
    evidence.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    p = evidence / f"{stamp}-{name}.log"
    p.write_text(text, encoding="utf-8", newline="\n")
    digest = hashlib.sha256(p.read_bytes()).hexdigest()
    print(f"EVIDENCE {p} sha256={digest}")
    return p


def boot_ok(text: str, host: bool = True) -> list[str]:
    """Return the list of failed boot checks (empty = pass). `host`: a USB HOST is attached and must mount us."""
    fails = []
    if "BOOT_OK" not in text:
        fails.append("no BOOT_OK")
    if not re.search(r"EXIO5\(USB_SEL\)=0 -> verified", text):
        fails.append("EXIO5 not verified LOW")
    if not re.search(r"GT911 addr=0x(5D|14)", text):
        fails.append("GT911 not found")
    if "TinyUSB started" not in text:
        fails.append("USB stack not started")
    if host and not re.search(r"usb_hid: mounted \(epoch", text):
        fails.append("USB not mounted by the HOST")
    if not re.search(r"cfg_res=1024x600", text):
        fails.append("GT911 configuration is not 1024x600 (ADR-026)")
    if re.search(r"Guru Meditation|abort\(\)|panic|Backtrace", text):
        fails.append("panic")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--evidence", default=str(pathlib.Path(__file__).resolve().parents[3] / "evidence" / "hardware"))
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--no-host", action="store_true", help="no USB HOST attached: do not require the mount")
    sub = ap.add_subparsers(dest="what", required=True)
    sub.add_parser("boot")
    c = sub.add_parser("cmd")
    c.add_argument("lines", nargs="+")
    c.add_argument("--name", default="cmd")
    c.add_argument("--no-reset", action="store_true", default=True)
    b = sub.add_parser("bootloop")
    b.add_argument("count", type=int)
    args = ap.parse_args()
    evidence = pathlib.Path(args.evidence)

    s = open_port(args.port)
    try:
        if args.what == "boot":
            reset(s)
            text, ok = read_until(s, re.escape(PROMPT), args.timeout)
            save(evidence, "boot", text)
            fails = boot_ok(text, not args.no_host)
            for ln in text.splitlines():
                if re.search(r"board_hal|touch_input|usb_hid|board_lcd|kvmpad:|GT911|BOOT_OK|E \(|W \(", ln):
                    print(ln)
            print("BOOT", "PASS" if not fails else "FAIL " + ", ".join(fails))
            return 0 if not fails else 1

        if args.what == "cmd":
            # Wake the REPL (prompt) without resetting the board.
            s.write(b"\r\n")
            read_until(s, re.escape(PROMPT), 2.0)
            all_out = []
            for line in args.lines:
                out, ok = run_cmd(s, line, args.timeout)
                all_out.append(f"### {line}\n{out}\n")
                print(out.rstrip().removesuffix(PROMPT).rstrip())
                if not ok:
                    print(f"TIMEOUT waiting for the prompt after: {line}")
            save(evidence, args.name, "".join(all_out))
            return 0

        if args.what == "bootloop":
            log = []
            failed = 0
            for i in range(1, args.count + 1):
                reset(s)
                text, _ = read_until(s, re.escape(PROMPT), args.timeout)
                fails = boot_ok(text, not args.no_host)
                m = re.search(r"boot #(\d+)", text)
                boot_no = m.group(1) if m else "?"
                att = re.search(r"\(attempt (\d+)/\d+\)", text)
                mnt = re.search(r"\((\d+)\) usb_hid: mounted", text)
                detail = f"expander_attempt={att.group(1) if att else '?'} usb_mount_ms={mnt.group(1) if mnt else '-'}"
                line = f"cycle {i:3d} boot#{boot_no}: {'PASS' if not fails else 'FAIL ' + ', '.join(fails)} {detail}"
                print(line, flush=True)
                log.append(line + "\n")
                if fails:
                    failed += 1
                    log.append(text + "\n")
            log.append(f"SUMMARY {args.count - failed}/{args.count} PASS\n")
            print(f"SUMMARY {args.count - failed}/{args.count} PASS")
            save(evidence, f"bootloop{args.count}", "".join(log))
            return 0 if failed == 0 else 1
    finally:
        s.close()
    return 2


if __name__ == "__main__":
    sys.exit(main())
