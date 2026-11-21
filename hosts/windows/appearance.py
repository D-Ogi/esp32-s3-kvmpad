# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Apply the Windows app palette and control artwork to KVMpad's Tk windows."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import tkinter as tk
from tkinter import ttk


class HighContrast(ctypes.Structure):
    _fields_ = [("size", wintypes.UINT), ("flags", wintypes.DWORD), ("scheme", wintypes.LPWSTR)]


def system_palette():
    import winreg
    user = ctypes.windll.user32
    hc = HighContrast()
    hc.size = ctypes.sizeof(hc)
    if user.SystemParametersInfoW(0x42, hc.size, ctypes.byref(hc), 0) and hc.flags & 1:
        def color(index):
            rgb = user.GetSysColor(index)
            return f"#{rgb & 255:02x}{rgb >> 8 & 255:02x}{rgb >> 16 & 255:02x}"
        return tuple(color(index) for index in (5, 8, 15, 18, 13, 14, 17))
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as key:
            light, _ = winreg.QueryValueEx(key, "AppsUseLightTheme")
    except OSError:
        light = True
    if light:
        return ("#f4f6f8", "#172332", "#ffffff", "#172332", "#15559a", "#ffffff", "#526174")
    return ("#16202b", "#ecf2f8", "#263442", "#ecf2f8", "#9bceff", "#12202d", "#b2c0cc")


class ControlAppearance:
    def __init__(self, root, badge, icon_name):
        self.root = root
        self.badge = badge
        self.icon_name = icon_name
        self.previous = None
        self.image = None
        self.style = ttk.Style(root)
        self.style.theme_use("clam")
        self.refresh()

    def refresh(self):
        palette = system_palette()
        size = max(16, min(96, round(self.root.winfo_fpixels("24p"))))
        if self.previous == (palette, size):
            return
        self.previous = (palette, size)
        bg, fg, face, ink, focus, selected, disabled = palette
        self.root.configure(background=bg)
        self.style.configure(".", background=bg, foreground=fg, fieldbackground=face,
                             selectbackground=focus, selectforeground=selected, focuscolor=focus)
        self.style.configure("TButton", background=face, foreground=ink,
                             bordercolor=disabled, lightcolor=face, darkcolor=face, padding=(8, 5))
        self.style.map("TButton", background=[("disabled", face), ("pressed", focus), ("active", focus)],
                       foreground=[("disabled", disabled), ("pressed", selected), ("active", selected)],
                       focuscolor=[("pressed", selected), ("active", selected)])
        self.style.configure("TEntry", fieldbackground=face, foreground=ink, insertcolor=ink,
                             bordercolor=disabled, lightcolor=face, darkcolor=face)
        self.style.map("TEntry", bordercolor=[("focus", focus)], lightcolor=[("focus", focus)],
                       selectbackground=[("!focus", ink)], selectforeground=[("!focus", face)])
        self.style.configure("TCombobox", arrowcolor=ink, bordercolor=disabled, background=face,
                             fieldbackground=face, foreground=ink, lightcolor=face, darkcolor=face)
        self.style.map("TCombobox", fieldbackground=[("readonly", face)], foreground=[("disabled", disabled), ("readonly", ink)],
                       selectbackground=[("readonly", face)], selectforeground=[("readonly", ink)],
                       arrowcolor=[("disabled", disabled), ("!disabled", ink)], background=[("active", face)],
                       bordercolor=[("focus", focus)], lightcolor=[("focus", focus)])
        for key, value in (("background", face), ("foreground", ink),
                           ("selectBackground", focus), ("selectForeground", selected)):
            self.root.option_add("*TCombobox*Listbox." + key, value)
        folder = Path(__file__).resolve().parent
        relative = Path("assets/control-icons") / (self.icon_name + ".png")
        packaged = folder.parent / relative
        source = packaged if packaged.is_file() else folder.parent.parent / "design" / relative
        if source.is_file():
            try:
                self.update_badge(source, size, bg, fg)
            except (OSError, tk.TclError):
                self.badge.configure(image="")
                self.image = None

    def update_badge(self, source, size, bg, fg):
        mask = tk.PhotoImage(master=self.root, file=source)
        if mask.width() != 96 or mask.height() != 96:
            self.badge.configure(image="")
            self.image = None
            return
        # A white mask on black gives its alpha coverage, including the antialiased edges.
        ppm = self.root.tk.call(str(mask), "data", "-background", "black", "-format", "ppm")
        coverage = ppm.split(b"\n", 3)[3][::3]
        surface = bytes.fromhex(bg[1:])
        ink_rgb = bytes.fromhex(fg[1:])
        ramp = [bytes((a * target + (255 - a) * base + 127) // 255
                      for target, base in zip(ink_rgb, surface)) for a in range(256)]
        width, height = mask.width(), mask.height()
        edges = [i * width // size for i in range(size + 1)]
        rows = [i * height // size for i in range(size + 1)]
        pixels = bytearray()
        for top, bottom in zip(rows, rows[1:]):
            for left, right in zip(edges, edges[1:]):
                total = sum(sum(coverage[y * width + left:y * width + right]) for y in range(top, bottom))
                pixels.extend(ramp[total // ((bottom - top) * (right - left))])
        header = f"P6\n{size} {size}\n255\n".encode("ascii")
        self.image = tk.PhotoImage(master=self.root, data=header + pixels, format="ppm")
        self.badge.configure(image=self.image, compound="left")
