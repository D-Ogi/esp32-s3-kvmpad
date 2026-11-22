# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Windows CLIENT board firmware dialog."""
import json
import shutil
import tempfile
from pathlib import Path
from queue import Empty, Queue
from threading import Thread
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from serial.tools import list_ports

from appearance import ControlAppearance
from firmware import install, load_bundle, update, validate_url
from firmware_image import prepare


class FirmwareWindow:
    def __init__(self, root):
        self.root = root
        root.title("KVMpad board firmware")
        root.minsize(540, 345)
        root.columnconfigure(0, weight=1)
        form = ttk.Frame(root, padding=18)
        form.grid(sticky="nsew")
        form.columnconfigure(0, weight=1)
        self.bundle = Path(__file__).with_name("7b")
        try:
            manifest = json.loads((self.bundle / "manifest.json").read_text(encoding="utf-8"))
            version = manifest["firmware"]
        except (KeyError, ValueError, OSError):
            version = None
        self.version = version
        self.local = None
        badge = ttk.Label(form, text="Waveshare ESP32-S3-Touch-LCD-7B", font=("Segoe UI", 13, "bold"))
        badge.grid(row=0, column=0, columnspan=2, sticky="w")
        self.appearance = ControlAppearance(root, badge, "settings")
        ttk.Label(form, text="Connect the USB-UART port to this PC. Leave the native USB port disconnected.").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(8, 12))
        ttk.Label(form, text="USB-UART port").grid(row=2, column=0, sticky="w")
        self.port = ttk.Combobox(form, state="readonly", width=53)
        self.port.grid(row=3, column=0, sticky="ew")
        self.refresh = ttk.Button(form, text="Refresh", command=self.refresh_ports)
        self.refresh.grid(row=3, column=1, padx=(8, 0))
        self.mode = tk.StringVar(value="install" if version else "update")
        self.wired = ttk.Radiobutton(form, variable=self.mode, value="install", command=self.select,
            text=f"Install or recover from this PC  ·  {version or 'no image installed'}")
        self.wired.grid(row=4, column=0, columnspan=2, sticky="w", pady=(14, 4))
        self.ota = ttk.Radiobutton(form, variable=self.mode, value="update", command=self.select,
                                   text="Update a running KVMpad from an HTTPS server")
        self.ota.grid(row=5, column=0, columnspan=2, sticky="w")
        self.url = ttk.Entry(form)
        self.url.grid(row=6, column=0, columnspan=2, sticky="ew", pady=(4, 8))
        self.hint = ttk.Label(form, wraplength=510)
        self.hint.grid(row=7, column=0, columnspan=2, sticky="w")
        self.status = ttk.Label(form, wraplength=510)
        self.status.grid(row=8, column=0, columnspan=2, sticky="w", pady=10)
        actions = ttk.Frame(form)
        actions.grid(row=9, column=0, columnspan=2, sticky="ew")
        actions.columnconfigure(2, weight=1)
        self.source = ttk.Button(actions, text="Use local packages…", command=self.choose_source)
        self.source.grid(row=0, column=0, padx=(0, 6))
        self.export = ttk.Button(actions, text="Save firmware…", command=self.save)
        self.export.grid(row=0, column=1)
        self.action = ttk.Button(actions, command=self.start)
        self.action.grid(row=0, column=2, sticky="e")
        self.events = Queue()
        self.busy = False
        self.port.bind("<<ComboboxSelected>>", lambda _: self.select())
        self.refresh_ports()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(125, self.poll)

    def refresh_ports(self):
        previous = self.port.get()
        self.ports = {f"{p.device}  ·  {p.description}": p.device for p in sorted(list_ports.comports())}
        self.port.configure(values=list(self.ports))
        self.port.set(previous if previous in self.ports else "")
        self.status.configure(text="Choose the port for this board." if self.ports else "No serial ports found. Connect USB-UART and select Refresh.")
        self.select()

    def select(self):
        wired = self.mode.get() == "install"
        self.wired.configure(state="normal" if self.version and not self.busy else "disabled")
        self.ota.configure(state="disabled" if self.busy else "normal")
        self.port.configure(state="disabled" if self.busy else "readonly")
        self.refresh.configure(state="disabled" if self.busy else "normal")
        for button in (self.source, self.export):
            button.configure(state="disabled" if self.busy or not self.version else "normal")
        self.url.configure(state="disabled" if self.busy or wired else "normal")
        self.hint.configure(text=("CLIENT prepares the image on this PC. The first run downloads the firmware packages and linker. Installation saves a full flash backup."
            if wired else "Enter a direct HTTPS URL for a signed kvmpad.bin. The board needs Wi-Fi. It keeps the previous firmware for rollback."))
        self.action.configure(text="Choose backup and install…" if wired else "Start signed update…",
                              state="normal" if self.port.get() in self.ports and not self.busy else "disabled")

    def choose_source(self):
        path = filedialog.askdirectory(parent=self.root, title="Choose the folder with both firmware ZIP files")
        if path:
            self.local = Path(path)
            self.status.configure(text="Local firmware packages selected.")

    def save(self):
        name = filedialog.asksaveasfilename(parent=self.root, initialfile=f"kvmpad-{self.version}.bin",
                                            defaultextension=".bin", filetypes=[("Signed OTA image", "*.bin")])
        if not name:
            return
        def export(report):
            image = prepare(self.bundle, self.local, report)
            load_bundle(self.bundle, image)
            if image.resolve() != Path(name).resolve():
                with tempfile.TemporaryDirectory(dir=Path(name).parent) as folder:
                    copy = Path(folder) / "kvmpad.bin"
                    shutil.copyfile(image, copy)
                    copy.replace(name)
            return f"Saved the verified firmware to {name}."
        self.run(export, ())

    def start(self):
        port = self.ports[self.port.get()]
        if self.mode.get() == "install":
            backup = filedialog.asksaveasfilename(parent=self.root, title="Save private board backup to a new file",
                defaultextension=".bin", initialfile=f"KVMpad-{port}-backup.bin", filetypes=[("Flash backup", "*.bin")])
            if not backup:
                return
            if Path(backup).exists():
                messagebox.showerror("Backup exists", "Choose a new filename to keep the previous backup.", parent=self.root)
                return
            question = (f"Install firmware {self.version} on the 7B connected to {port}?\n\n"
                        "This restarts the board. The installer saves all 16 MiB first. If the layout differs, "
                        "it erases the board, including settings and pairings. Keep the backup private.")
            args = (port, self.bundle, Path(backup), self.local)
            operation = install
        else:
            url = self.url.get().strip()
            try:
                validate_url(url)
            except ValueError as e:
                messagebox.showerror("Image URL", str(e), parent=self.root)
                return
            question = f"Start a signed update on {port}? KVMpad will end remote control and restart after verification."
            args = (port, url)
            operation = update
        if not messagebox.askokcancel("Confirm board firmware", question, parent=self.root, default="cancel"):
            return
        self.run(operation, args)

    def run(self, operation, args):
        self.busy = True
        self.select()
        self.status.configure(text="Starting…")
        def work():
            try:
                result = operation(*args, lambda value: self.events.put(("progress", value)))
                self.events.put(("done", result))
            except Exception as e:
                self.events.put(("error", str(e)))
        Thread(target=work, daemon=False).start()

    def poll(self):
        try:
            while True:
                event, message = self.events.get_nowait()
                self.status.configure(text=message)
                if event != "progress":
                    self.busy = False
                    self.select()
                    if event == "error":
                        messagebox.showerror("Board firmware", message, parent=self.root)
        except Empty:
            pass
        self.root.after(125, self.poll)

    def close(self):
        if self.busy:
            messagebox.showinfo("Board firmware", "Keep USB connected until the operation finishes.", parent=self.root)
        else:
            self.root.destroy()


if __name__ == "__main__":
    window = tk.Tk()
    FirmwareWindow(window)
    window.mainloop()
