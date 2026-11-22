# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build and inspect extension packs on the desktop."""

import argparse
import base64
import hashlib
import io
import json
import math
import re
import stat
import struct
import sys
import zipfile
import zlib
from graphlib import CycleError, TopologicalSorter
from itertools import chain, groupby
from pathlib import Path

from jsonschema import Draft202012Validator


DOCUMENTS = ("manifest.json", "ui.json", "bindings.json")
SCHEMAS = Path(__file__).with_name("schemas")
FILE_LIMIT = 64 * 1024
PACK_LIMIT = 256 * 1024
EXPANDED_LIMIT = 2 * 1024 * 1024
ENTRY_LIMIT = 64
INTEGRITY = "integrity.json"
SIGNATURE = "signature.ed25519"
# Measured with the 7B release ABI and the pinned libpng/zlib configuration.
SCENE_BYTES_7B = 49712
PNG_STRUCT_BYTES_7B = 760
PNG_INFO_BYTES_7B = 276
INFLATE_STATE_BYTES_7B = 7120
PNG_ALLOCATION_HEADER_7B = 16
CAPABILITY = {
    "hid.key_tap": "hid.keyboard", "hid.pointer_button": "hid.mouse",
    "hid.radial.rotate": "hid.radial", "hid.joystick.set_axes": "hid.joystick",
    "host.command": "host.actions", "ui.navigate": "ui.basic", "state.set": "ui.basic", "state.adjust": "ui.basic",
    "stream.start": "host.screen.region", "stream.stop": "host.screen.region",
}
USB_SURFACES = {"MonitorViewport": "usb.monitor", "KeyboardSurface": "hid.keyboard", "TouchpadSurface": "hid.mouse"}


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


class DocumentError(ValueError):
    def __init__(self, name, data, path, code, line, column, detail):
        self.name, self.data = name, data
        self.line, self.column = line, column
        self.path, self.code = path, code
        super().__init__(f"{name}:{line}:{column} {path} [{code}] {detail}")


def document_error(name, data, path, code, detail):
    """Locate a value in already parsed JSON, skipping siblings with the standard decoder."""
    text = data.decode("utf-8")
    decoder = json.JSONDecoder()
    space = re.compile(r"[ \t\r\n]*")
    pos = space.match(text).end()
    display_path = "$"
    for part in path:
        pos = space.match(text, pos + 1).end()
        if isinstance(part, int):
            display_path += f"[{part}]"
            for _ in range(part):
                _, end = decoder.raw_decode(text, pos)
                comma = space.match(text, end).end()
                pos = space.match(text, comma + 1).end()
        else:
            display_path += "." + part if part.isidentifier() else "[" + json.dumps(part) + "]"
            while True:
                key, end = decoder.raw_decode(text, pos)
                colon = space.match(text, end).end()
                pos = space.match(text, colon + 1).end()
                if key == part:
                    break
                _, end = decoder.raw_decode(text, pos)
                comma = space.match(text, end).end()
                pos = space.match(text, comma + 1).end()
    return DocumentError(name, data, display_path, code, text.count("\n", 0, pos) + 1,
                         pos - text.rfind("\n", 0, pos), detail)


LOCAL_CONTROLS = {
    "TouchArea": ({"label"}, {"tap", "drag", "long_press"}),
    "RateSlider": (set(), {"step_up", "step_down"}),
    "Dial": ({"label", "min", "max", "value"}, {"rotate"}),
    "TabStrip": ({"tabs", "pages", "selected"}, {"change"}),
    "StatusChip": ({"text", "label", "status"}, set()),
}
BINDING_FIELDS = {"Text": ("text",), "StatusChip": ("text",), "ValueTile": ("value",),
                  "TabStrip": ("selected",), "RadioPanel": ("selected",),
                  "GaugeArc": ("value",), "GaugeNeedle": ("value",), "Horizon": ("pitch", "roll")}
CONTAINERS = {"Column", "Row", "Grid"}
CHIP_SYMBOLS = {"neutral": "−", "capability": "⚙", "permission": "◉", "info": "i",
                "success": "✓", "warning": "⚠", "error": "×"}


def check_control(node):
    kind, props, bounds = node["type"], node.get("props", {}), node["bounds"]
    keys, events = LOCAL_CONTROLS[kind]
    require(props.keys() <= keys and node.get("events", {}).keys() <= events, f"{kind}: unknown property or event")
    for key in ("text", "label"):
        if key in props:
            text = props[key]
            require(isinstance(text, str) and 0 < len(text.encode("utf-8")) <= 96 and "\0" not in text,
                    f"{kind}: invalid label")
    if kind == "RateSlider":
        require(bounds["w"] >= 48 and bounds["h"] >= 96, "RateSlider: too small")
    elif kind == "Dial":
        lo, hi = props.get("min", 0), props.get("max", 100)
        v = props.get("value", lo)
        require(all(type(n) is int for n in (lo, hi, v)) and -32768 <= lo <= v <= hi <= 32767 and lo < hi and
                bounds["w"] >= 72 and bounds["h"] >= 80, "Dial: invalid range, initial value or size")
    elif kind == "TouchArea":
        require(bounds["w"] >= 48 and bounds["h"] >= 48, "TouchArea: too small")
    elif kind == "StatusChip":
        role = props.get("status", "neutral")
        require(isinstance(role, str) and role in CHIP_SYMBOLS,
                "StatusChip: unsupported status")
    elif kind == "TabStrip":
        tabs = props.get("tabs", ["Tab 1", "Tab 2"])
        require(isinstance(tabs, list) and 2 <= len(tabs) <= 4 and
                all(isinstance(t, str) and 0 < len(t.encode("utf-8")) <= 24 and "\0" not in t for t in tabs) and
                len(set(tabs)) == len(tabs), "TabStrip: invalid captions")
        selected = props.get("selected", 0)
        require(type(selected) is int and 0 <= selected < len(tabs) and bounds["w"] >= 48*len(tabs) and bounds["h"] >= 42,
                "TabStrip: invalid initial tab or size")
        if "pages" in props:
            pages = props["pages"]
            require(isinstance(pages, list) and len(pages) == len(tabs) and all(isinstance(p, str) for p in pages),
                    "TabStrip: one container ID per tab required")


