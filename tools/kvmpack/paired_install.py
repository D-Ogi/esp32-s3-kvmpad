# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Use the installed Windows CLIENT pairing store and pinned session transport."""

import json
import os
from pathlib import Path
from queue import Empty, SimpleQueue
import shutil
import subprocess
import threading

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
PHASES = {
    b"transfer": "Sending the package to KVMpad.",
    b"review": "On KVMpad, review the package and permissions, then choose whether to install it.",
    b"committing": "KVMpad is opening the extension and checking its first screen.",
}


def client_path():
    here = Path(__file__).resolve()
    bundled = here.parent.parent / "KVMpad.Client.exe"
    if bundled.is_file():
        return bundled
    found = shutil.which("KVMpad.Client.exe")
    if found:
        return Path(found)
    checkout = here.parents[2] / "clients/windows/src/KVMpad.Client.App/bin"
    for config in ("Release", "Debug"):
        exe = checkout / config / "net10.0-windows/win-x64/KVMpad.Client.exe"
        if exe.is_file():
            return exe
    raise ValueError("Install the Windows CLIENT or build its Release configuration first.")


def paired_devices():
    try:
        reply = subprocess.run(
            [client_path(), "--pack-devices"],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=10,
            creationflags=NO_WINDOW,
        )
    except subprocess.TimeoutExpired as exc:
        raise ValueError("The Windows CLIENT pairing store timed out.") from exc
    if reply.returncode:
        raise ValueError("Cannot read the Windows CLIENT pairing store.")
    return json.loads(reply.stdout.decode("utf-8-sig"))


def start(device, *, status_output=subprocess.DEVNULL):
    if not device or len(device) > 128:
        raise ValueError("Choose a paired CLIENT device ID.")
    return subprocess.Popen(
        [client_path(), "--pack-install", device],
        stdin=subprocess.PIPE,
        stdout=status_output,
        stderr=subprocess.PIPE,
        creationflags=NO_WINDOW,
    )


def finish(process, archive):
    try:
        _, error = process.communicate(archive, timeout=510)
        if process.returncode:
            raise ValueError(
                error.decode("utf-8-sig", errors="replace").strip()[:240]
                or "Transfer interrupted. Check the device for the installation result."
            )
    except subprocess.TimeoutExpired as exc:
        raise ValueError("Transfer timed out. Check the device for the installation result.") from exc
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


def dialog(root, archive):
    import tkinter as tk
    from tkinter import ttk

    devices = paired_devices()
    if not devices:
        raise ValueError("Pair a device in the Windows CLIENT first.")
    top = tk.Toplevel(root)
    top.title("Install to paired device")
    top.transient(root)
    top.grab_set()
    body = ttk.Frame(top, padding=16)
    body.pack(fill="both", expand=True)
    options = ttk.Combobox(
        body, state="readonly", width=46, values=[f"{d['name']} ({d['id']})" for d in devices]
    )
    options.pack(fill="x")
    options.current(0)
    prompt = tk.StringVar(value="On KVMpad, open Extensions > Receive from paired client, then send the package.")
    ttk.Label(body, textvariable=prompt, wraplength=420).pack(fill="x", pady=10)
    actions = ttk.Frame(body)
    actions.pack(fill="x")
    process = None
    poll_timer = None
    done = threading.Event()
    error = []
    phases = SimpleQueue()

    def poll():
        nonlocal poll_timer
        poll_timer = None
        if done.is_set():
            prompt.set(error[0] if error else "Installation confirmed by the device.")
            cancel.config(text="Close")
        else:
            try:
                while True:
                    phase = phases.get_nowait()
                    prompt.set(PHASES[phase])
                    if phase == b"committing":
                        cancel.config(text="Close")
            except Empty:
                pass
            poll_timer = top.after(100, poll)

    def send():
        nonlocal process
        progress = None
        try:
            read_fd, write_fd = os.pipe()
            progress = os.fdopen(read_fd, "rb")
            with os.fdopen(write_fd, "wb") as output:
                process = start(devices[options.current()]["id"], status_output=output)
        except (OSError, ValueError) as exc:
            if progress is not None:
                progress.close()
            prompt.set(str(exc))
            return
        options.config(state="disabled")
        button.config(state="disabled")
        prompt.set("Connecting to KVMpad. When the package appears on its screen, review the permissions there.")

        def read_phases():
            with progress:
                for line in progress:
                    phase = line.rstrip(b"\r\n")
                    if phase in PHASES:
                        phases.put(phase)

        def worker():
            try:
                finish(process, archive)
            except (OSError, ValueError, subprocess.SubprocessError) as exc:
                error.append(str(exc))
            finally:
                done.set()

        threading.Thread(target=read_phases, name="pack-install-status").start()
        threading.Thread(target=worker, name="pack-install").start()
        poll()

    button = ttk.Button(actions, text="Send for device review", command=send)
    button.pack(side="left")
    cancel = ttk.Button(actions, text="Cancel", command=top.destroy)
    cancel.pack(side="right")
    top.protocol("WM_DELETE_WINDOW", top.destroy)

    def destroyed(event):
        if event.widget is not top:
            return
        if poll_timer is not None:
            top.after_cancel(poll_timer)
        if process and process.poll() is None:
            process.kill()

    top.bind("<Destroy>", destroyed)
    root.wait_window(top)
