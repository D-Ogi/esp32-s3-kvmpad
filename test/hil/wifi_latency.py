# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""ADR-017 Wi-Fi benches on the board (TASK-080), driven over the UART console. No reset, no HID.

  reconnect --count N          AT-EXT-02: alternate `wifi reconnect` (cached BSSID/channel) and `wifi reconnect full`
                               (in-RAM record dropped -> full scan); the board times each cycle to the IP.
  ping --host H [--idle 10 60] AT-EXT-01: first-packet latency after N s of silence, WIFI_PS_NONE vs
       [--count N]             WIFI_PS_MIN_MODEM (`wifi ps none|min`), with the system `ping`. The board is left
                               in WIFI_PS_NONE (the TECH-070 default) whatever happens.

Prints one line per sample and a min / p50 / p95 / max summary; `--save` writes the whole run to the evidence
directory and prints its SHA-256. Nothing typed on the board and no credential is involved.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import pathlib
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import kvm_serial as ks  # noqa: E402

STATUS = re.compile(r"WIFI link=(\S+) .*?ip=(\S+)")
COUNTERS = re.compile(r"connects=(\d+)")
TIMING = re.compile(r"WIFI_TIMING last_connect_ms=(\d+) via=(\S+) ps=(\S+)")
PING_MS = re.compile(r"(?:time|czas|Zeit|temps)\s*([=<])\s*(\d+)\s*ms", re.IGNORECASE)


def pct(values: list[float], p: float) -> float:
    v = sorted(values)
    if not v:
        return float("nan")
    k = (len(v) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def summary(label: str, values: list[float], out) -> None:
    if not values:
        print(f"SUMMARY {label} n=0", file=out)
        return
    print(
        f"SUMMARY {label} n={len(values)} min={min(values):.0f} p50={statistics.median(values):.0f} "
        f"p95={pct(values, 0.95):.0f} max={max(values):.0f} ms",
        file=out,
    )


def status(s) -> tuple[str, bool, int, int, str, str]:
    text, _ = ks.run_cmd(s, "wifi status", 5.0)
    m, c, t = STATUS.search(text), COUNTERS.search(text), TIMING.search(text)
    if not (m and c and t):
        raise RuntimeError("unexpected wifi status answer")
    return m.group(1), m.group(2) == "yes", int(c.group(1)), int(t.group(1)), t.group(2), t.group(3)


def cmd_reconnect(args, s, out) -> int:
    results: dict[str, list[float]] = {"cached": [], "full": []}
    bad = 0
    for i in range(args.count):
        want = "cached" if i % 2 == 0 else "full"
        link, ip, connects0, _, _, _ = status(s)
        if link != "connected" or not ip:
            print(f"SAMPLE {i} skipped: link={link}", file=out)
            bad += 1
            time.sleep(2)
            continue
        ks.run_cmd(s, "wifi reconnect" + (" full" if want == "full" else ""), 5.0)
        t_end = time.monotonic() + 40
        done = False
        while time.monotonic() < t_end:
            time.sleep(0.5)
            link, ip, connects, ms, via, _ = status(s)
            if connects > connects0 and link == "connected" and ip:
                done = True
                break
        if not done:
            print(f"SAMPLE {i} path={want} TIMEOUT link={link}", file=out)
            bad += 1
            continue
        ok = via == want
        bad += 0 if ok else 1
        results[via].append(float(ms))
        print(f"SAMPLE {i} asked={want} via={via} ms={ms}{'' if ok else ' MISMATCH'}", file=out, flush=True)
        time.sleep(args.settle)
    for k, v in results.items():
        summary(f"reconnect_{k}", v, out)
    text, _ = ks.run_cmd(s, "wifi status", 5.0)
    for line in text.splitlines():
        if line.startswith("WIFI_COUNTERS"):
            print("FINAL " + line.strip(), file=out)
    return 0 if bad == 0 else 1


def ping_once(host: str) -> float | None:
    p = subprocess.run(["ping", "-n", "1", "-w", "3000", host], capture_output=True, text=True, errors="replace")
    m = PING_MS.search(p.stdout)
    if not m:
        return None
    return 0.5 if m.group(1) == "<" else float(m.group(2))


def cmd_ping(args, s, out) -> int:
    lost = 0
    try:
        for mode in ("none", "min"):
            text, _ = ks.run_cmd(s, f"wifi ps {mode}", 5.0)
            if "err=ESP_OK" not in text:
                print(f"PS {mode} could not be set", file=out)
                return 2
            print(f"PS mode={mode}", file=out, flush=True)
            for idle in args.idle:
                vals: list[float] = []
                for i in range(args.count):
                    time.sleep(idle)
                    ms = ping_once(args.host)
                    if ms is None:
                        lost += 1
                        print(f"SAMPLE ps={mode} idle={idle}s #{i} LOST", file=out, flush=True)
                        continue
                    vals.append(ms)
                    print(f"SAMPLE ps={mode} idle={idle}s #{i} ms={ms:g}", file=out, flush=True)
                summary(f"ping_ps={mode}_idle={idle}s", vals, out)
    finally:
        text, _ = ks.run_cmd(s, "wifi ps none", 5.0)  # TECH-070 default restored in every case
        print("RESTORED " + ("ps=none" if "err=ESP_OK" in text else "FAILED to restore ps=none"), file=out)
    print(f"LOST {lost}", file=out)
    return 0


class Tee(io.StringIO):
    def write(self, s):  # type: ignore[override]
        sys.stdout.write(s)
        sys.stdout.flush()
        return super().write(s)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--save", help="evidence directory (outside the repository)")
    ap.add_argument("--name", default="wifi_latency")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("reconnect")
    r.add_argument("--count", type=int, default=20)
    r.add_argument("--settle", type=float, default=3.0, help="seconds connected before the next sample")
    p = sub.add_parser("ping")
    p.add_argument("--host", required=True)
    p.add_argument("--idle", type=int, nargs="+", default=[10, 60])
    p.add_argument("--count", type=int, default=10)
    args = ap.parse_args()
    out = Tee()
    s = ks.open_port(args.port)
    try:
        rc = (cmd_reconnect if args.cmd == "reconnect" else cmd_ping)(args, s, out)
    finally:
        s.close()
    print(f"RESULT rc={rc}", file=out)
    if args.save:
        ks.save(pathlib.Path(args.save), f"{args.name}-{args.cmd}", out.getvalue())
    return rc


if __name__ == "__main__":
    sys.exit(main())