def check_action(kind, args, state, widgets, source, event):
    if source["type"] in LOCAL_CONTROLS:
        allowed = ("state.set", "state.adjust") if source["type"] == "RateSlider" else ("state.set",)
        require(kind in allowed, "unsupported local action for this control")
    if kind == "hid.joystick.set_axes":
        require(bool(args) and args.keys() <= {"axes", "buttons", "hat"}, "unknown joystick argument")
        axes = args.get("axes", {})
        require(isinstance(axes, dict) and axes.keys() <= {"X", "Y", "Z", "Rz"}, "unknown axis")
        for axis, value in axes.items():
            require(value in ("$event.x", "$event.y", "$event.value") or
                    type(value) in (int, float) and (0 if axis == "Z" else -1) <= value <= 1,
                    f"invalid value for axis {axis}")
        buttons = args.get("buttons", {})
        require(isinstance(buttons, dict) and all(k in {str(i) for i in range(1, 17)} and
                type(v) is bool for k, v in buttons.items()), "invalid joystick buttons")
        if "hat" in args:
            hat = args["hat"]
            require(args.keys() == {"hat"} and type(hat) is int and source["type"] == "Button" and
                    ((event == "press" and 0 <= hat < 8) or (event == "release" and hat == 8)),
                    "hat requires a Button press direction 0..7 and neutral release 8")
    elif kind in ("stream.start", "stream.stop"):
        expected = {"stream", "mode", "requires_foreground_consent"} if kind == "stream.start" else {"stream"}
        require(args.keys() == expected and args["stream"] == "cursor_region", "unsupported capture arguments")
        if kind == "stream.start":
            mode = args["mode"]
            require(args["requires_foreground_consent"] is True and isinstance(mode, str) and
                    (mode in ("OVERVIEW", "ZOOM_2X", "PIXEL_1X") or
                     mode.startswith("$state.") and state.get(mode[7:], {}).get("type") == "string"),
                    "invalid capture mode or consent")
    elif kind == "state.adjust":
        step = source["type"] == "RateSlider" and event in ("step_up", "step_down")
        tap = source["type"] == "Button" and event == "tap"
        require((step or tap) and
                args.keys() == {"name", "delta"} and isinstance(args["name"], str) and
                args["name"] in state and state[args["name"]]["type"] == "int" and
                type(args["delta"]) is int and -(1 << 31) <= args["delta"] < (1 << 31) and args["delta"] != 0,
                "state.adjust requires a Button tap or RateSlider step, an integer state and a nonzero int32 delta")
    elif kind == "state.set":
        require(args.keys() == {"name", "value"} and isinstance(args["name"], str) and
                args["name"] in state, "unknown state target or argument")
        item, v = state[args["name"]], args["value"]
        if isinstance(v, dict):
            require(item["type"] == "int" and v.keys() == {"event"}, "event value requires an int state")
            props = source.get("props", {})
            if event == "rotate" and source["type"] == "Dial" and v["event"] == "value":
                limits = props.get("min", 0), props.get("max", 100)
            elif event == "change" and source["type"] == "TabStrip" and v["event"] == "value":
                limits = 0, len(props.get("tabs", ["Tab 1", "Tab 2"])) - 1
            elif event == "drag" and source["type"] == "TouchArea" and v["event"] in ("x", "y"):
                limits = 0, source["bounds"]["w" if v["event"] == "x" else "h"] - 1
            else:
                raise ValueError("unsupported event value for this control")
            for limit in limits:
                check_state_value(item, limit)
        else:
            check_state_value(item, v)
    elif kind == "ui.navigate":
        require(args.keys() == {"target"} and isinstance(args["target"], str) and args["target"] in widgets,
                "invalid navigation target")
    else:
        raise ValueError(f"{kind}: no approved action argument contract in this desktop release")


def check_state_value(item, value):
    types = {"float": (int, float), "int": (int,), "bool": (bool,), "string": (str,)}
    require(type(value) in types[item["type"]], f"invalid state type for {item['id']}")
    kind = item["type"]
    if kind == "string":
        require("\0" not in value and len(value.encode("utf-8")) <= 96, f"invalid state string for {item['id']}")
    if kind in ("string", "bool"):
        require("min" not in item and "max" not in item, f"invalid state range for {item['id']}")
        return
    bounds = {k: item[k] for k in ("min", "max") if k in item}
    require(all(type(n) in (int, float) and (type(n) is int or math.isfinite(n)) for n in bounds.values()),
            f"invalid state bound for {item['id']}")
    if kind == "int":
        require(-(1 << 63) <= value < 1 << 63, f"state integer outside int64 for {item['id']}")
    else:
        try:
            value = float(value)
            bounds = {k: float(n) for k, n in bounds.items()}
        except OverflowError as error:
            raise ValueError(f"nonfinite state number for {item['id']}") from error
        require(math.isfinite(value), f"nonfinite state number for {item['id']}")
    require(bounds.get("min", -math.inf) <= value <= bounds.get("max", math.inf), f"invalid state range for {item['id']}")


