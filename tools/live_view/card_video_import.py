# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Prepare an original H.264 video and its lossless LCD cache for the 7B video library."""

from concurrent.futures import Future
import os
from pathlib import Path
import sys
import threading
import tkinter as tk
from tkinter import filedialog, ttk
import uuid

from prepare_card_video import MODES, TEMPORAL_MODES, digest, prepare, publish_new
from stock_card_pixels import CARD_DEMUXERS

PLAYBACK_MODES = {
    "Exact": "exact",
    "Reduced": "reduced",
    "Large bank": "bank",
    "Temporal blend": "rr_blend",
    "Temporal smart loose": "rr_smart_loose",
}


def validate_library_source(source, mode):
    if mode not in MODES:
        raise ValueError("Choose a supported playback mode.")
    suffix = source.suffix.lower()
    if suffix not in CARD_DEMUXERS:
        raise ValueError("Choose an H.264 video with a .mp4, .h264 or .264 extension so it appears in Video files.")
    if mode in TEMPORAL_MODES and suffix != ".mp4":
        raise ValueError("Temporal playback requires a constant-frame-rate H.264 MP4.")


def import_video(source, folder, progress=None, cancelled=lambda: False, *, mode="exact", reader=None):
    source = Path(source).resolve()
    folder = Path(folder).resolve()
    if not folder.is_dir() or not source.is_file():
        raise ValueError("Choose a video and an existing destination folder.")
    if len(source.name.encode("utf-8")) > 100:
        raise ValueError("Shorten the video filename to 100 UTF-8 bytes for the board library.")
    validate_library_source(source, mode)
    if mode != "exact" and (reader is None or not Path(reader).is_file()):
        raise ValueError("Choose the KVMpad card-pixels.exe reader for this playback mode.")
    original = folder / source.name
    cache = folder / (source.name + MODES[mode][1])
    if cache.exists():
        raise FileExistsError(f"{cache.name} already exists. Choose another destination.")
    staging = folder / (".kvmpad-import-" + uuid.uuid4().hex)
    staging.mkdir()
    staged_source, staged_cache = staging / "source.tmp", staging / "cache.tmp"

    def check():
        if cancelled():
            raise InterruptedError("Preparation cancelled.")

    def frame(n):
        check()
        if progress:
            progress(n)

    try:
        check()
        if original.exists() and not source.samefile(original) and digest(original, check) != digest(source, check):
            raise FileExistsError(f"{original.name} already exists with different contents.")
        result = prepare(source, staged_cache, reader=reader, mode=mode, progress=frame, check_cancel=check)
        with staged_cache.open("rb") as stream:
            stream.seek(24)
            expected = stream.read(32)
        check()
        existing_original = original.exists()
        if not existing_original:
            with source.open("rb") as src, staged_source.open("xb") as dst:
                while chunk := src.read(1024 * 1024):
                    check()
                    dst.write(chunk)
                dst.flush()
                os.fsync(dst.fileno())
            if digest(staged_source, check) != expected:
                raise ValueError("The source changed during the copy.")
            check()
            try:
                publish_new(staged_source, original)
            except FileExistsError:
                # Another reconstruction mode may have just published the shared original.
                existing_original = True
        if existing_original:
            if digest(original, check) != expected:
                raise ValueError("The destination video differs from the prepared source.")
            check()
        publish_new(staged_cache, cache)
        return result
    finally:
        staged_source.unlink(missing_ok=True)
        staged_cache.unlink(missing_ok=True)
        staging.rmdir()


