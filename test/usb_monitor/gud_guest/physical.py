# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Boot the pinned Linux DRM client against one USB target and wait for two scanout acknowledgments."""

import argparse
import json
import queue
import re
import secrets
import socket
import subprocess
import threading
import time
from pathlib import Path

from run import build
from host_identity import check_target


def usb_filter(hostbus, hostaddr, vidpid):
    if not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{4}", vidpid):
        raise ValueError("Expected a four-digit hexadecimal VID:PID")
    vid, pid = [int(field, 16) for field in vidpid.split(":")]
    if vid in (0, 0xffff) or not pid or (vid == 0x303a and pid in (0x400c, 0x400a, 0x2986, 0x2987)):
        raise ValueError("Expected an authorized, dedicated GUD identity")
    if not 1 <= hostbus <= 255 or not 1 <= hostaddr <= 127:
        raise ValueError("Expected exact nonzero libusb host bus and address")
    return f"usb-host,bus=xhci.0,hostbus={hostbus},hostaddr={hostaddr},vendorid=0x{vid:04x},productid=0x{pid:04x}"


def publish(path, value):
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    tmp.replace(path)


def validate_ack(ready, ack):
    if ready["phase"] in VT_TEXT:
        expected = dict(ready, divisor=1, examined=640 * 480, mismatch=0, lit=ready["font_lit"])
    elif ready["phase"] in MODESET:
        _, _, w, h = MODESET[ready["phase"]]
        expected = dict(ready, divisor=1, examined=w * h, mismatch=0)
    elif ready["phase"] in BLANK_WAKE:
        expected = dict(ready, divisor=1, examined=640 * 480, mismatch=0)
    else:
        expected = dict(ready, examined=19200, mismatch=0, inverted_samples=10 if ready["phase"] == "edge" else 0)
    if any(type(ack.get(key)) is not type(value) or ack[key] != value for key, value in expected.items()):
        raise ValueError(f"Invalid acknowledgment for {ready['phase']}")


VT_TEXT = {"tty1": "KVMpad Linux console", "tty2": "Recovery console ready"}
BLANK_WAKE = ("lit", "blank", "wake")
MODESET = {f"{mode}-{image}": geometry
           for mode, geometry in (("vga", (192, 84, 640, 480)), ("svga", (112, 0, 800, 600)),
                                  ("return", (192, 84, 640, 480)))
           for image in ("full", "edge")}


def parse_bitmap(line, phase):
    match = re.fullmatch(r"GUD_VT_BITMAP (tty[12]) (\d+) (\d+) (\d+) ([0-9a-f]+)", line)
    if not match or match[1] != phase:
        raise ValueError("Wrong or malformed VT bitmap")
    fw, fh, lit = map(int, match.group(2, 3, 4))
    width = fw * len(VT_TEXT[phase])
    packed = bytes.fromhex(match[5])
    stride = (width + 7) // 8
    if (not 1 <= fw <= 32 or not 1 <= fh <= 32 or width > 640 or len(packed) != stride * fh
            or any(b & ((1 << (-width % 8)) - 1) for b in packed[stride - 1::stride])
            or not lit or sum(b.bit_count() for b in packed) != lit):
        raise ValueError("Invalid VT font raster")
    return dict(text=VT_TEXT[phase], font_width=fw, font_height=fh, font_lit=lit, glyph_rows=match[5])


class MotionRun:
    def __init__(self, work, token, mode, vidpid, serial):
        self.work = work
        self.identity = dict(run=token, serial=serial, vidpid=vidpid,
                             source="virtual-receiver" if mode == "loopback" else "scanout")
        self.started = False
        self.acknowledged = False
        self.last_id = 0

    def gate(self, phase, signal, vm, timeout):
        record = dict(self.identity, phase=phase)
        path = self.work / f"{phase}-motion.json"
        incoming = self.work / f"{signal}-motion.json"
        if incoming.exists():
            raise ValueError(f"Premature {incoming.name}")
        publish(path, record)
        print(f"GUD_MOTION_{phase.upper()} {path}", flush=True)
        deadline = time.monotonic() + timeout
        while not incoming.exists():
            if vm.poll() is not None or time.monotonic() >= deadline:
                raise TimeoutError(f"Missing {incoming.name}")
            time.sleep(0.05)
        if json.loads(incoming.read_text("utf-8")) != record:
            raise ValueError(f"Invalid {incoming.name}")

    def handle(self, line, host_ns, console, vm, timeout):
        if line == "GUD_MOTION_READY" and not self.started:
            self.gate("ready", "start", vm, timeout)
            console.sendall(b"START\r")
            self.started = True
        elif self.started and self.last_id < 20 and line == f"GUD_MOTION_FRAME {self.last_id + 1}":
            self.last_id += 1
            with (self.work / "motion-frames.jsonl").open("a", encoding="ascii") as log:
                print(json.dumps(dict(id=self.last_id, host_ns=host_ns)), file=log)
        elif line == "GUD_MOTION_HELD" and self.last_id == 20 and not self.acknowledged:
            self.gate("held", "ack", vm, timeout)
            console.sendall(b"ACK motion\r")
            self.acknowledged = True
        else:
            raise ValueError(f"Unexpected guest motion event: {line}")