def check_transform(connection, kind):
    name = connection["transform"]
    params = connection.get("params", {})
    allowed = {"identity": (), "clamp": ("min", "max"), "scale": ("factor",), "offset": ("amount",),
               "round": (), "enum_map": ("entries",), "hysteresis": ("low", "high"), "lowpass": ("tau_ms",),
               "format_number": ("digits",), "boolean_threshold": ("threshold",)}
    require(params.keys() <= set(allowed[name]), f"unexpected {name} parameter")
    if name == "identity":
        return kind
    require(kind in ("int", "float"), f"{name} requires numeric input")
    for key, value in params.items():
        if key == "entries":
            require(len({e["code"] for e in value}) == len(value) and
                    all(type(e["code"]) is int and len(e["label"].encode("utf-8")) <= 48 and
                        "\0" not in e["label"] for e in value), "invalid enum entries")
        elif key in ("tau_ms", "digits"):
            require(type(value) is int, f"{key} must be an integer")
        else:
            try:
                finite = math.isfinite(value)
            except OverflowError:
                finite = False
            require(finite, f"nonfinite {name} parameter")
    if name == "clamp":
        require(float(params.get("min", 0)) <= float(params.get("max", 1)), "inverted clamp bounds")
    if name == "hysteresis":
        require(float(params.get("low", .25)) < float(params.get("high", .75)), "invalid hysteresis thresholds")
    if name in ("format_number", "enum_map"):
        return "string"
    return "bool" if name in ("boolean_threshold", "hysteresis") else "float"


def parse_json(data, name):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"{name}: duplicate key {key}")
            result[key] = value
        return result

    def check(value, depth):
        if isinstance(value, (dict, list)):
            require(depth <= 12, f"{name}: JSON nesting exceeds 12")
            for item in value.values() if isinstance(value, dict) else value:
                check(item, depth + 1)
        elif isinstance(value, float):
            require(math.isfinite(value), f"{name}: nonfinite number")

    require(len(data) <= FILE_LIMIT, f"{name}: exceeds 64 KiB")
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=unique)
    except json.JSONDecodeError as error:
        raise DocumentError(name, data, "$", "JSON_SYNTAX", error.lineno, error.colno, error.msg) from error
    except UnicodeDecodeError as error:
        prefix = data[:error.start].decode("utf-8")
        raise DocumentError(name, data, "$", "JSON_ENCODING", prefix.count("\n") + 1,
                            len(prefix) - prefix.rfind("\n"), "Invalid UTF-8") from error
    check(value, 1)
    return value


def png_chunks_after_header(data, name):
    view = memoryview(data)
    offset = 33
    while offset + 12 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        require(length <= len(data) - offset - 12, f"invalid PNG chunk for {name}")
        if kind == b"IEND":
            return
        yield kind, view[offset + 8:offset + 8 + length]
        offset += length + 12


def png_inflate_bytes(chunks, multiple_rows, output_limit, name):
    """Account for the largest IDAT read and zlib's on-demand history window."""
    stream = zlib.decompressobj()
    input_bytes = window = produced = 0
    history_used = multiple_rows
    for chunk in chunks:
        for pos in range(0, len(chunk), 8192):
            block = chunk[pos:pos + 8192]
            if not window:
                cmf = block[0]
                require((cmf & 15) == 8 and (cmf >> 4) <= 7, f"invalid PNG compression header for {name}")
                window = 1 << (8 + (cmf >> 4))
            input_bytes = max(input_bytes, len(block))
            while block and not stream.eof:
                try:
                    output = stream.decompress(block, 32768)
                except zlib.error as exc:
                    raise ValueError(f"invalid PNG image data for {name}") from exc
                produced += len(output)
                require(produced <= output_limit, f"excess PNG image data for {name}")
                # A single row can finish in one call without history. Output before
                # the trailer is available requires the advertised window.
                if output and not stream.eof:
                    history_used = True
                block = stream.unconsumed_tail
            if stream.eof:
                return input_bytes + (window + PNG_ALLOCATION_HEADER_7B if history_used else 0)
    raise ValueError(f"incomplete PNG image data for {name}")


def png_workspace_floor(data, name):
    """Required concurrent allocations for PNG image data on the 7B."""
    require(len(data) >= 33 and data[12:16] == b"IHDR", f"invalid PNG header for {name}")
    width, height = struct.unpack_from(">II", data, 16)
    require(0 < width <= 1024 and 0 < height <= 568, f"invalid PNG dimensions for {name}")
    depth, color, interlaced = data[24], data[25], data[28]
    palette_bytes = 0
    chunks = png_chunks_after_header(data, name)
    for kind, payload in chunks:
        if kind == b"PLTE" and color in (2, 3, 6):
            # Both png_struct and png_info own full 256-entry palette copies.
            palette_bytes += 2 * (768 + PNG_ALLOCATION_HEADER_7B)
        elif kind == b"tRNS" and color in (0, 2, 3) and payload:
            palette_bytes += 2 * (256 + PNG_ALLOCATION_HEADER_7B)
        elif kind == b"IDAT":
            break
    else:
        raise ValueError(f"missing PNG image data for {name}")

    blocks = chain((payload,), (part for tag, part in chunks if tag == b"IDAT"))
    inflate_bytes = png_inflate_bytes(blocks, height > 1 or (interlaced == 1 and width > 1),
                                      height * (8 * width + 7), name)

    # libpng transforms into four channels, using wide rows before stripping
    # 16-bit samples. Each of the two filter rows includes a spare pixel and padding.
    aligned_width = (width + 7) // 8 * 8
    pixel_bytes = 8 if depth == 16 else 4
    filter_row = pixel_bytes * (aligned_width + 1) + 1 + 48
    # The fixed 21-entry chunk policy uses five bytes per entry. The other
    # seven allocations are the PNG structs, inflate state, three rows and IDAT input.
    fixed = PNG_STRUCT_BYTES_7B + PNG_INFO_BYTES_7B + INFLATE_STATE_BYTES_7B + 21 * 5
    return (fixed + 8 * PNG_ALLOCATION_HEADER_7B + palette_bytes + 2 * filter_row
            + 4 * width + inflate_bytes)