class ImportWindow:
    def __init__(self, root):
        self.root = root
        root.title("KVMpad · Prepare card video")
        body = ttk.Frame(root, padding=16)
        body.pack(fill="both", expand=True)
        body.columnconfigure(1, weight=1)
        self.source = tk.StringVar()
        self.folder = tk.StringVar()
        self.mode = tk.StringVar(value="Exact")
        self.reader = Path(__file__).resolve().with_name("card-pixels.exe")
        self.status = tk.StringVar(
            value="Choose an H.264 video and the root of a FAT32 or exFAT card, or a folder to copy later."
        )
        self.task = None
        self.closing = False
        self.stop = threading.Event()
        self.frame = 0
        for row, (name, var, command) in enumerate(
            (
                ("Video", self.source, self.choose_video),
                ("Destination", self.folder, self.choose_folder),
            )
        ):
            label = ttk.Label(body, text=name)
            label.grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
            if row == 0:
                video_label = label
            ttk.Entry(body, textvariable=var, width=48).grid(row=row, column=1, sticky="ew")
            ttk.Button(body, text="Browse…", command=command).grid(row=row, column=2, padx=(8, 0))
        ttk.Label(body, text="Playback mode").grid(row=2, column=0, sticky="w", padx=(0, 8))
        self.modes = ttk.Combobox(body, textvariable=self.mode, values=tuple(PLAYBACK_MODES), state="readonly")
        self.modes.grid(row=2, column=1, sticky="ew", pady=4)
        self.start = ttk.Button(body, text="Prepare playback", command=self.begin)
        self.start.grid(row=3, column=1, sticky="w", pady=10)
        self.cancel = ttk.Button(body, text="Cancel", command=self.stop.set, state="disabled")
        self.cancel.grid(row=3, column=2)
        ttk.Label(body, textvariable=self.status, wraplength=490).grid(
            row=4, column=0, columnspan=3, sticky="w"
        )
        root.protocol("WM_DELETE_WINDOW", self.close)
        if os.name == "nt":
            if not Path(__file__).with_name("appearance.py").is_file():
                sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "hosts/windows"))
            from appearance import ControlAppearance

            self.appearance = ControlAppearance(root, video_label, "video")
            self.palette_timer = root.after(1000, self.refresh_palette)
            root.bind("<Destroy>", self.end_palette_timer, add="+")
            self.modes.configure(postcommand=self.dropdown_colors)

    def refresh_palette(self):
        self.appearance.refresh()
        self.palette_timer = self.root.after(1000, self.refresh_palette)

    def end_palette_timer(self, event):
        if event.widget is self.root:
            self.root.after_cancel(self.palette_timer)

    def dropdown_colors(self):
        # Tk creates a separate listbox for the popdown and retains it across palette changes.
        style = self.appearance.style
        popdown = self.root.tk.call("ttk::combobox::PopdownWindow", self.modes)
        self.root.tk.call(str(popdown) + ".f.l", "configure",
                          "-background", style.lookup("TCombobox", "fieldbackground", ("readonly",)),
                          "-foreground", style.lookup("TCombobox", "foreground", ("readonly",)),
                          "-selectbackground", style.lookup(".", "selectbackground", ("focus",)),
                          "-selectforeground", style.lookup(".", "selectforeground", ("focus",)))

    def choose_video(self):
        path = filedialog.askopenfilename(
            parent=self.root,
            title="Choose H.264 video",
            filetypes=[("H.264 video", " ".join("*" + suffix for suffix in CARD_DEMUXERS))],
        )
        if path:
            self.source.set(path)

    def choose_folder(self):
        path = filedialog.askdirectory(parent=self.root, title="Choose a destination", mustexist=True)
        if path:
            self.folder.set(path)

    def begin(self):
        if self.task or not self.source.get() or not self.folder.get():
            return
        source, folder = Path(self.source.get()), Path(self.folder.get())
        label = self.mode.get()
        mode = PLAYBACK_MODES[label]
        try:
            validate_library_source(source, mode)
        except ValueError as error:
            self.status.set(str(error))
            return
        if mode != "exact" and not self.reader.is_file():
            path = filedialog.askopenfilename(parent=self.root, title="Choose the KVMpad card video reader",
                                              filetypes=[("KVMpad card-pixels.exe", "card-pixels.exe")])
            if not path:
                self.status.set("This playback mode requires the KVMpad card-pixels.exe reader.")
                return
            self.reader = Path(path)
        reader = self.reader if mode != "exact" else None
        button = {"Exact": "Play", "Temporal smart loose": "Smart loose"}.get(label, label)
        self.stop.clear()
        self.frame = 0
        self.task = Future()
        self.start.configure(state="disabled")
        self.modes.configure(state="disabled")
        self.cancel.configure(state="normal")

        def progress(n):
            self.frame = n

        def work():
            try:
                count, width, height = import_video(source, folder, progress, self.stop.is_set, mode=mode, reader=reader)
                cache_name = source.name + MODES[mode][1]
                self.task.set_result(
                    f"Ready. {cache_name} contains {count} frames at {width} × {height}.\n"
                    "Keep both files together in the root of the microSD card.\n"
                    f"On KVMpad, open Extensions → Video files, select {source.name} and tap {button}."
                )
            except Exception as error:
                self.task.set_exception(error)

        threading.Thread(target=work, name="card-video-import").start()
        self.poll()

    def poll(self):
        if not self.task.done():
            self.status.set("Cancelling…" if self.stop.is_set() else f"Preparing… {self.frame} frames")
            self.root.after(150, self.poll)
            return
        try:
            self.status.set(self.task.result())
        except FileNotFoundError as error:
            self.status.set(
                f"File or program missing: {error.filename or error}. Check the video, FFmpeg and ffprobe."
            )
        except Exception as error:
            self.status.set(str(error))
        self.task = None
        if self.closing:
            self.root.destroy()
        else:
            self.start.configure(state="normal")
            self.modes.configure(state="readonly")
            self.cancel.configure(state="disabled")

    def close(self):
        if self.task:
            self.closing = True
            self.stop.set()
        else:
            self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    ImportWindow(root)
    root.mainloop()
