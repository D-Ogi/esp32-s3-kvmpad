# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run the KVMpad HOST in the interactive Windows user session."""
import argparse
import asyncio
import base64
import ctypes
import json
import os
import shutil
import subprocess
import sys
import threading
import tkinter as tk
import time
from pathlib import Path
from tkinter import ttk
from ctypes import wintypes

from adapters import MSFS, OBS, XPlane
from appearance import ControlAppearance
from broker import Broker, Services, listen
from media import Capture
from paired_host import listener_identity, read_board, route_address


class Foreground:
    padding = 16
    text_width = 460

    def __init__(self, pin, identity_label="Paired board SHA-256"):
        self.root = tk.Tk()
        self.root.title("KVMpad HOST")
        self.closed = False
        self.pending = None
        self.review_lock = asyncio.Lock()
        self.broker = None
        self.interactive = True
        self.panel = panel = ttk.Frame(self.root, padding=self.padding)
        panel.pack(fill="both", expand=True)
        panel.columnconfigure(0, weight=1)
        badge = ttk.Label(panel, text=identity_label)
        badge.grid(row=0, column=0, sticky="w")
        self.appearance = ControlAppearance(self.root, badge, "host")
        identity = ttk.Label(panel, text=pin.hex() if isinstance(pin, bytes) else pin, wraplength=self.text_width)
        self.label = ttk.Label(panel, wraplength=self.text_width)
        self.review = ttk.Label(panel, wraplength=self.text_width)
        for row, label in enumerate((identity, self.label, self.review), start=1):
            label.grid(row=row, column=0, sticky="ew", pady=(0, 12 if row == 1 else 0))
            label.bind("<Configure>", self._wrap_label)
        decisions = ttk.Frame(panel)
        decisions.grid(row=4, column=0, sticky="w", pady=(12, 6))
        self.allow = ttk.Button(decisions, text="Allow once", command=lambda: self.decide(True))
        self.deny = ttk.Button(decisions, text="Deny", command=lambda: self.decide(False))
        self.allow.grid(row=0, column=0, padx=(0, 8))
        self.deny.grid(row=0, column=1)
        session = ttk.Frame(panel)
        session.grid(row=5, column=0, sticky="w")
        ttk.Button(session, text="Stop session", command=self.stop).grid(row=0, column=0, padx=(0, 8))
        ttk.Button(session, text="Revoke and exit", command=self.close).grid(row=0, column=1)
        self.minimum_width_widgets = (decisions, session, badge)
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.status("Waiting for the paired board")
        self.allow.state(["disabled"])
        self.deny.state(["disabled"])

    def _fit_height(self):
        width = 2 * self.padding + max(widget.winfo_reqwidth() for widget in self.minimum_width_widgets)
        self.root.minsize(width, self.panel.winfo_reqheight())

    def _wrap_label(self, event):
        # The clam label reserves two pixels on each side of its text.
        wrap = max(1, min(self.text_width, event.width - 4))
        if int(event.widget.cget("wraplength")) != wrap:
            event.widget.configure(wraplength=wrap)
        self.root.after_idle(self._fit_height)

    def status(self, text):
        self.label.config(text=text)
        self.root.after_idle(self._fit_height)

    def configure_obs(self, providers, scene, source):
        row = ttk.Frame(self.panel)
        row.grid(row=6, column=0, sticky="ew", pady=(12, 0))
        row.columnconfigure(0, weight=1)
        ttk.Label(row, text="Local OBS WebSocket password").grid(row=0, column=0, columnspan=2, sticky="w")
        password = ttk.Entry(row, show="*", width=1)
        password.grid(row=1, column=0, sticky="ew", padx=(0, 8))

        def enable():
            if password.get():
                providers["obs"] = OBS(password.get(), scene, source)
                password.delete(0, "end")
                row.destroy()
                self.root.after_idle(self._fit_height)

        ttk.Button(row, text="Enable OBS", command=enable).grid(row=1, column=1)
        self.root.after_idle(self._fit_height)

    def decide(self, result):
        if self.pending and not self.pending.done():
            self.pending.set_result(result)
            self.allow.state(["disabled"])
            self.deny.state(["disabled"])

    async def approve(self, profile, message):
        async with self.review_lock:
            if not self.interactive or self.closed:
                return False
            self.pending = asyncio.get_running_loop().create_future()
            self.review.config(text=f"{profile}\n{message}")
            self.allow.state(["!disabled"])
            self.deny.state(["!disabled"])
            self.root.deiconify()
            self.root.lift()
            try:
                return await self.pending
            finally:
                self.pending = None
                self.review.config(text="")
                self.allow.state(["disabled"])
                self.deny.state(["disabled"])

    def stop(self):
        if self.broker:
            self.broker.stop()

    def close(self):
        self.stop()
        self.closed = True

    async def run(self):
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.OpenInputDesktop.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        user32.OpenInputDesktop.restype = wintypes.HANDLE
        user32.GetUserObjectInformationW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                    wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        user32.CloseDesktop.argtypes = [wintypes.HANDLE]
        last = 0.0
        appearance_checked = 0.0
        try:
            while not self.closed:
                now = time.monotonic()
                if now - appearance_checked >= 1.0:
                    self.appearance.refresh()
                    appearance_checked = now
                if now - last >= .25:
                    desktop = user32.OpenInputDesktop(0, False, 1)
                    label = ctypes.create_unicode_buffer(64)
                    used = wintypes.DWORD()
                    self.interactive = bool(desktop and user32.GetUserObjectInformationW(
                        desktop, 2, label, ctypes.sizeof(label), ctypes.byref(used)) and
                        label.value.casefold() == "default")
                    if desktop:
                        user32.CloseDesktop(desktop)
                    if not self.interactive or (last and now - last > 3):
                        self.stop()
                    last = now
                self.root.update()
                await asyncio.sleep(.02)
        finally:
            self.stop()