class ProfileBudgetError(ValueError):
    def __init__(self, resource, declared_kib, minimum_kib):
        self.declared_kib = declared_kib
        self.minimum_kib = minimum_kib
        super().__init__(f"{resource} needs at least {minimum_kib} KiB to load on the 7B. "
                         f"Manifest max_memory_kib is {declared_kib}.")


def icon_size(width, height):
    longest = max(width, height)
    if longest <= 32:
        return width, height
    return max(1, width * 32 // longest), max(1, height * 32 // longest)


def button_icons(node):
    if node["type"] == "Button" and "icon" in node.get("props", {}):
        yield node["props"]["icon"]
    for child in node.get("children", []):
        yield from button_icons(child)


def icon_thumbnail(data):
    from PIL import Image
    with Image.open(io.BytesIO(data), formats=["PNG"]) as image:
        w, h = icon_size(*image.size)
        points = [(x * image.width // w, y * image.height // h) for y in range(h) for x in range(w)]
        if data[24:26] == b"\x10\x00":
            # libpng strips the low byte after comparing the original grayscale tRNS key.
            key = image.info.get("transparency")
            values = (image.getpixel(point) for point in points)
            pixels = [(v >> 8, v >> 8, v >> 8, 0 if v == key else 255) for v in values]
        else:
            rgba = image.convert("RGBA")
            pixels = [rgba.getpixel(point) for point in points]
        result = Image.new("RGBA", (w, h))
        result.putdata(pixels)
        return result


def prepare_chart(path):
    from PIL import Image, ImageOps
    with Image.open(path, formats=["PNG", "JPEG", "WEBP"]) as source:
        require(source.width * source.height <= 32_000_000 and getattr(source, "n_frames", 1) == 1,
                "Choose a single chart image with up to 32 megapixels.")
        rgba = ImageOps.exif_transpose(source).convert("RGBA")
        rgba.thumbnail((600, 440), Image.Resampling.LANCZOS)
        paper = Image.new("RGB", rgba.size, "white")
        paper.paste(rgba, mask=rgba.getchannel("A"))
        # Preserve sparse ink and runway markings on a mostly uniform paper background.
        chart = paper.quantize(colors=16, method=Image.Quantize.MAXCOVERAGE, dither=Image.Dither.NONE)
        result = io.BytesIO()
        chart.save(result, format="PNG", bits=4, optimize=True)
        return result.getvalue()


def chart_assets(node):
    if node["type"] == "ChartViewer":
        yield from (chart["asset"] for chart in node["props"]["charts"])
    for child in node.get("children", []):
        yield from chart_assets(child)


def chart_run_bytes(data):
    from PIL import Image
    with Image.open(io.BytesIO(data), formats=["PNG"]) as image:
        # Palette entries with identical RGBA values also share a device run.
        pixels = image.convert("RGBA")
        total = 0
        for y in range(image.height):
            row = pixels.crop((0, y, image.width, y+1)).tobytes()
            total += sum((sum(1 for _ in run)+15)//16 for _, run in groupby(memoryview(row).cast("I")))
        return total


def check_memory_budget(files, kib, icons=(), charts=(), keyboards=0):
    resident = SCENE_BYTES_7B
    resident += sum(chart_run_bytes(files[name]) for name in charts)
    for name in set(icons):
        w, h = icon_size(*struct.unpack_from(">II", files[name], 16))
        resident += 8 + 4 * w * h
    required = resident
    resource = "manifest.json"
    for name, data in files.items():
        if name.startswith("assets/"):
            peak = resident + png_workspace_floor(data, name)
            if peak > required:
                required, resource = peak, name
    # Rendering starts after image decoding releases its workspace. Each
    # KeyboardSurface keeps its system keys, including on a hidden page.
    if keyboards and resident + keyboards * 48 * 1024 > required:
        required, resource = resident + keyboards * 48 * 1024, "KeyboardSurface"
    minimum_kib = (required + 1023) // 1024
    if kib < minimum_kib:
        raise ProfileBudgetError(resource, kib, minimum_kib)


def content_size(ui):
    return (600, 992) if ui.get("orientation") == "portrait" else (1024, 568)


def validate_scene(files, *, editing=False):
    docs = []
    for name in DOCUMENTS:
        require(name in files, f"missing {name}")
        data = parse_json(files[name], name)
        schema = json.loads((SCHEMAS / f"kvmpack-{name[:-5]}.schema.json").read_bytes())
        error = next(Draft202012Validator(schema).iter_errors(data), None)
        if error:
            raise document_error(name, files[name], error.absolute_path, "JSON_SCHEMA", error.message)
        docs.append(data)
    manifest, ui, bindings = docs
    permissions = set(manifest["permissions"])
    require(set(manifest["required_permissions"]) <= permissions, "undeclared required permission")
    require("ui.basic" in permissions, "scene requires ui.basic")
    require(manifest["entry_view"] == ui["root"]["id"], "missing entry view")
    if manifest["usb"]["mode"] == "monitor_controls":
        usb_caps = set(manifest["usb"]["requires"])
        require(usb_caps == permissions.intersection(USB_SURFACES.values()) and
                usb_caps <= set(manifest["required_permissions"]) and "hid.joystick" not in permissions,
                "The monitor_controls USB capabilities must match the required display and input permissions")
    else:
        require("usb.monitor" not in permissions, "usb.monitor requires USB mode monitor_controls")

    widgets, events, parents = {}, {}, {}
    width, height = content_size(ui)
    screens = ui.get("screens", [])
    if screens:
        height -= 48

    surface_types = set()
    def visit(node, depth, parent=None, screen=None):
        key = node["id"]
        if key in screens:
            screen = key
        require(key not in widgets and len(widgets) < 128 and depth <= 8,
                f"ui.json: duplicate id or scene limit at {key}")
        widgets[key] = node
        parents[key] = parent
        require("bounds" in node, f"ui.json: {key} has no absolute bounds")
        b = node["bounds"]
        limit = widgets[parent]["bounds"] if parent else {"w":width, "h":height}
        boundary = "its parent" if parent else f"the {width}x{height} content area"
        require(b["x"] + b["w"] <= limit["w"] and b["y"] + b["h"] <= limit["h"],
                f"ui.json: {key} leaves {boundary}")
        require(node["type"] in CONTAINERS or not node.get("children"), "only containers may have children")
        if node["type"] in LOCAL_CONTROLS:
            check_control(node)
        props = node.get("props", {})
        if node["type"] in USB_SURFACES:
            kind = node["type"]
            cap = USB_SURFACES[kind]
            require(manifest["usb"]["mode"] == "monitor_controls", f"{kind} requires USB mode monitor_controls")
            require(cap in manifest["required_permissions"] and cap in manifest["usb"]["requires"],
                    f"{kind} requires {cap} in permissions, required_permissions and usb.requires")
            require((screen, kind) not in surface_types, f"Only one {kind} is supported per screen")
            surface_types.add((screen, kind))
        if node["type"] in ("AutopilotPanel", "RadioPanel"):
            require(not props and not node.get("events") and b["w"] >= 540 and b["h"] >= 680 and
                    sum(w["type"] == node["type"] for w in widgets.values()) == 1,
                    f"Use one {node['type']} with empty properties and at least 540 × 680 pixels")
        if node["type"] == "ChartViewer" or "charts" in props:
            require(node["type"] == "ChartViewer" and props.keys() == {"charts"} and
                    b["w"] >= 540 and b["h"] >= 420, "ChartViewer needs charts and a 540 × 420 or larger area")
            require(all(w is node or w["type"] != "ChartViewer" for w in widgets.values()),
                    "Only one ChartViewer is supported in a profile")
            charts = props["charts"]
            require(len({c["title"] for c in charts}) == len({c["asset"] for c in charts}) == len(charts),
                    "Chart titles and assets must be distinct")
            for chart in charts:
                name = chart["asset"]
                require(name in files and "\0" not in chart["title"] and len(chart["title"].encode()) <= 48,
                        "Each chart needs a package asset and a title of up to 48 UTF-8 bytes")
                data = files[name]
                require(len(data) >= 33 and data[25] == 3 and data[24] <= 4 and data[28] == 0 and
                        0 < int.from_bytes(data[16:20], "big") <= 600 and 0 < int.from_bytes(data[20:24], "big") <= 440,
                        "Prepare charts as noninterlaced indexed PNGs up to 600 × 440 and 16 colors")
        if "icon" in props:
            name = props["icon"]
            require(node["type"] == "Button" and isinstance(name, str) and name.startswith("assets/") and name in files,
                    f"ui.json: {key}: icon must name a PNG in the package on a Button")
            check_path(name)
        if props.keys() & {"invert_y", "curve", "dead_zone_percent"}:
            zone = props.get("dead_zone_percent", 0)
            require(node["type"] == "AxisPad" and type(props.get("invert_y", False)) is bool and
                    props.get("curve", "linear") in ("linear", "cubic") and
                    type(zone) is int and 0 <= zone < 100, "invalid AxisPad calibration")
        if node["type"] == "StreamViewport":
            require("host.screen.region" in permissions, "StreamViewport requires host.screen.region")
        for event, action in node.get("events", {}).items():
            if event in ("step_up", "step_down"):
                require(node["type"] == "RateSlider", "step events require a RateSlider")
            events[f"{key}.{event}"] = action
        for child in node.get("children", []):
            visit(child, depth + 1, key, screen)

    require(ui["root"]["type"] in CONTAINERS, "entry must be a container")
    visit(ui["root"], 1)
    if manifest["usb"]["mode"] == "monitor_controls" and not editing:
        require(any(kind == "MonitorViewport" for _, kind in surface_types),
                "The monitor_controls profile requires a MonitorViewport for local start")
    if screens:
        children = ui["root"].get("children", [])
        require(set(screens) == {child["id"] for child in children},
                "screens must list every direct child of the root")
        for child in children:
            title = child.get("props", {}).get("title")
            require(child["type"] in CONTAINERS and isinstance(title, str) and 0 < len(title.encode()) <= 48 and
                    "\0" not in title and not ({"text","label"} & child["props"].keys()),
                    "each screen must be a container with a title of up to 48 bytes")
    if not editing:
        check_memory_budget(files, manifest["limits"]["max_memory_kib"], button_icons(ui["root"]), chart_assets(ui["root"]),
                            sum(n["type"] == "KeyboardSurface" for n in widgets.values()))
    claimed_pages = set()
    caption_sets = set()
    tab_count = 0
    for key, node in widgets.items():
        if node["type"] != "TabStrip":
            continue
        props = node.get("props", {})
        captions = tuple(props.get("tabs", ["Tab 1", "Tab 2"]))
        if "pages" in props or captions not in caption_sets:
            tab_count += len(captions)
        if "pages" not in props:
            caption_sets.add(captions)
        require(tab_count <= 32, "scene has more than 32 tabs")
        for page in props.get("pages", []):
            require(page in widgets and page not in screens and page not in claimed_pages and parents[page] == parents[key] and
                    widgets[page]["type"] in CONTAINERS, "tab pages must be distinct sibling containers")
            claimed_pages.add(page)
    variables = {}
    for item in bindings["state"] + bindings["sources"]:
        key = item["id"]
        require(key not in variables, f"bindings.json: duplicate variable {key}")
        variables[key] = item
    if bindings["sources"]:
        require("host.telemetry.read" in permissions, "telemetry requires permission")
    state = {s["id"]: s for s in bindings["state"]}
    for item in state.values():
        check_state_value(item, item["initial"])
    def binding_error(index, field, code, detail):
        return document_error("bindings.json", files["bindings.json"], ("connections", index, field), code, detail)

    graph = TopologicalSorter()
    targets, enum_entries = {}, 0
    for index, connection in enumerate(bindings["connections"]):
        # The v1 schema connects a named source/state to a widget property in one direction.
        source, endpoint = connection["from"], connection["to"]
        if source not in variables:
            raise binding_error(index, "from", "BINDING_SOURCE", f"unknown binding source {source}")
        target, field = endpoint.split(".")
        if target not in widgets:
            raise binding_error(index, "to", "BINDING_TARGET", f"unknown binding target {endpoint}")
        try:
            output = check_transform(connection, variables[source]["type"])
        except ValueError as error:
            field_name = "params" if connection.get("params") else "transform"
            raise binding_error(index, field_name, "BINDING_TRANSFORM", str(error)) from error
        if field in ("value", "pitch", "roll") and output not in ("int", "float"):
            raise binding_error(index, "to", "BINDING_TYPE", f"numeric binding requires numeric output: {endpoint}")
        if endpoint in targets:
            raise binding_error(index, "to", "BINDING_DUPLICATE", f"duplicate binding target {endpoint}")
        targets[endpoint] = index
        enum_entries += len(connection.get("params", {}).get("entries", []))
        if enum_entries > 64:
            raise binding_error(index, "params", "BINDING_LIMIT", "scene enum table exceeds 64 entries")
        graph.add(endpoint, source)
    try:
        graph.prepare()
    except CycleError as error:
        raise binding_error(targets[error.args[1][0]], "to", "BINDING_CYCLE", "cyclic connections") from error
    for endpoint, index in targets.items():
        target, field = endpoint.split(".")
        if field not in BINDING_FIELDS.get(widgets[target]["type"], ()):
            raise binding_error(index, "to", "BINDING_TARGET", f"unsupported binding target {endpoint}")
        if field == "selected":
            connection = bindings["connections"][index]
            item = state.get(connection["from"], {})
            if widgets[target]["type"] == "RadioPanel":
                if tuple(map(int, manifest["min_firmware"].split("."))) < (1, 4, 0):
                    raise binding_error(index, "to", "BINDING_TARGET", "RadioPanel.selected requires min_firmware 1.4.0 or later")
                count = 2
            else:
                count = len(widgets[target].get("props", {}).get("tabs", ["Tab 1", "Tab 2"]))
            if (item.get("type") != "int" or connection["transform"] != "identity" or
                    item.get("min", -1) < 0 or item.get("max", count) >= count):
                raise binding_error(index, "to", "BINDING_TYPE", "Selection requires a local integer bounded to the available options")
    actions = {}
    for action in bindings["actions"]:
        key, trigger, kind = action["id"], action["trigger"], action["type"]
        require(key not in actions and events.get(trigger) == key,
                f"bindings.json: duplicate action or unresolved trigger for {key}")
        require(action["capability"] == CAPABILITY[kind] and CAPABILITY[kind] in permissions,
                f"bindings.json: wrong permission for {key}")
        if kind.startswith("hid."):
            require(CAPABILITY[kind] in manifest["usb"]["requires"], "missing USB capability")
        if kind.startswith("stream."):
            require("capture.windows" in manifest["host"]["requires"], "missing capture provider")
        widget, event = trigger.rsplit(".", 1)
        if kind == "state.adjust" and widgets[widget]["type"] == "Button":
            require(tuple(map(int, manifest["min_firmware"].split("."))) >= (1, 4, 0),
                    "Button state.adjust requires min_firmware 1.4.0 or later")
        check_action(kind, action["args"], state, widgets, widgets[widget], event)
        actions[key] = trigger
    require(all(actions.get(key) == trigger for trigger, key in events.items()), "unresolved UI event")
    hats = {a["trigger"] for a in bindings["actions"] if a["type"] == "hid.joystick.set_axes" and "hat" in a["args"]}
    for trigger in hats:
        widget, event = trigger.rsplit(".", 1)
        other = "release" if event == "press" else "press"
        require(f"{widget}.{other}" in hats, "hat control requires matched press and release actions")
    return manifest


def check_path(name):
    if name in (*DOCUMENTS, INTEGRITY):
        return
    # The desktop data profile accepts PNG assets. A new asset format needs a decoder budget.
    parts = name.split("/")
    require(parts[0] == "assets" and name.endswith(".png") and len(name) <= 160,
            f"unsupported pack member {name!r}")
    for part in parts:
        require(re.fullmatch(r"[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*", part) and
                not re.fullmatch(r"(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])", part.split(".")[0]),
                f"invalid asset path {name!r}")


class PublisherKeyRequired(ValueError):
    """A signed archive needs a caller-selected publisher key."""


def read_public_key(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import UnsupportedAlgorithm

    try:
        key = serialization.load_pem_public_key(path.read_bytes())
    except (ValueError, UnsupportedAlgorithm) as exc:
        raise ValueError("Expected an Ed25519 public PEM file.") from exc
    require(isinstance(key, Ed25519PublicKey), "expected Ed25519 public key")
    return key


def check_png(data, name):
    from PIL import Image
    require(len(data) >= 45 and data.startswith(b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR") and
            data.endswith(b"\0\0\0\0IEND\xaeB`\x82"), f"invalid PNG container for {name}")
    w, h = struct.unpack_from(">II", data, 16)
    require(0 < w <= 1024 and 0 < h <= 568, f"PNG dimensions exceed viewport for {name}")
    with Image.open(io.BytesIO(data), formats=["PNG"]) as img:
        require(img.n_frames == 1, "animated PNG is unsupported")
        img.verify()
    with Image.open(io.BytesIO(data), formats=["PNG"]) as img:
        img.load()


def read_package(path, verify_key=None):
    files, seen = {}, set()
    total = 0
    archive_bytes = None

    def read(name, size, open_file):
        nonlocal total
        if name == SIGNATURE:
            require(size == 64 and archive_bytes is not None,
                    "signature requires a trusted Ed25519 public key")
            if verify_key is None:
                raise PublisherKeyRequired("signature requires a trusted Ed25519 public key")
        else:
            check_path(name)
        folded = name.casefold()
        require(folded not in seen and len(seen) < ENTRY_LIMIT, "duplicate or excess members")
        seen.add(folded)
        total += size
        limit = FILE_LIMIT if name.endswith(".json") else EXPANDED_LIMIT
        require(0 <= size <= limit and total <= EXPANDED_LIMIT, "pack exceeds expanded size limit")
        with open_file() as stream:
            data = stream.read(size + 1)
        require(len(data) == size, f"invalid length for {name}")
        if name.startswith("assets/"):
            check_png(data, name)
        files[name] = data

    if path.is_dir():
        require(verify_key is None, "signature verification requires a packaged archive")
        def visit(folder):
            for item in folder.iterdir():
                name = item.relative_to(path).as_posix()
                require(not item.is_symlink() and not getattr(item, "is_junction", lambda: False)(),
                        f"link in pack: {name}")
                if item.is_dir():
                    require(len(name) <= 150 and (name == "assets" or name.startswith("assets/")),
                            f"invalid input directory: {name}")
                    visit(item)
                else:
                    require(item.is_file() and name != INTEGRITY, f"invalid source file {name}")
                    read(name, item.stat().st_size, lambda: item.open("rb"))
        visit(path)
    else:
        require(path.stat().st_size <= PACK_LIMIT, "archive exceeds 256 KiB")
        with path.open("rb") as source:
            archive_bytes = source.read(PACK_LIMIT + 1)
        require(len(archive_bytes) <= PACK_LIMIT, "archive exceeds 256 KiB")
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
            for entry in archive.infolist():
                kind = stat.S_IFMT(entry.external_attr >> 16)
                require(kind in (0, stat.S_IFREG) and not entry.flag_bits & 1 and
                        entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED),
                        "unsupported archive entry")
                require(entry.orig_filename == entry.filename, "noncanonical archive name")
                read(entry.filename, entry.file_size, lambda: archive.open(entry))
        require(INTEGRITY in files, "missing integrity.json")
        recorded = parse_json(files.pop(INTEGRITY), INTEGRITY)
        signature = files.pop(SIGNATURE, None)
        require(recorded == integrity(files), "payload digest or inventory mismatch")
        if verify_key is not None:
            from cryptography.exceptions import InvalidSignature
            require(signature is not None, "unsigned package")
            try:
                verify_key.verify(signature, signature_input(files))
            except InvalidSignature as exc:
                raise ValueError("untrusted package signature") from exc
    return files, archive_bytes


def read_members(path, verify_key=None):
    return read_package(path, verify_key)[0]


def integrity(files):
    return {name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in sorted(files.items())}


def signature_input(files):
    message = bytearray(b"KVM-PACK-SIG\x00\x01")
    for name, content in sorted(files.items()):
        encoded = name.encode("ascii")
        message.extend(struct.pack("<H", len(encoded)))
        message.extend(encoded)
        message.extend(struct.pack("<I", len(content)))
        message.extend(hashlib.sha256(content).digest())
    return bytes(message)


def build(files, signing_key=None):
    require(len(files) + 1 + bool(signing_key) <= ENTRY_LIMIT, "too many pack entries")
    records = json.dumps(integrity(files), separators=(",", ":"), sort_keys=True).encode()
    require(sum(map(len, files.values())) + len(records) <= EXPANDED_LIMIT, "archive too large")
    members = {**files, INTEGRITY: records}
    if signing_key is not None:
        members[SIGNATURE] = signing_key.sign(signature_input(files))
        require(sum(map(len, members.values())) <= EXPANDED_LIMIT, "archive too large")
    result = io.BytesIO()
    with zipfile.ZipFile(result, "w") as archive:
        for name, data in sorted(members.items()):
            entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    require(result.tell() <= PACK_LIMIT, "archive exceeds 256 KiB")
    return result.getvalue()


def preview_html(files):
    scene = {Path(name).stem: parse_json(files[name], name) for name in DOCUMENTS}
    scene["chip_symbols"] = CHIP_SYMBOLS
    icons = {}
    for name in sorted(set(button_icons(scene["ui"]["root"]))):
        png = io.BytesIO()
        icon_thumbnail(files[name]).save(png, format="PNG")
        icons[name] = "data:image/png;base64," + base64.b64encode(png.getvalue()).decode("ascii")
    scene["icons"] = icons
    scene["charts"] = {name: "data:image/png;base64," + base64.b64encode(files[name]).decode("ascii")
                       for name in chart_assets(scene["ui"]["root"])}
    # JSON.parse rounds integers beyond 2^53. Carry the decimal text to the preview's BigInt constructor.
    bindings = scene["bindings"]
    kinds = {s["id"]: s["type"] for s in bindings["state"]}
    def float_label(value):
        return format(float(value) or 0.0, ".15g")
    for state in bindings["state"]:
        if state["type"] == "int":
            state["initial"] = str(state["initial"])
            state["int_low"] = str(max(-(1 << 63), math.ceil(state.get("min", -(1 << 63)))))
            state["int_high"] = str(min((1 << 63)-1, math.floor(state.get("max", (1 << 63)-1))))
        elif state["type"] == "float":
            state["initial_text"] = float_label(state["initial"])
    for action in bindings["actions"]:
        if action["type"] == "state.set":
            args = action["args"]
            if isinstance(args["value"], dict):
                continue
            if kinds[args["name"]] == "int":
                args["value"] = str(args["value"])
            elif kinds[args["name"]] == "float":
                action["state_text"] = float_label(args["value"])
    # All package strings remain inert, including a literal closing script tag.
    data = json.dumps(scene, ensure_ascii=True, allow_nan=False).replace("<", "\\u003c")
    template = Path(__file__).with_name("preview.html").read_text(encoding="utf-8")
    autopilot = Path(__file__).with_name("autopilot_preview.js").read_text(encoding="utf-8")
    return template.replace("__KVM_AUTOPILOT_PREVIEW__", autopilot).replace("__KVM_PREVIEW_DATA__", data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["validate", "inspect", "build", "stage", "preview", "studio", "install", "devices"])
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--card", type=Path, help="mount directory for offline staging")
    parser.add_argument("--device", help="paired device ID in the Windows CLIENT credential store")
    parser.add_argument("--sign-key", type=Path, help="Ed25519 private PEM for build")
    parser.add_argument("--verify-key", type=Path, help="trusted Ed25519 public PEM, requires a signed archive")
    args = parser.parse_intermixed_args()
    verify_key = None
    if args.verify_key:
        require(args.source is not None and args.source.is_file(), "--verify-key requires an archive")
        verify_key = read_public_key(args.verify_key)
    if args.command == "devices":
        from paired_install import paired_devices
        for device in paired_devices():
            print(f"{device['id']}  {device['name']}")
        return
    if args.command == "studio":
        from extension_studio import launch
        launch(args.source, verify_key)
        return
    if args.source is None:
        parser.error("a source directory or archive is required")
    if args.command == "install" and (not args.device or not args.source.is_file()):
        parser.error("install requires --device PAIRED-ID and an existing archive")
    signing_key = None
    if args.sign_key:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        require(args.command == "build", "--sign-key requires build")
        signing_key = serialization.load_pem_private_key(args.sign_key.read_bytes(), password=None)
        require(isinstance(signing_key, Ed25519PrivateKey), "expected Ed25519 private key")
    files, archive_bytes = read_package(args.source, verify_key)
    manifest = validate_scene(files)
    if args.command == "install":
        from paired_install import start, finish
        print("Open the device receive window and review the requested permissions on the panel.", flush=True)
        finish(start(args.device), archive_bytes)
        print(f"{manifest['id']}: installation confirmed by the device")
    elif args.command == "stage":
        from card import stage_installer
        require(args.card is not None, "stage requires --card MOUNT")
        data = archive_bytes if archive_bytes is not None else build(files)
        path = stage_installer(args.card, manifest, data)
        print(f"{path}: verified installer. Open Extensions and choose Install from card on the device.")
    elif args.command == "preview":
        output = args.output or Path(f"{manifest['id']}.html")
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(preview_html(files))
        print(output)
    elif args.command == "build":
        require(args.source.is_dir(), "build requires a source directory")
        output = args.output or Path(f"{manifest['id']}-{manifest['version']}.kvmpack")
        data = build(files, signing_key)
        with output.open("xb") as stream:
            stream.write(data)
        print(output)
    elif args.command == "inspect":
        print(json.dumps(manifest, indent=2, ensure_ascii=True))
    else:
        print(f"{manifest['id']} {manifest['version']}: valid desktop source and payload")


if __name__ == "__main__":
    try:
        main()
    except (OSError, SyntaxError, ValueError, RecursionError, zipfile.BadZipFile) as error:
        sys.exit(f"kvmpack: {error}")
