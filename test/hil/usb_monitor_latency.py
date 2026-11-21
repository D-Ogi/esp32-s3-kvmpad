# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Measure Windows GDI paint to 7B USB monitor refresh completion using a shared PC clock."""
import argparse
from contextlib import contextmanager
import ctypes
from datetime import datetime, timezone
from pathlib import Path
import re
from statistics import median
import subprocess
import sys
from threading import Timer
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from usb_monitor_pattern import panel_lines, save_result, verify_uart


class WindowsClock:
    def __init__(self):
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.frequency = self.read(self.api.QueryPerformanceFrequency)

    @staticmethod
    def read(query):
        value = ctypes.c_longlong()
        if not query(ctypes.byref(value)):
            raise ctypes.WinError(ctypes.get_last_error())
        return value.value

    def now(self):
        return self.read(self.api.QueryPerformanceCounter)


class Board:
    def __init__(self, port, mode="640x480", portrait_console=False):
        self.port = port
        self.mode = mode
        self.portrait_console = portrait_console

    def presented(self):
        # Stop at the newline carrying the counter. Waiting for the prompt or
        # draining an idle serial port here would inflate the upper bound.
        self.port.reset_input_buffer()
        self.port.write(b"ui usb-monitor-status\r\n")
        data = bytearray()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and len(data) < 8192:
            data.extend(self.port.read(max(1, min(self.port.in_waiting, 1024))))
            match = re.search(rb"(?m)^USB_MONITOR mounted=([01]) [^\r\n]*?\bpresented=(\d+)\b([^\r\n]*)\r?\n", data)
            if match:
                if match[1] != b"1" and b"suspended=1" not in match[3]:
                    raise ValueError("USB monitor disconnected")
                return int(match[2])
        raise ValueError("Missing USB presentation counter on UART")

    def verify(self, order):
        self.port.reset_input_buffer()
        self.port.write(b"ui snap 16 compact\r\n")
        self.port.flush()
        return verify_uart(panel_lines(self.port), order, div=16, viewport=self.mode,
                           portrait_console=self.portrait_console)

    def receiver_timing(self):
        import kvm_serial

        self.port.reset_input_buffer()
        self.port.write(b"ui usb-monitor-status latency\r\n")
        self.port.flush()
        reply, ok = kvm_serial.read_until(self.port, r"(?m)^USB_MONITOR_MEAN [^\r\n]*\r?\n", 3)
        header = re.search(r"(?m)^USB_MONITOR_LATENCY unit=us samples=(\d+) sequence=(\d+) peak_total=\d+\s*$", reply)
        stages = re.search(r"(?m)^USB_MONITOR_LAST receive=(\d+) queue=(\d+) decode=(\d+) ready=(\d+) refresh=(\d+)\s*$", reply)
        if not ok or not header or not stages or not int(header[1]) or not int(header[2]):
            raise ValueError("Missing receiver timing for the displayed USB frame")
        return {"samples": int(header[1]), "sequence": int(header[2]),
                "stages_us": dict(zip(("receive", "queue", "decode", "ready", "refresh"), map(int, stages.groups())))}


def measure_step(board, clock, initial, paint_tick, order, previous_receiver=None):
    # 'presented' advances after the last flush. A read returning the old
    # counter proves completion occurred after the beginning of that query.
    lower = paint_tick
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        start = clock.now()
        count = board.presented()
        end = clock.now()
        if count == initial:
            lower = max(start, lower)
            continue
        if count != initial + 1:
            raise ValueError("Multiple presentations or a counter reset made the transition ambiguous")
        # Snapshot transfer time stays outside the timing interval. Both
        # bracketing counts must identify the same submitted framebuffer.
        board.verify(order)
        sample = {"presented": count, "order": order,
                  "lower_ms": (lower - paint_tick) * 1000 / clock.frequency,
                  "upper_ms": (end - paint_tick) * 1000 / clock.frequency}
        if previous_receiver is not None:
            timing = board.receiver_timing()
            if timing["samples"] != previous_receiver["samples"] + 1 or timing["sequence"] <= previous_receiver["sequence"]:
                raise ValueError("Receiver timing did not advance with the verified presentation")
            sample["receiver"] = timing
        if board.presented() != count:
            raise ValueError("Presentation changed during scanout verification")
        return sample
    raise ValueError("Timed out waiting for a changed USB picture")