def encoder_formats(executable):
    native = ("jpeg", "cinepak-intra")
    if not executable:
        return native
    result = subprocess.run([executable, "-hide_banner", "-encoders"], capture_output=True,
                            timeout=5, check=True)
    names = {line.split()[1] for line in result.stdout.decode("utf-8", errors="replace").splitlines()
             if len(line.split()) >= 2 and line.lstrip().startswith("V")}
    return native + (("h264-intra",) if "libx264" in names else ())


async def run_listener(args, address, pin, context, leaf=None, parent_closed=None):
    providers = {"sim.xplane": XPlane()}
    if args.msfs and args.msfs.is_file():
        providers["sim.msfs2024"] = MSFS(args.msfs.resolve())
    picker = args.picker.resolve() if args.picker and args.picker.is_file() else None
    ffmpeg = shutil.which(args.ffmpeg)
    codecs = encoder_formats(ffmpeg) if picker else ()
    ui = Foreground(pin)
    if args.obs:
        ui.configure_obs(providers, args.obs_scene, args.obs_input)
    # The picker packaged next to this paired HOST uses PickSingleItemAsync for every request.
    system_picker = leaf is not None and picker == Path(__file__).resolve().with_name("kvmpad-capture.exe")
    if system_picker:
        ui.root.iconify()
    services = Services(ui.approve, ui.status, lambda: Capture(picker, ffmpeg), providers, codecs,
                        system_picker=system_picker)
    ui.broker = Broker(pin, services)
    try:
        async with listen(ui.broker, str(address), args.port, context) as server:
            ui.status("Waiting for a request from KVMpad. To share a display, open Cursor Magnifier "
                      "on the device and choose Start live view.")
            if leaf is None:
                await ui.run()
                return
            if parent_closed.is_set():
                return
            reply = {"port": server.sockets[0].getsockname()[1], "leaf": base64.b64encode(leaf).decode("ascii")}
            print(json.dumps(reply, separators=(",", ":")), flush=True)
            foreground = asyncio.create_task(ui.run())
            closed = asyncio.create_task(parent_closed.wait())
            try:
                await asyncio.wait((foreground, closed), return_when=asyncio.FIRST_COMPLETED)
                if foreground.done():
                    await foreground
            finally:
                ui.broker.stop()
                foreground.cancel()
                closed.cancel()
                await asyncio.gather(foreground, closed, return_exceptions=True)
    finally:
        ui.root.destroy()


def watch_parent(pipe, loop, closed):
    # Stdin carries only the startup line. Both EOF and further input retire this listener.
    # A daemon using unbuffered file I/O lets the foreground window exit with the parent alive.
    try:
        pipe.read(1)
    except OSError:
        pass
    finally:
        try:
            loop.call_soon_threadsafe(closed.set)
        except RuntimeError:
            pass  # The foreground window has already ended the event loop.


async def main(args):
    if os.name != "nt":
        raise SystemExit("The HOST requires an interactive Windows desktop.")
    pipe = os.fdopen(sys.stdin.fileno(), "rb", buffering=0, closefd=False)
    board, peer = read_board(pipe)
    address = route_address(peer)
    closed = asyncio.Event()
    threading.Thread(target=watch_parent, args=(pipe, asyncio.get_running_loop(), closed), daemon=True).start()
    pin, context, leaf = listener_identity(board, address)
    await run_listener(args, address, pin, context, leaf, closed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--paired-client", action="store_true", required=True, help="Use the active Windows CLIENT session")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--picker", type=Path, help="Locally built kvmpad-capture.exe")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--msfs", type=Path, help="Locally built kvmpad-msfs.exe")
    parser.add_argument("--obs", action="store_true")
    parser.add_argument("--obs-scene", help="Local scene permitted for obs.scene.select")
    parser.add_argument("--obs-input", help="Local input permitted for obs.input.mute/unmute")
    options = parser.parse_args()
    try:
        asyncio.run(main(options))
    except Exception:
        print("The paired HOST listener could not run.", file=sys.stderr)
        raise SystemExit(1)
