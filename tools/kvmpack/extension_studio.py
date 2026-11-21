# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Offline layout and binding editor for KVMpad extension packs."""

from copy import copy
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog, messagebox, ttk
from typing import NamedTuple
import webbrowser
from zipfile import BadZipFile

import kvmpack
from card import stage_installer, write_file

EXAMPLES = Path(__file__).with_name("examples")
USB_CONSOLE = EXAMPLES / "usb-monitor-console"
if not USB_CONSOLE.is_dir():
    USB_CONSOLE = Path(__file__).resolve().parents[2] / "extensions" / "usb-monitor-console"
WIDTH, HEIGHT = 1024, 568
PALETTE = {
    "Column": (320, 180, {"layout": "absolute"}),
    "Row": (320, 160, {"layout": "absolute"}),
    "Grid": (360, 200, {"layout": "absolute"}),
    "Text": (180, 56, {"text": "Text"}),
    "Button": (180, 56, {"label": "Button"}),
    "StatusChip": (180, 52, {"label": "Status", "status": "neutral"}),
    "TouchArea": (200, 100, {"label": "Touch area"}),
    "AxisPad": (200, 180, {"title": "Axis pad", "axes": ["X", "Y"], "spring_center": True}),
    "Dial": (144, 160, {"label": "Dial", "min": 0, "max": 100, "value": 0}),
    "GaugeNeedle": (154, 144, {"title": "Gauge", "min": 0, "max": 100}),
    "GaugeArc": (154, 144, {"title": "Gauge", "min": 0, "max": 100}),
    "Horizon": (200, 150, {"title": "Horizon"}),
    "ValueTile": (180, 80, {"title": "Value"}),
    "StreamViewport": (512, 512, {"stream": "cursor_region", "fit": "native",
                                  "source_size": [512, 512], "mode": "PIXEL_1X"}),
    "Slider": (200, 64, {"title": "Slider", "min": 0, "max": 1, "orientation": "horizontal"}),
    "Spacer": (48, 48, {}),
    "TabStrip": (288, 56, {"tabs": ["Tab 1", "Tab 2"], "selected": 0}),
    "ChartViewer": (580, 780, {}),
    "AutopilotPanel": (584, 864, {}),
    "RadioPanel": (584, 864, {}),
    "MonitorViewport": (584, 438, {}),
    "KeyboardSurface": (584, 288, {}),
    "TouchpadSurface": (584, 186, {}),
}
SURFACE_LABELS = {"MonitorViewport": "USB desktop", "KeyboardSurface": "Keyboard", "TouchpadSurface": "Touchpad"}
WIDGET_EVENTS = {
    "Button": ("tap", "press", "release"), "TouchArea": ("tap", "drag", "long_press"),
    "Dial": ("rotate",), "TabStrip": ("change",), "AxisPad": ("change",), "Slider": ("change",),
}


class LayoutColors(NamedTuple):
    page: str
    text: str
    control: str
    control_text: str
    selection: str
    selection_text: str
    edge: str


DAY = LayoutColors("#f4f6f8", "#172332", "#ffffff", "#172332", "#15559a", "#ffffff", "#526174")
NIGHT = LayoutColors("#16202b", "#ecf2f8", "#263442", "#ecf2f8", "#9bceff", "#12202d", "#b2c0cc")


class HighContrast(ctypes.Structure):
    _fields_ = [("size", wintypes.UINT), ("flags", wintypes.DWORD), ("scheme", wintypes.LPWSTR)]


def layout_colors():
    if os.name != "nt":
        return DAY
    import winreg

    user = ctypes.windll.user32
    contrast = HighContrast()
    contrast.size = ctypes.sizeof(contrast)
    if user.SystemParametersInfoW(0x42, contrast.size, ctypes.byref(contrast), 0) and contrast.flags & 1:
        def color(index):
            rgb = user.GetSysColor(index)
            return f"#{rgb & 255:02x}{rgb >> 8 & 255:02x}{rgb >> 16 & 255:02x}"

        # Keep the foreground/background pairs from the user's contrast scheme.
        return LayoutColors(*(color(i) for i in (5, 8, 15, 18, 13, 14, 8)))
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as key:
            light, _ = winreg.QueryValueEx(key, "AppsUseLightTheme")
    except OSError:
        return DAY
    return DAY if light else NIGHT