def run(cmd, work, mode, vidpid, serial, timeout, listener, require_autobind, motion=False, console_test=False,
        input_events=False, blank_wake=False, modeset=False):
    token = secrets.token_hex(16)
    sequence = MotionRun(work, token, mode, vidpid, serial) if motion else None
    phases = tuple(VT_TEXT) if console_test else ("full", "edge")
    if blank_wake:
        phases = BLANK_WAKE
    elif modeset:
        phases = tuple(MODESET)
    bitmap = None
    grabbed = input_pass = False
    stage = 0
    identified = complete = False
    binding = None
    lines = queue.Queue()
    with (work / "qemu.log").open("wb") as diagnostics, subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=diagnostics, stderr=subprocess.STDOUT
    ) as vm:
        console = None
        reader = None

        def collect():
            try:
                with console.makefile("r", encoding="utf-8", errors="replace") as stream:
                    with (work / "guest.log").open("w", encoding="utf-8") as log:
                        for line in stream:
                            host_ns = time.perf_counter_ns()
                            log.write(line)
                            log.flush()
                            lines.put((line.rstrip("\r\n"), host_ns))
            except ConnectionResetError:
                # QEMU on Windows resets its serial socket when the guest powers off.
                pass
            finally:
                lines.put((None, None))

        try:
            listener.settimeout(min(timeout, 15))
            console, _ = listener.accept()
            reader = threading.Thread(target=collect, daemon=True)
            reader.start()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    line, host_ns = lines.get(timeout=0.2)
                except queue.Empty:
                    continue
                if line is None:
                    break
                if line == f"GUD_TARGET {vidpid} {serial}":
                    identified = True
                elif line in ("GUD_BIND upstream", "GUD_BIND runtime", "GUD_BIND none"):
                    binding = line.split()[-1]
                    print(line, flush=True)
                    if binding == "runtime" and require_autobind:
                        raise ValueError("Strict qualification requires an upstream match")
                elif line.startswith("GUD_REJECT "):
                    raise ValueError(line)
                elif line.startswith("GUD_MOTION_"):
                    if not sequence or not identified or binding not in ("upstream", "runtime") or mode == "no-device":
                        raise ValueError(f"Unexpected guest motion event: {line}")
                    sequence.handle(line, host_ns, console, vm, timeout)
                    deadline = time.monotonic() + timeout
                elif line == "GUD_INPUT_READY":
                    if not input_events or grabbed or stage or not bitmap:
                        raise ValueError("Unexpected guest input readiness")
                    grabbed = True
                elif line == "GUD_INPUT_PASS":
                    if not input_events or not grabbed or input_pass or stage != 1:
                        raise ValueError("Unexpected guest input confirmation")
                    input_pass = True
                elif line.startswith("GUD_VT_BITMAP "):
                    if not console_test or not identified or binding not in ("upstream", "runtime") or bitmap or stage >= 2:
                        raise ValueError("Unexpected VT font raster")
                    bitmap = parse_bitmap(line, phases[stage])
                elif line.startswith("GUD_LCD_READY "):
                    phase = line.split()[-1]
                    if sequence or not identified or binding not in ("upstream", "runtime") or stage >= len(phases) or phase != phases[stage] or mode == "no-device":
                        raise ValueError(f"Unexpected guest readiness: {line}")
                    ready = dict(run=token, phase=phase, serial=serial, vidpid=vidpid,
                                 source="virtual-receiver" if mode == "loopback" else "scanout")
                    if console_test:
                        if not bitmap:
                            raise ValueError("Missing VT font raster")
                        if input_events:
                            if not grabbed or (stage and not input_pass):
                                raise ValueError("Unconfirmed evdev gate")
                            ready["input_gate"] = "passed" if stage else "grabbed"
                        ready.update(bitmap)
                        bitmap = None
                    path = work / f"ready-{phase}.json"
                    ack = work / f"ack-{phase}.json"
                    if ack.exists():
                        raise ValueError(f"Premature {ack.name}")
                    publish(path, ready)
                    print(f"{line} {path}", flush=True)
                    deadline = time.monotonic() + timeout
                    while not ack.exists():
                        if vm.poll() is not None or time.monotonic() >= deadline:
                            raise TimeoutError(f"Missing {ack.name}")
                        time.sleep(0.1)
                    validate_ack(ready, json.loads(ack.read_text("utf-8")))
                    console.sendall(f"ACK {phase}\r".encode("ascii"))
                    stage += 1
                    deadline = time.monotonic() + timeout
                elif line == ("GUD_CONSOLE_COMPLETE" if console_test else "GUD_DRAW_COMPLETE"):
                    complete = sequence.acknowledged if sequence else stage == len(phases)
            if not complete or vm.wait(timeout=10) != 0:
                raise ValueError("Guest did not finish the acknowledged display sequence")
        finally:
            if vm.poll() is None:
                vm.kill()
            vm.wait()
            if reader:
                reader.join()
            if console:
                console.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, help="prepared and verified run.py inputs")
    parser.add_argument("work", type=Path, help="fresh output directory, must not exist")
    group = parser.add_mutually_exclusive_group(required=True)
    for mode in ("attach", "no-device", "loopback"):
        group.add_argument(f"--{mode}", dest="mode", action="store_const", const=mode)
    parser.add_argument("--vidpid", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--require-autobind", action="store_true",
                        help="require a built-in driver ID match; never add a runtime ID")
    parser.add_argument("--require-controls", action="store_true",
                        help="require the exact boot-keyboard/mouse interfaces, Linux HID drivers and evdev capabilities")
    parser.add_argument("--check-input-events", action="store_true",
                        help="observe the operator's fixed HID probes in the isolated guest; implies --console --require-controls")
    exercise = parser.add_mutually_exclusive_group()
    exercise.add_argument("--modeset", action="store_true", help="switch VGA to SVGA and back, verifying each full and edge update")
    exercise.add_argument("--blank-wake", action="store_true", help="disable then re-enable the active DRM output using the same framebuffer")
    exercise.add_argument("--motion", action="store_true", help="hold for START, submit 20 numbered fullscreen frames at 2 Hz")
    exercise.add_argument("--console", action="store_true", help="write two messages to pre-login tty1 and verify scanout")
    parser.add_argument("--hostbus", type=int)
    parser.add_argument("--hostaddr", type=int)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--timeout", type=int, default=120, help="seconds allowed per phase")
    args = parser.parse_args()
    if args.blank_wake and (args.mode != "attach" or args.check_input_events):
        parser.error("--blank-wake requires one physical board and exclusive use of its display exercise")
    if args.check_input_events:
        if args.mode != "attach" or args.motion or args.modeset:
            parser.error("--check-input-events requires --attach and the console exercise")
        args.console = args.require_controls = True
    vidpid = args.vidpid.lower()
    if not re.fullmatch(r"[0-9A-F]{12}", args.serial) or not re.fullmatch(r"[a-f0-9]{4}:[a-f0-9]{4}", vidpid):
        parser.error("Expected VID:PID and exact 12-digit uppercase KVMpad serial")
    if not 1 <= args.timeout <= 900:
        parser.error("Timeout must be 1..900 seconds")
    device = None
    if args.mode == "attach":
        if args.hostbus is None or args.hostaddr is None:
            parser.error("Both --hostbus and --hostaddr are required with --attach")
        device = usb_filter(args.hostbus, args.hostaddr, vidpid)
    elif args.hostbus is not None or args.hostaddr is not None:
        parser.error("Only --attach accepts host USB coordinates")
    vid, pid = vidpid.split(":")
    virtual = args.mode == "loopback"
    work = args.work.resolve()
    work.mkdir(parents=True)
    target = f"VID={vid}\nPID={pid}\nSERIAL={args.serial}\nVIRTUAL={int(virtual)}\n"
    target += f"REQUIRE_AUTOBIND={int(args.require_autobind)}\n"
    target += f"REQUIRE_CONTROLS={int(args.require_controls)}\n"
    target += f"CHECK_INPUT_EVENTS={int(args.check_input_events)}\nINPUT_TIMEOUT={args.timeout}\n"
    target += f"MOTION={int(args.motion)}\n"
    target += f"BLANK_WAKE={int(args.blank_wake)}\n"
    target += f"MODESET_TEST={int(args.modeset)}\n"
    target += f"CONSOLE_TEST={int(args.console)}\n"
    cmd = build(args.inputs.resolve(), work, args.clang, args.qemu, target, virtual)
    if not virtual:
        cmd.extend(["-device", "qemu-xhci,id=xhci"])
    if device:
        cmd.extend(["-device", device])
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        cmd[cmd.index("stdio")] = "chardev:gudconsole"
        cmd.extend(["-chardev", f"socket,id=gudconsole,host=127.0.0.1,port={port}"])
        publish(work / "qemu.json", cmd)
        if device:
            check_target(args.qemu, args.hostbus, args.hostaddr, vidpid, args.serial)
        run(cmd, work, args.mode, vidpid, args.serial, args.timeout, listener, args.require_autobind,
            args.motion, args.console, args.check_input_events, args.blank_wake, args.modeset)
    if args.console:
        print(f"PASS {args.mode} both pre-login tty1 messages acknowledged")
    elif args.motion:
        print(f"PASS {args.mode} 20 DRM submissions and final hold acknowledged")
    elif args.blank_wake:
        print(f"PASS {args.mode} lit, blank and restored framebuffer acknowledged")
    elif args.modeset:
        print(f"PASS {args.mode} VGA to SVGA to VGA, six images acknowledged")
    else:
        print(f"PASS {args.mode} full and edge acknowledged")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TimeoutError, subprocess.SubprocessError) as exc:
        raise SystemExit(str(exc))