def find_display(probe, serial, wait_seconds=0, mode="640x480", match_client=None):
    version = subprocess.run([probe, "--latency-pattern-version"], capture_output=True, timeout=5)
    if version.returncode or version.stdout.splitlines() != [b"KVM_USB_LATENCY_PATTERN 1"]:
        raise ValueError("Rebuild the Windows probe with latency pattern support")
    native_version = subprocess.run([probe, "--native-latency-version"], capture_output=True, timeout=5)
    strict = native_version.returncode == 0 and native_version.stdout.splitlines() == [b"KVM_USB_NATIVE_LATENCY 1"]
    if mode != "640x480" and not strict:
        raise ValueError("Rebuild the Windows probe with native latency support")
    deadline = time.monotonic() + wait_seconds
    observed = ""
    matched = False
    while True:
        for options in ([], ["--composite"]):
            prefix = [probe, *options, serial]
            query = [*prefix, "--latency-target", mode] if strict else prefix
            topology = subprocess.run(query, capture_output=True, timeout=10)
            match = re.search(rb"(?m)^board GDI source=(\\\\\.\\DISPLAY\d+) origin=", topology.stdout)
            if (strict and topology.returncode == 0 and
                    b"LATENCY_TARGET " + mode.encode() in topology.stdout.splitlines()):
                if not match:
                    raise ValueError("The probe verified the resolution but omitted the Windows display name. Rebuild the Windows probe.")
                return prefix, match[1].decode("ascii")
            modes = subprocess.run([*prefix, "--modes"], capture_output=True, timeout=10)
            sizes = re.search(rb"\bsource=(\\\\\.\\DISPLAY\d+) current=(\d+x\d+) [^\r\n]*"
                              rb"\bactive_signal=(\d+x\d+|unknown)\b", modes.stdout)
            if modes.returncode == 0 and sizes:
                desktop, signal = (v.decode("ascii") for v in (sizes[2], sizes[3]))
                observed = f" Windows reports desktop {desktop}, USB signal {signal}."
                if strict and desktop == signal == mode:
                    observed += " The display target could not be validated for drawing in this Windows session."
                if not strict and topology.returncode == 0 and match and match[1] == sizes[1] and desktop == signal == mode:
                    return prefix, match[1].decode("ascii")
                if match_client and not matched and signal == mode and desktop != mode:
                    change = subprocess.run([match_client, "--usb-match-signal", serial, mode],
                                            capture_output=True, timeout=12)
                    if change.returncode:
                        message = change.stderr.decode(errors="replace").strip()
                        raise ValueError(f"Windows could not match the desktop to the signal.{observed} {message}")
                    matched = True
                    deadline = max(deadline, time.monotonic() + 2)
        if time.monotonic() >= deadline:
            raise ValueError(f"The requested board needs an independent {mode} desktop and matching signal.{observed}")
        time.sleep(.25)


@contextmanager
def monitor_session(port, fingerprint, mode="640x480"):
    """Optionally open the requested monitor from Extensions and restore that menu on exit."""
    if fingerprint is None:
        yield False
        return
    import kvm_serial

    def command(text, expected):
        reply, done = kvm_serial.run_cmd(port, text, 3)
        if not done or not re.search(expected, reply, re.M):
            raise ValueError(f"Board could not complete {text}")
        return reply

    command("remote status", rf"\bcert_sha256={fingerprint.lower()}\b")
    state = command("ui usb-monitor-status", r"^USB_MONITOR mounted=[01]\b")
    if re.search(r"^USB_MONITOR .*?\b(?:mounted|suspended)=1\b", state, re.M):
        yield False
        return
    # The firmware accepts this command only from the foreground Extensions menu.
    command(f"ui open usb-monitor {mode} controls", rf"^USB_MONITOR_OPEN size={mode} started=1\b")
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            state = command("ui usb-monitor-status", r"^USB_MONITOR mounted=[01]\b")
            if re.search(r"^USB_MONITOR mounted=1\b", state, re.M):
                break
            time.sleep(.2)
        else:
            raise ValueError("Windows did not configure the USB monitor")
        yield True
    finally:
        command("ui shell", r"shell -> ESP_OK")
        deadline = time.monotonic() + 5
        while True:
            state = command("ui shell status", r"^UI_SHELL active=(?:yes|no)\b")
            if re.search(r"^UI_SHELL active=yes\b", state, re.M):
                break
            if time.monotonic() >= deadline:
                raise ValueError("The Home screen did not become active after closing the USB monitor")
            time.sleep(.1)
        command("ui open card", r"^UI_CARD browser=ESP_OK")


def confirm_initial(board):
    deadline = time.monotonic() + 8
    count = 0
    while time.monotonic() < deadline:
        count = board.presented()
        if not count:
            time.sleep(.1)
            continue
        try:
            board.verify("RGBW")
        except ValueError as exc:
            if "quadrant mismatch" not in str(exc):
                raise
            continue
        if board.presented() == count:
            return count
    if not count:
        raise ValueError("USB is configured but the board has not presented its first frame")
    raise ValueError("The initial RGBW pattern did not settle on the board")