def formatted(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def walk(node):
    yield node
    for child in node.get("children", ()):
        yield from walk(child)


def scene_layout(node, parent_size=(WIDTH, HEIGHT), origin=(0, 0)):
    bounds = node["bounds"]
    position = (origin[0] + bounds["x"], origin[1] + bounds["y"])
    yield node, bounds, parent_size, position
    for child in node.get("children", ()):
        yield from scene_layout(child, (bounds["w"], bounds["h"]), position)


def placement(parent, size, width, height):
    occupied = [child["bounds"] for child in parent.get("children", ())]
    xs = sorted({0, *(b["x"] + b["w"] + 8 for b in occupied)})
    ys = sorted({0, *(b["y"] + b["h"] + 8 for b in occupied)})
    for y in ys:
        for x in xs:
            if x + width > size[0] or y + height > size[1]:
                continue
            if all(x + width <= b["x"] or x >= b["x"] + b["w"] or
                   y + height <= b["y"] or y >= b["y"] + b["h"] for b in occupied):
                return {"x": x, "y": y, "w": width, "h": height}
    return {"x": 0, "y": 0, "w": width, "h": height}


class Project:
    def __init__(self, members, verified_archive=None):
        self.members = dict(members)
        self.manifest = kvmpack.validate_scene(self.members, editing=True)
        self.verified_archive = verified_archive
        self.signed_members = dict(members) if verified_archive is not None else None

    def doc(self, name):
        return kvmpack.parse_json(self.members[name], name)

    def update(self, documents):
        next_members = {**self.members, **documents}
        for name, data in documents.items():
            kvmpack.require(
                name in kvmpack.DOCUMENTS and len(data) <= kvmpack.FILE_LIMIT,
                f"{name}: document exceeds limit",
            )
        manifest = kvmpack.validate_scene(next_members, editing=True)
        self.members, self.manifest = next_members, manifest

    def import_icon(self, path):
        name = "assets/" + path.name
        kvmpack.check_path(name)
        with path.open("rb") as stream:
            png = stream.read(kvmpack.EXPANDED_LIMIT + 1)
        if name in self.members and self.members[name] == png:
            return name
        kvmpack.require(name.casefold() not in {p.casefold() for p in self.members},
                        "An asset with that name already exists. Rename the PNG before importing it.")
        kvmpack.require(sum(map(len, self.members.values())) + len(png) <= kvmpack.EXPANDED_LIMIT and
                        len(self.members) + 2 <= kvmpack.ENTRY_LIMIT, "Package asset limit exceeded.")
        kvmpack.check_png(png, name)
        self.members = {**self.members, name: png}
        return name

    def edit_widget(self, widget, bounds, label, event="", action="", source="", target="value", chip_role="", icon=None):
        ui, bindings = self.doc("ui.json"), self.doc("bindings.json")
        nodes = {node["id"]: node for node in walk(ui["root"])}
        node = nodes[widget]
        node["bounds"] = dict(bounds)
        props = node.setdefault("props", {})
        key = next(
            (k for k in ("text", "title", "label") if k in props),
            "text" if node["type"] == "Text" else "label",
        )
        if label or key in props:
            props[key] = label
        if node["type"] == "StatusChip" and chip_role and chip_role != props.get("status", "neutral"):
            props["status"] = chip_role
        if node["type"] == "Button" and icon is not None:
            if icon:
                props["icon"] = icon
            else:
                props.pop("icon", None)
        if action:
            definition = next((a for a in bindings["actions"] if a["id"] == action), None)
            kvmpack.require(definition is not None, "Select an existing action from Bindings.")
            events = node.setdefault("events", {})
            kvmpack.require(
                not events.get(event) or events[event] == action,
                "The target event already has an action.",
            )
            old_id, old_event = definition["trigger"].split(".")
            nodes[old_id]["events"].pop(old_event)
            events[event] = action
            definition["trigger"] = widget + "." + event
        if source:
            endpoint = widget + "." + target
            conn = next((c for c in bindings["connections"] if c["to"] == endpoint), None)
            if conn:
                conn["from"] = source
            else:
                bindings["connections"].append(
                    {"from": source, "to": endpoint, "transform": "identity"}
                )
        self.update({"ui.json": formatted(ui), "bindings.json": formatted(bindings)})

    def add_widget(self, kind, parent_id=None, chart_paths=()):
        kvmpack.require(kind in PALETTE, "Choose a widget from the palette.")
        ui = self.doc("ui.json")
        parent_id = parent_id or ui["root"]["id"]
        nodes = {node["id"]: node for node in walk(ui["root"])}
        parent = nodes.get(parent_id)
        kvmpack.require(parent and parent["type"] in kvmpack.CONTAINERS,
                        "Choose a Column, Row or Grid as parent.")
        size = (parent["bounds"]["w"], parent["bounds"]["h"])
        width, height, props = PALETTE[kind]
        assets = {}
        if kind == "ChartViewer":
            kvmpack.require(2 <= len(chart_paths) <= 3, "Choose two or three charts.")
            props = {"charts": []}
            for index, path in enumerate(chart_paths, 1):
                title = path.stem.encode("utf-8")[:48].decode("utf-8", errors="ignore")
                data = kvmpack.prepare_chart(path)
                digest = hashlib.sha256(data).hexdigest()[:16]
                name = f"assets/charts/{index}-{digest}.png"
                kvmpack.require(name.casefold() not in {n.casefold() for n in (*assets, *self.members)} or
                                self.members.get(name) == data, "Choose chart files with distinct names.")
                assets[name] = data
                props["charts"].append({"title": title, "asset": name})
        if kind == "StreamViewport":
            kvmpack.require("host.screen.region" in self.manifest["permissions"] and
                            "capture.windows" in self.manifest["host"]["requires"],
                            "StreamViewport needs host.screen.region and capture.windows in the Manifest. See the Cursor Magnifier template.")
            kvmpack.require(width <= size[0] and height <= size[1],
                            "Choose a parent with room for a 512 × 512 native viewport.")
        if kind in ("Slider", "AxisPad"):
            kvmpack.require("hid.joystick" in self.manifest["permissions"] and
                            "hid.joystick" in self.manifest["usb"]["requires"],
                            f"{kind} needs hid.joystick in the Manifest permissions and USB requirements. See the Flight Yoke template.")
        number = 1
        while f"{kind.lower()}_{number}" in nodes:
            number += 1
        name = f"{kind.lower()}_{number}"
        bounds = placement(parent, size, min(width, size[0]), min(height, size[1]))
        parent.setdefault("children", []).append({"id": name, "type": kind, "bounds": bounds, "props": props})
        if assets:
            candidate = {**self.members, **assets, "ui.json": formatted(ui)}
            kvmpack.validate_scene(candidate, editing=True)
            self.members = candidate
        else:
            self.update({"ui.json": formatted(ui)})
        return name

    def package(self):
        kvmpack.validate_scene(self.members)
        if self.verified_archive is not None:
            kvmpack.require(self.members == self.signed_members,
                            "This edit changes the signed package. Use File → Save source as… to create an unsigned project.")
            return self.verified_archive
        return kvmpack.build(self.members)

    def diagnostics(self):
        bindings = self.doc("bindings.json")
        width, height = kvmpack.content_size(self.doc("ui.json"))
        report = {
            "validator": "kvmpack",
            "status": "valid",
            "schema_version": self.manifest["schema_version"],
            "board_px": [width, height + 32],
            "content_px": [width, height],
            "widgets": sum(1 for _ in walk(self.doc("ui.json")["root"])),
            "actions": len(bindings["actions"]),
            "sources": len(bindings["sources"]),
            "permissions": self.manifest["permissions"],
        }
        try:
            report["package_bytes"] = len(self.package())
        except kvmpack.ProfileBudgetError as error:
            report.update(status="invalid", max_memory_kib=error.declared_kib,
                          minimum_load_kib=error.minimum_kib)
        return report


class Editor:
    def __init__(self, root, source, verify_key=None):
        self.root = root
        root.title("KVMpad Extension Studio")
        root.geometry("1150x720")
        root.minsize(800, 540)
        self.current = self.selected = self.drag = None
        self.saved = {}
        self.documents = {}
        self.preview_dir = None
        self.preview_number = 0
        top = ttk.Frame(root, padding=(12, 8))
        top.pack(fill="x")
        self.title = ttk.Label(top, font=("Segoe UI", 14, "bold"))
        self.title.pack(side="left", padx=(0, 12))
        for label, command in (
            ("Validate", self.validate),
            ("Simulate", self.simulate),
            ("Package…", self.package),
            ("Install to paired device…", self.install_paired),
            ("Install via card…", self.install),
            ("Permissions", self.permissions),
        ):
            ttk.Button(top, text=label, command=lambda fn=command: self.run(fn)).pack(
                side="left", padx=2
            )
        menu = tk.Menu(root)
        file_menu = tk.Menu(menu, tearoff=False)
        templates = tk.Menu(file_menu, tearoff=False)
        for path in sorted(EXAMPLES.iterdir()):
            if path.is_dir():
                templates.add_command(
                    label=path.name, command=lambda p=path: self.run(lambda: self.open(p))
                )
        file_menu.add_cascade(label="New from template", menu=templates)
        file_menu.add_command(label="Open USB Monitor Console example", command=lambda: self.run(lambda: self.open(USB_CONSOLE)))
        file_menu.add_command(label="Open source folder…", command=lambda: self.run(self.open_folder))
        file_menu.add_command(label="Open package…", command=lambda: self.run(self.open_archive))
        file_menu.add_command(label="Save source as…", command=lambda: self.run(self.save_source))
        file_menu.add_command(label="Export diagnostics…", command=lambda: self.run(self.diagnostics))
        menu.add_cascade(label="File", menu=file_menu)
        root.config(menu=menu)
        panes = ttk.Panedwindow(root, orient="horizontal")
        panes.pack(fill="both", expand=True, padx=12)
        layout = ttk.Frame(panes)
        panes.add(layout, weight=3)
        layout_header = ttk.Frame(layout)
        layout_header.pack(fill="x")
        self.layout_title = ttk.Label(layout_header, padding=(0, 6))
        self.layout_title.pack(side="left")
        self.screen_choice = ttk.Combobox(layout_header, state="readonly")
        self.screen_choice.bind("<<ComboboxSelected>>", lambda _: self.run(
            lambda: self.choose(self.screen_ids[self.screen_choice.current()])))
        self.layout_colors = layout_colors()
        self.canvas = tk.Canvas(layout, bg=self.layout_colors.page, highlightthickness=0, width=640, height=375)
        self.canvas.pack(fill="both", expand=True)
        palette = ttk.Frame(layout, padding=(0, 8))
        palette.pack(fill="x")
        self.widget_kind = ttk.Combobox(palette, values=tuple(PALETTE), state="readonly", width=15)
        self.widget_kind.set("Text")
        self.widget_kind.pack(side="left")
        ttk.Label(palette, text="Parent", padding=(8, 0, 4, 0)).pack(side="left")
        self.parent_choice = ttk.Combobox(palette, state="readonly", width=16)
        self.parent_choice.pack(side="left")
        self.add_button = ttk.Button(palette, text="Add widget",
                                     command=lambda: self.run(lambda: self.add(self.widget_kind.get())))
        self.add_button.pack(side="left", padx=4)
        self.canvas.bind("<Configure>", lambda _: self.draw())
        self.canvas.bind("<ButtonPress-1>", self.press)
        self.canvas.bind("<ButtonRelease-1>", self.release)
        self.canvas.bind("<B1-Motion>", self.motion)
        self.notebook = notebook = ttk.Notebook(panes)
        panes.add(notebook, weight=2)
        inspector = ttk.Frame(notebook, padding=12)
        notebook.add(inspector, text="Widget")
        self.selection = ttk.Combobox(inspector, state="readonly")
        self.selection.pack(fill="x")
        self.selection.bind(
            "<<ComboboxSelected>>", lambda _: self.run(lambda: self.choose(self.selection.get()))
        )
        self.fields = {}
        self.combos = {}
        for key, label, values in [
            ("label", "Displayed text", None),
            ("icon", "Button icon", ()),
            ("chip_role", "Status chip role", tuple(kvmpack.CHIP_SYMBOLS)),
            ("x", "X", None),
            ("y", "Y", None),
            ("w", "Width", None),
            ("h", "Height", None),
            ("event", "Event", ["tap", "press", "release", "change"]),
            ("action", "Move action to event", []),
            ("target", "Binding target", ["value", "text", "pitch", "roll"]),
            ("source", "Bind source or state", []),
        ]:
            row = ttk.Frame(inspector)
            row.pack(fill="x", pady=4)
            ttk.Label(row, text=label, width=19).pack(side="left")
            var = self.fields[key] = tk.StringVar()
            if values is None:
                control = ttk.Entry(row, textvariable=var)
                if key == "label":
                    self.label_entry = control
            else:
                control = ttk.Combobox(row, textvariable=var, values=values, state="readonly")
                self.combos[key] = control
            if key == "icon":
                self.import_button = ttk.Button(row, text="Import PNG…", command=lambda: self.run(self.choose_icon))
                self.import_button.pack(side="right")
            control.pack(fill="x", expand=True)
        ttk.Button(
            inspector, text="Apply widget changes", command=lambda: self.run(self.apply_widget)
        ).pack(anchor="w", pady=8)
        for name in kvmpack.DOCUMENTS:
            frame = ttk.Frame(notebook)
            notebook.add(frame, text=Path(name).stem.capitalize())
            box = tk.Text(frame, wrap="none", undo=True, font=("Consolas", 10))
            scroll = ttk.Scrollbar(frame, command=box.yview)
            scroll.pack(side="right", fill="y")
            box.config(yscrollcommand=scroll.set)
            box.pack(fill="both", expand=True)
            self.documents[name] = box
        self.status = tk.StringVar(value="")
        ttk.Label(root, textvariable=self.status, padding=(12, 8), wraplength=1000).pack(fill="x")
        self.open(source, verify_key)
        if self.current is None:
            self.open(EXAMPLES / "flight-checklist")
        if os.name == "nt":
            self.palette_timer = self.canvas.after(1000, self.poll_layout_colors)
            self.canvas.bind("<Destroy>", lambda _: self.canvas.after_cancel(self.palette_timer))
        root.protocol("WM_DELETE_WINDOW", self.close)

    def run(self, fn):
        try:
            return fn()
        except (OSError, ValueError, KeyError, BadZipFile, RecursionError) as error:
            self.status.set(str(error))
            if isinstance(error, kvmpack.DocumentError) and error.name in self.documents:
                box = self.documents[error.name]
                if box.get("1.0", "end-1c").encode("utf-8") == error.data:
                    self.notebook.select(box.master)
                    start = f"{error.line}.0"
                    prefix = box.get(start, start + " lineend")[:error.column - 1]
                    pos = f"{error.line}.{self.root.tk.call('string', 'length', prefix)}"
                    box.mark_set("insert", pos)
                    box.see(pos)
                    box.focus_set()
            self.root.bell()

    def drafts(self):
        return {name: box.get("1.0", "end-1c").encode("utf-8") for name, box in self.documents.items()}

    def discard(self):
        return (
            not self.current
            or (self.drafts() == self.saved and not self.widget_pending())
            or messagebox.askyesno("Extension Studio", "Discard unsaved edits?", parent=self.root)
        )

    def open(self, path, verify_key=None):
        if not path or not self.discard():
            return
        path = Path(path)
        try:
            members, archive = kvmpack.read_package(path, verify_key)
        except kvmpack.PublisherKeyRequired:
            key_file = filedialog.askopenfilename(parent=self.root, title="Choose the trusted publisher's Ed25519 public key",
                                                  filetypes=[("Public PEM key", "*.pem"), ("All files", "*")])
            if not key_file:
                return
            verify_key = kvmpack.read_public_key(Path(key_file))
            members, archive = kvmpack.read_package(path, verify_key)
        project = Project(members, archive if verify_key is not None else None)
        try:
            kvmpack.check_memory_budget(members, project.manifest["limits"]["max_memory_kib"],
                                        kvmpack.button_icons(project.doc("ui.json")["root"]))
        except kvmpack.ProfileBudgetError as error:
            status = f"Opened for editing. {error}"
        else:
            status = f"Verified publisher signature for {path.name}" if verify_key else f"Opened {path.name}"
        self.current = project
        self.refresh()
        self.saved = self.drafts()
        self.status.set(status)

    def open_folder(self):
        self.open(filedialog.askdirectory(parent=self.root, title="Open source folder"))

    def open_archive(self):
        self.open(
            filedialog.askopenfilename(parent=self.root, filetypes=[("Extension pack", "*.kvmpack")])
        )

    def widget_pending(self):
        return self.selected and self.inspected != {k: v.get() for k, v in self.fields.items()}

    def sync(self):
        if self.widget_pending():
            self.apply_widget()
            return
        data = self.drafts()
        if any(data[n] != self.current.members[n] for n in data):
            self.current.update(data)
            self.refresh()

    def refresh(self, selection=None):
        self.title.config(text=self.current.manifest["title"])
        for name, box in self.documents.items():
            text = self.current.members[name].decode("utf-8")
            if box.get("1.0", "end-1c") != text:
                box.delete("1.0", "end")
                box.insert("1.0", text)
                box.edit_reset()
        ui = self.current.doc("ui.json")
        self.scene = ui["root"]
        self.content_size = kvmpack.content_size(ui)
        self.screen_ids = ui.get("screens", [])
        width, height = self.content_size
        self.scene_size = (width, height - 48 if self.screen_ids else height)
        self.layout_title.config(text=f"Layout  ·  {self.content_size[0]} × {self.content_size[1] + 32}")
        self.icon_thumbs = {name: kvmpack.icon_thumbnail(self.current.members[name])
                            for name in set(kvmpack.button_icons(self.scene))}
        self.nodes = {node["id"]: node for node in walk(self.scene)}
        self.screen_of = {node["id"]: page for page in self.screen_ids for node in walk(self.nodes[page])}
        if self.screen_ids:
            self.screen_choice.config(values=[self.nodes[p]["props"]["title"] for p in self.screen_ids])
            self.screen_choice.pack(side="right")
        else:
            self.screen_choice.pack_forget()
        parents = [name for name, node in self.nodes.items() if node["type"] in kvmpack.CONTAINERS]
        self.parent_choice.config(values=parents)
        if self.parent_choice.get() not in parents:
            self.parent_choice.set(next(iter(parents), ""))
        self.add_button.state(["!disabled"] if parents else ["disabled"])
        self.selection.config(values=list(self.nodes))
        first = self.screen_ids[0] if self.screen_ids else next((n for n in self.nodes if n != self.scene["id"]), self.scene["id"])
        self.select(
            next((s for s in (selection, self.selected) if s in self.nodes), first)
        )

    def choose(self, name):
        self.selection.set(self.selected or "")
        self.sync()
        if name in self.nodes:
            self.select(name)

    def select(self, name):
        self.selected = name
        if self.screen_ids:
            self.active_page = self.screen_of.get(name, self.screen_ids[0])
            self.screen_choice.current(self.screen_ids.index(self.active_page))
        self.selection.set(name or "")
        if name:
            node = self.nodes[name]
            props = node.get("props", {})
            displays_label = node["type"] not in kvmpack.CONTAINERS | kvmpack.USB_SURFACES.keys() | {"Spacer", "StreamViewport", "TabStrip", "ChartViewer", "AutopilotPanel", "RadioPanel"}
            self.label_entry.config(state="normal" if displays_label else "disabled")
            chip = node["type"] == "StatusChip"
            button = node["type"] == "Button"
            self.combos["icon"].config(state="readonly" if button else "disabled",
                                      values=["", *sorted(p for p in self.current.members if p.startswith("assets/"))])
            self.fields["icon"].set(props.get("icon", "") if button else "")
            self.import_button.config(state="normal" if button else "disabled")
            self.combos["chip_role"].config(state="readonly" if chip else "disabled")
            self.fields["chip_role"].set(props.get("status", "neutral") if chip else "")
            self.fields["label"].set(
                next((props[k] for k in ("text", "title", "label") if k in props), "")
            )
            _, bounds, self.parent_size, _ = next(row for row in scene_layout(self.scene, self.scene_size) if row[0]["id"] == name)
            for k, v in bounds.items():
                self.fields[k].set(v)
            bindings = self.current.doc("bindings.json")
            actions = bindings["actions"]
            if node["type"] in kvmpack.LOCAL_CONTROLS:
                actions = [a for a in actions if a["type"] == "state.set"]
            self.combos["action"].config(values=[""] + [a["id"] for a in actions])
            self.combos["source"].config(
                values=[""] + [s["id"] for s in bindings["sources"] + bindings["state"]]
            )
            events = WIDGET_EVENTS.get(node["type"], ())
            targets = kvmpack.BINDING_FIELDS.get(node["type"], ())
            self.combos["event"].config(values=events)
            self.combos["target"].config(values=targets)
            self.combos["action"].config(state="readonly" if events else "disabled")
            self.combos["source"].config(state="readonly" if targets else "disabled")
            self.fields["event"].set(next(iter(node.get("events", {})), next(iter(events), "")))
            self.fields["target"].set(next(iter(targets), ""))
            self.fields["action"].set("")
            self.fields["source"].set("")
        self.inspected = {k: v.get() for k, v in self.fields.items()}
        self.draw()

    def poll_layout_colors(self):
        colors = layout_colors()
        if colors != self.layout_colors:
            self.layout_colors = colors
            self.canvas.configure(bg=colors.page)
            self.draw()
        self.palette_timer = self.canvas.after(1000, self.poll_layout_colors)

    def draw(self):
        if not self.current or not hasattr(self, "nodes"):
            return
        c = self.canvas
        c.delete("all")
        self.canvas_icons = {}
        W, H = max(1, c.winfo_width()), max(1, c.winfo_height())
        width, content_height = self.content_size
        height = content_height + 32
        self.scale = min(W / width, H / height)
        self.ox = (W - width * self.scale) / 2
        self.oy = (H - height * self.scale) / 2
        x, y, s = self.ox, self.oy, self.scale
        colors = self.layout_colors
        c.create_rectangle(x, y, x + width * s, y + height * s, fill=colors.page, outline="")
        c.create_rectangle(x, y, x + width * s, y + 32 * s, fill=colors.control, outline="")
        c.create_text(
            x + 8,
            y + 16 * s,
            text=("" if self.screen_ids else "Home    |    ") + self.current.manifest["title"],
            fill=colors.control_text,
            anchor="w",
        )
        selection_rect = None
        for node, b, _, (px, py) in scene_layout(self.scene, self.scene_size):
            name = node["id"]
            if name in self.screen_of and self.screen_of[name] != self.active_page:
                continue
            left = x + px * s
            top = y + (32 + py) * s
            tag = "node:" + name
            rect = (left, top, left + b["w"] * s, top + b["h"] * s)
            container = node["type"] in kvmpack.CONTAINERS
            fill, ink = (colors.page, colors.text) if container else (colors.control, colors.control_text)
            if name == self.selected:
                selection_rect = rect
                if not container:
                    fill, ink = colors.selection, colors.selection_text
            c.create_rectangle(*rect, fill=fill, outline=colors.edge, tags=(tag,))
            if container:
                continue
            p = node.get("props", {})
            chip = node["type"] == "StatusChip"
            caption = p.get("text") or p.get("label") or p.get("title") or SURFACE_LABELS.get(node["type"], name)
            if node["type"] == "Button" and p.get("icon") and b["w"] >= 64 and b["h"] >= 40:
                from PIL import Image, ImageTk
                key = p["icon"]
                thumb = self.icon_thumbs[key]
                if key not in self.canvas_icons:
                    size = tuple(max(1, round(n * s)) for n in thumb.size)
                    self.canvas_icons[key] = ImageTk.PhotoImage(thumb.resize(size, Image.Resampling.NEAREST), master=c)
                middle = top + b["h"] * s / 2
                c.create_image(left + 6*s, middle, anchor="w", image=self.canvas_icons[key], tags=(tag,))
                label_x = left + (12 + thumb.width)*s
                available = rect[2] - 6*s - label_x
                font = tkfont.nametofont("TkDefaultFont", root=c)
                if font.measure(caption) > available:
                    while caption and font.measure(caption + "…") > available:
                        caption = caption[:-1]
                    caption = caption + "…" if font.measure("…") <= available else ""
                c.create_text(label_x, middle, text=caption, anchor="w", fill=ink, font=font, tags=(tag,))
                continue
            if chip:
                c.create_text(left + 16 * s, top + b["h"] * s / 2,
                              text=kvmpack.CHIP_SYMBOLS[p.get("status", "neutral")], fill=ink, tags=(tag,))
            c.create_text(
                left + (28 if chip else b["w"] / 2) * s,
                top + b["h"] * s / 2,
                text=caption,
                width=max(1, (b["w"] - 34) * s if chip else b["w"] * s - 6),
                anchor="w" if chip else "center",
                fill=ink,
                tags=(tag,),
            )
        if self.screen_ids:
            c.create_rectangle(x, y+(height-48)*s, x+width*s, y+height*s, fill=colors.control, outline=colors.edge)
            c.create_text(x+width*s/2, y+(height-24)*s, text="⌃  Screens", fill=colors.control_text)
        if selection_rect:
            # Keep the selected group's outline above its children, without intercepting drags.
            c.create_rectangle(*selection_rect, outline=colors.text, width=3, dash=(4, 3),
                               tags=("selection",), state="disabled")

    def press(self, e):
        tags = self.canvas.gettags("current")
        name = next((t[5:] for t in tags if t.startswith("node:")), None)
        if name:
            self.run(lambda: self.choose(name))
            if self.selected == name and not self.widget_pending():
                self.drag = (e.x, e.y, dict(self.nodes[name]["bounds"]))

    def motion(self, e):
        if not self.drag:
            return
        x, y, b = self.drag
        left = max(0, min(self.parent_size[0] - b["w"], b["x"] + round((e.x - x) / self.scale)))
        top = max(0, min(self.parent_size[1] - b["h"], b["y"] + round((e.y - y) / self.scale)))
        bounds = {**b, "x": left, "y": top}
        if self.nodes[self.selected]["bounds"] == bounds:
            return
        self.nodes[self.selected]["bounds"] = bounds
        self.fields["x"].set(left)
        self.fields["y"].set(top)
        self.draw()

    def release(self, e):
        if self.drag:
            _, _, original = self.drag
            self.motion(e)
            self.drag = None
            if self.nodes[self.selected]["bounds"] != original:
                self.run(self.apply_widget)

    def choose_icon(self):
        path = filedialog.askopenfilename(title="Import a button icon", filetypes=[("PNG images", "*.png")])
        if path:
            self.apply_widget(Path(path))

    def apply_widget(self, icon_path=None):
        if not self.selected:
            raise ValueError("Select a widget.")
        name = self.selected
        values = {k: v.get() for k, v in self.fields.items()}
        candidate = copy(self.current)
        candidate.update(self.drafts())
        if icon_path is not None:
            values["icon"] = candidate.import_icon(icon_path)
        bounds = {k: int(values[k]) for k in ("x", "y", "w", "h")}
        candidate.edit_widget(
            name,
            bounds,
            values["label"],
            values["event"],
            values["action"],
            values["source"],
            values["target"],
            values["chip_role"],
            values["icon"],
        )
        self.current = candidate
        self.refresh(name)
        self.status.set("Widget updated")

    def add(self, kind):
        self.sync()
        charts = []
        if kind == "ChartViewer":
            paths = filedialog.askopenfilenames(parent=self.root, title="Select two or three chart images",
                                                filetypes=[("Chart images", "*.png *.jpg *.jpeg *.webp")])
            if not paths:
                return
            charts = [Path(p) for p in paths]
        self.refresh(self.current.add_widget(kind, self.parent_choice.get(), charts))
        self.status.set(f"{kind} added")

    def validate(self):
        self.sync()
        kvmpack.validate_scene(self.current.members)
        self.status.set("Schema, permissions, layout, bindings and 7B loading budget validated")

    def simulate(self):
        self.sync()
        if self.preview_dir is None:
            self.preview_dir = Path(tempfile.gettempdir()) / ("kvmpad-studio-" + os.urandom(10).hex())
            self.preview_dir.mkdir()
        self.preview_number += 1
        path = self.preview_dir / f"preview-{self.preview_number}.html"
        write_file(path, kvmpack.preview_html(self.current.members).encode())
        if not webbrowser.open(path.as_uri()):
            raise ValueError(f"Open the simulator at {path}")
        self.status.set("Offline simulator opened")

    def package(self):
        self.sync()
        data = self.current.package()
        name = self.current.manifest["id"] + ".kvmpack"
        path = filedialog.asksaveasfilename(
            parent=self.root,
            initialfile=name,
            defaultextension=".kvmpack",
            filetypes=[("Extension pack", "*.kvmpack")],
        )
        if path:
            write_file(path, data, replace=True)
            self.saved = self.drafts()
            self.status.set(f"Saved {Path(path).name}")

    def save_source(self):
        self.sync()
        path = filedialog.asksaveasfilename(
            parent=self.root, title="Name a new source folder", initialfile=self.current.manifest["id"]
        )
        if not path:
            return
        directory = Path(path)
        if directory.exists():
            raise ValueError("Choose a new source folder.")
        temporary = directory.with_name(".studio-" + os.urandom(10).hex())
        temporary.mkdir()
        try:
            for name, data in self.current.members.items():
                dest = temporary / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                write_file(dest, data)
            temporary.rename(directory)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
        self.saved = self.drafts()
        if self.current.verified_archive is not None:
            self.current = Project(self.current.members)
            self.status.set(f"Editing unsigned source in {directory.name}")
        else:
            self.status.set(f"Saved source in {directory.name}")

    def install_paired(self):
        import paired_install
        self.sync()
        paired_install.dialog(self.root, self.current.package())

    def install(self):
        self.sync()
        directory = filedialog.askdirectory(parent=self.root, title="Choose the FAT microSD root")
        if directory:
            stage_installer(directory, self.current.manifest, self.current.package())
            self.status.set(
                "Installer verified. On the board choose Extensions → Install from card and approve the permissions."
            )

    def permissions(self):
        self.sync()
        m = self.current.manifest
        message = "\n".join(
            (
                "Required permissions",
                *(m.get("required_permissions") or ["None"]),
                "\nAll declared permissions",
                *(m["permissions"] or ["None"]),
                "\nUSB requirements",
                *(m.get("usb", {}).get("requires") or ["None"]),
                "\nHOST requirements",
                *(m.get("host", {}).get("requires") or ["None"]),
            )
        )
        messagebox.showinfo("Extension permissions", message, parent=self.root)

    def diagnostics(self):
        self.sync()
        path = filedialog.asksaveasfilename(
            parent=self.root, initialfile="studio-diagnostics.json", defaultextension=".json"
        )
        if path:
            write_file(path, formatted(self.current.diagnostics()), replace=True)
            self.status.set("Diagnostics exported")

    def close(self):
        if self.discard():
            if self.preview_dir:
                for index in range(1, self.preview_number + 1):
                    (self.preview_dir / f"preview-{index}.html").unlink(missing_ok=True)
                self.preview_dir.rmdir()
            self.root.destroy()


def launch(source=None, verify_key=None):
    root = tk.Tk()
    try:
        Editor(root, source or EXAMPLES / "flight-checklist", verify_key)
        root.mainloop()
    except (ValueError, OSError, BadZipFile) as error:
        messagebox.showerror("Extension Studio", str(error), parent=root)
        root.destroy()


if __name__ == "__main__":
    import sys

    launch(Path(sys.argv[1]) if len(sys.argv) > 1 else None)