def run_steps(pattern, board, clock, steps, result, mode="640x480", receiver_stages=False):
    for line in pattern.stdout:
        if line.strip() == b"PATTERN_UNAVAILABLE input_desktop":
            raise ValueError("The Windows session has no access to the active desktop")
        if line.strip() == f"PATTERN_READY {mode} RGBW".encode():
            break
        if line.startswith(b"PATTERN_READY "):
            raise ValueError("Windows painted a different resolution than requested")
    else:
        raise ValueError("The target window did not paint its initial pattern")
    initial = confirm_initial(board)
    previous_receiver = None
    if receiver_stages:
        previous_receiver = board.receiver_timing()
        if board.presented() != initial:
            raise ValueError("Initial presentation changed while reading receiver timing")
    for step in range(1, steps + 1):
        order = "WRGB" if step % 2 else "RGBW"
        requested = clock.now()
        pattern.stdin.write(b"NEXT\n")
        pattern.stdin.flush()
        for line in pattern.stdout:
            if line.startswith(b"PATTERN_STEP "):
                break
        else:
            raise ValueError("Missing Windows paint acknowledgement")
        ack = re.fullmatch(rb"PATTERN_STEP (\d+) (RGBW|WRGB) (\d+) (\d+)", line.strip())
        if not ack or int(ack[1]) != step or ack[2].decode() != order or int(ack[4]) != clock.frequency:
            raise ValueError("Unexpected Windows paint acknowledgement")
        paint = int(ack[3])
        if not requested <= paint <= clock.now():
            raise ValueError("Paint timestamp is outside the command interval")
        sample = measure_step(board, clock, initial, paint, order, previous_receiver)
        result["samples"].append(sample)
        initial = sample["presented"]
        previous_receiver = sample.get("receiver")
        print(f"{step} {order}  {sample['lower_ms']:.1f}–{sample['upper_ms']:.1f} ms", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--mode", choices=["640x480", "1024x600"], default="640x480")
    parser.add_argument("--match-opened-desktop", metavar="CLIENT", type=Path,
                        help="match the Windows source to the USB signal when this trial opens the display")
    session = parser.add_mutually_exclusive_group()
    session.add_argument("--portrait-console", action="store_true",
                         help="measure the running portrait USB Monitor Console with its viewport set to Fit")
    session.add_argument("--open-from-extensions", metavar="BOARD_CERT_SHA256",
                        help="open the requested mode from Extensions and return there after the trial")
    parser.add_argument("--steps", type=int, choices=range(1, 17), default=8)
    parser.add_argument("--result", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch("[0-9A-Fa-f]{12}", args.serial):
        parser.error("Expected a 12-digit hexadecimal board USB serial")
    if args.open_from_extensions and not re.fullmatch("[0-9A-Fa-f]{64}", args.open_from_extensions):
        parser.error("Expected the board certificate SHA-256 for --open-from-extensions")
    if args.match_opened_desktop and not args.open_from_extensions:
        parser.error("--match-opened-desktop requires --open-from-extensions")
    result = {"status": "RUNNING", "started_utc": datetime.now(timezone.utc).isoformat(),
              "metric": "Windows paint start to board refresh completion",
              "board_usb_serial": args.serial.upper(), "requested_mode": args.mode,
              "view": "portrait-console" if args.portrait_console else "standalone", "samples": []}
    save_result(args.result, result)
    try:
        import kvm_serial

        clock = WindowsClock()
        with kvm_serial.open_port(args.port) as port, monitor_session(port, args.open_from_extensions, args.mode) as opened:
            prefix, result["display"] = find_display(str(args.probe.resolve()), args.serial.upper(), 12 if opened else 0,
                                                     args.mode, args.match_opened_desktop if opened else None)
            command = [*prefix, "--pattern-latency"]
            if args.mode != "640x480":
                command.append(args.mode)
            with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as pattern:
                watchdog = Timer(110, pattern.kill)
                watchdog.start()
                try:
                    run_steps(pattern, Board(port, args.mode, args.portrait_console), clock, args.steps, result,
                              args.mode, receiver_stages=args.portrait_console)
                    pattern.stdin.close()
                    if pattern.wait(timeout=5) != 0:
                        raise ValueError("Windows could not maintain the display target")
                finally:
                    watchdog.cancel()
                    if pattern.poll() is None:
                        pattern.kill()
        result["median_lower_ms"] = median(s["lower_ms"] for s in result["samples"])
        result["median_upper_ms"] = median(s["upper_ms"] for s in result["samples"])
        result["status"] = "PASS"
    except (KeyboardInterrupt, ImportError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        result.update(status="FAIL", error=str(exc) or "Interrupted")
        print(result["error"], file=sys.stderr)
    result["finished_utc"] = datetime.now(timezone.utc).isoformat()
    save_result(args.result, result)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
