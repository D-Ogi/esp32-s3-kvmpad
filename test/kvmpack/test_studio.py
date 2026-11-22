# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise the native editor using synthetic widgets and card contents."""

import json
import shutil
import subprocess
import sys
import tempfile
import tkinter as tk
import unittest
import uuid
from pathlib import Path
from unittest.mock import patch

SDK = Path(__file__).resolve().parents[2] / "tools/kvmpack"
sys.path.insert(0, str(SDK))
import extension_studio as studio
import kvmpack
import paired_install


class StudioTests(unittest.TestCase):
    def test_rate_slider_authoring(self):
        editor = self.editor
        editor.open(SDK / "samples/rate-slider")
        editor.select("wheel")
        self.assertGreaterEqual(len(editor.canvas.find_withtag("node:wheel")), 3)
        self.assertEqual(str(editor.label_entry["state"]), "disabled")
        self.assertEqual(tuple(editor.combos["event"]["values"]), ("step_up", "step_down"))
        self.assertIn("increase", editor.combos["action"]["values"])
        self.assertIn("decrease", editor.combos["action"]["values"])
        editor.select("panel")
        editor.add("RateSlider")
        node = editor.nodes[editor.selected]
        self.assertEqual(node["type"], "RateSlider")
        self.assertEqual((node["bounds"]["w"], node["bounds"]["h"]), (64, 160))
        kvmpack.validate_scene(editor.current.members)

    def test_usb_console_open_and_edit(self):
        editor = self.editor
        editor.open(studio.USB_CONSOLE)
        self.assertEqual(editor.scene_size, (600, 944))
        self.assertEqual(editor.screen_choice.get(), "Console")
        for name in ("desktop", "touchpad", "keyboard"):
            editor.select(name)
            self.assertTrue(editor.canvas.find_withtag("node:" + name))
            self.assertEqual(str(editor.label_entry["state"]), "disabled")
        editor.screen_choice.current(1)
        editor.screen_choice.event_generate("<<ComboboxSelected>>")
        self.root.update()
        self.assertTrue(editor.canvas.find_withtag("node:large_touchpad"))
        self.assertFalse(editor.canvas.find_withtag("node:keyboard"))
        editor.parent_choice.set("pointer")
        editor.widget_kind.set("KeyboardSurface")
        editor.add_button.invoke()
        self.assertEqual(editor.nodes[editor.selected]["type"], "KeyboardSurface")
        self.assertTrue(editor.current.package())

    def test_layout_shows_only_the_selected_extension_screen(self):
        editor = self.editor
        ui = editor.current.doc("ui.json")
        root = ui["root"]
        root["bounds"]["h"] = 520
        ui["screens"] = ["preflight", "setup"]
        items = root["children"]
        root["children"] = [
            {"id": name, "type": "Column", "bounds": dict(root["bounds"]),
             "props": {"title": title}, "children": children}
            for name, title, children in (("setup", "Setup", []), ("preflight", "Preflight", items))
        ]
        editor.current.update({"ui.json": studio.formatted(ui)})
        editor.refresh("preflight")
        self.assertEqual(editor.screen_choice.get(), "Preflight")
        self.assertTrue(editor.canvas.find_withtag("node:complete"))
        self.assertFalse(editor.canvas.find_withtag("node:setup"))
        editor.screen_choice.current(1)
        editor.screen_choice.event_generate("<<ComboboxSelected>>")
        self.root.update()
        self.assertEqual(editor.selected, "setup")
        self.assertFalse(editor.canvas.find_withtag("node:complete"))
        self.assertTrue(editor.canvas.find_withtag("node:setup"))
        editor.select("complete")
        self.assertEqual(editor.screen_choice.get(), "Preflight")
        editor.select(root["id"])
        self.assertEqual(editor.parent_size, (1024, 520))

    def test_button_icon_import_layout_reopen_and_remove(self):
        from PIL import Image
        editor = self.editor
        asset = self.tmp / "light.png"
        Image.new("RGBA", (65, 17), (240, 48, 22, 160)).save(asset)
        editor.select("complete")
        with patch.object(studio.filedialog, "askopenfilename", return_value=str(asset)):
            editor.choose_icon()
        key = "assets/light.png"
        self.assertEqual(editor.nodes["complete"]["props"]["icon"], key)
        self.assertEqual(editor.icon_thumbs[key].size, (32, 8))
        self.assertTrue(any(editor.canvas.type(i) == "image" for i in editor.canvas.find_withtag("node:complete")))
        editor.select("reset")
        editor.fields["icon"].set(key)
        editor.apply_widget()
        packaged = self.tmp / "icons.kvmpack"
        packaged.write_bytes(editor.current.package())
        with patch.object(studio.messagebox, "askyesno", return_value=True):
            editor.open(packaged)
        self.assertEqual(editor.nodes["reset"]["props"]["icon"], key)
        self.assertEqual(editor.current.members[key], asset.read_bytes())
        editor.select("reset")
        editor.fields["w"].set(60)
        editor.apply_widget()
        self.assertFalse(any(editor.canvas.type(i) == "image" for i in editor.canvas.find_withtag("node:reset")))
        editor.select("complete")
        editor.fields["icon"].set("")
        editor.apply_widget()
        self.assertNotIn("icon", editor.nodes["complete"]["props"])
        self.assertEqual(editor.nodes["reset"]["props"]["icon"], key)
        editor.select("status")
        self.assertIn("disabled", editor.combos["icon"].state())

    def setUp(self):
        self.tmp = Path(tempfile.gettempdir()) / ("kvmpad-studio-test-" + uuid.uuid4().hex)
        self.tmp.mkdir()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        self.editor = studio.Editor(self.root, studio.EXAMPLES / "flight-checklist")
        self.root.update_idletasks()

    @unittest.skipUnless(sys.platform == "win32", "Windows system palette")
    def test_windows_layout_uses_contrast_scheme_before_light_preference(self):
        import winreg

        def high_contrast(action, size, params, flags):
            self.assertEqual((action, size, flags), (0x42, studio.ctypes.sizeof(studio.HighContrast), 0))
            studio.ctypes.cast(params, studio.ctypes.POINTER(studio.HighContrast)).contents.flags = 1
            return 1

        user = studio.ctypes.windll.user32
        with patch.object(user, "SystemParametersInfoW", side_effect=high_contrast), \
             patch.object(user, "GetSysColor", side_effect=lambda index: 0x335500 + index), \
             patch.object(winreg, "OpenKey") as registry:
            self.assertEqual(studio.layout_colors(), ("#055533", "#085533", "#0f5533", "#125533",
                                                     "#0d5533", "#0e5533", "#085533"))
            registry.assert_not_called()
        with patch.object(user, "SystemParametersInfoW", return_value=1), \
             patch.object(winreg, "OpenKey"), patch.object(winreg, "QueryValueEx") as preference:
            for value, palette in ((0, studio.NIGHT), (1, studio.DAY)):
                preference.return_value = (value, winreg.REG_DWORD)
                self.assertEqual(studio.layout_colors(), palette)
            preference.side_effect = FileNotFoundError
            self.assertEqual(studio.layout_colors(), studio.DAY)

    @unittest.skipUnless(sys.platform == "win32", "Windows system palette")
    def test_recolor_layout_preserves_edits_and_marks_selected_group(self):
        ed, canvas = self.editor, self.editor.canvas
        canvas.pack_forget()
        canvas.place(x=0, y=0, width=1024, height=600)
        self.root.update_idletasks()
        self.assertFalse(canvas.winfo_ismapped())
        ed.select("complete")
        ed.fields["label"].set("Pending author edit")
        members = dict(ed.current.members)
        bounds = canvas.coords(canvas.find_withtag("node:complete")[0])
        self.assertEqual(bounds, [32, 296, 312, 366])
        custom = studio.LayoutColors("#1b1034", "#fff56c", "#224466", "#ffffff",
                                     "#ffeb66", "#173722", "#fff56c")
        for palette in (studio.DAY, studio.NIGHT, custom):
            with self.subTest(palette=palette), patch.object(studio, "layout_colors", return_value=palette):
                canvas.after_cancel(ed.palette_timer)
                ed.poll_layout_colors()
                self.assertEqual(canvas.cget("background"), palette.page)
                for name, bg, ink in (("complete", palette.selection, palette.selection_text),
                                      ("reset", palette.control, palette.control_text)):
                    rect, label = canvas.find_withtag("node:" + name)
                    self.assertEqual(canvas.itemcget(rect, "fill"), bg)
                    self.assertEqual(canvas.itemcget(label, "fill"), ink)
                self.assertEqual(canvas.coords(canvas.find_withtag("node:complete")[0]), bounds)
                self.assertEqual(ed.fields["label"].get(), "Pending author edit")
                self.assertEqual(ed.current.members, members)
        ed.select("checklist")
        group = canvas.find_withtag("node:checklist")[0]
        ring, = canvas.find_withtag("selection")
        self.assertEqual(canvas.coords(group), [0, 32, 1024, 600])
        self.assertEqual(canvas.coords(ring), canvas.coords(group))
        self.assertEqual(canvas.find_all()[-1], ring)
        self.assertEqual(canvas.itemcget(ring, "outline"), custom.text)
        self.assertTrue(canvas.itemcget(ring, "dash"))
        self.assertEqual(canvas.itemcget(ring, "state"), "disabled")

    def test_layout_binding_and_coordinate_rollback(self):
        ed = self.editor
        ed.add("Button")
        self.assertEqual(ed.selected, "button_1")
        ed.fields["label"].set("Checklist confirmed")
        ed.fields["x"].set("85")
        ed.fields["action"].set("mark_complete")
        ed.apply_widget()
        self.assertEqual(ed.nodes["button_1"]["events"], {"tap": "mark_complete"})
        self.assertEqual(ed.nodes["button_1"]["props"]["label"], "Checklist confirmed")
        self.assertNotIn("tap", ed.nodes["complete"]["events"])
        original = dict(ed.current.members)
        ed.fields["w"].set("2048")
        with self.assertRaises(ValueError):
            ed.apply_widget()
        self.assertEqual(ed.current.members, original)
        ed.select("status")
        ed.fields["source"].set("check")
        ed.apply_widget()
        self.assertIn(
            {"from": "check", "to": "status.text", "transform": "identity"},
            ed.current.doc("bindings.json")["connections"],
        )

    def test_offscreen_canvas_pointer_drag(self):
        ed = self.editor
        self.root.geometry("1150x720+20000+20000")
        self.root.deiconify()
        self.root.update()
        b = ed.nodes["complete"]["bounds"]
        x = round(ed.ox + (b["x"] + b["w"] / 2) * ed.scale)
        y = round(ed.oy + (32 + b["y"] + b["h"] / 2) * ed.scale)
        ed.canvas.event_generate("<Motion>", x=x, y=y)
        ed.canvas.event_generate("<ButtonPress-1>", x=x, y=y)
        ed.canvas.event_generate("<B1-Motion>", x=x + 16, y=y + 8)
        ed.canvas.event_generate("<ButtonRelease-1>", x=x + 16, y=y + 8)
        self.assertEqual(ed.selected, "complete")
        self.assertEqual(ed.nodes["complete"]["bounds"]["x"], b["x"] + round(16 / ed.scale))
        self.assertEqual(ed.nodes["complete"]["bounds"]["y"], b["y"] + round(8 / ed.scale))

    def drag_pointer_viewport(self, motion, release):
        ed = self.editor
        self.root.geometry("1150x720+20000+20000")
        self.root.deiconify()
        self.root.update()
        ed.screen_choice.current(1)
        ed.screen_choice.event_generate("<<ComboboxSelected>>")
        self.root.update()
        rect = ed.canvas.find_withtag("node:pointer_desktop")[0]
        left, top, right, bottom = ed.canvas.coords(rect)
        x, y = round((left + right) / 2), round((top + bottom) / 2)
        ed.canvas.event_generate("<Motion>", x=x, y=y)
        ed.canvas.event_generate("<ButtonPress-1>", x=x, y=y)
        self.assertEqual(ed.selected, "pointer_desktop")
        ed.canvas.event_generate("<B1-Motion>", x=x + motion[0], y=y + motion[1])
        ed.canvas.event_generate("<ButtonRelease-1>", x=x + release[0], y=y + release[1])

    def test_viewport_drag_exports_final_release_coordinates(self):
        ed = self.editor
        ed.open(studio.USB_CONSOLE)
        expected = {n["id"]: n["bounds"] for n in studio.walk(ed.current.doc("ui.json")["root"])}
        viewport = expected["pointer_desktop"]
        self.drag_pointer_viewport((0, 6), (45, 24))
        viewport["x"] = 600 - viewport["w"]
        viewport["y"] += round(24 / ed.scale)
        output = self.tmp / "moved.kvmpack"
        with patch.object(studio.filedialog, "asksaveasfilename", return_value=output):
            ed.package()
        ui = json.loads(kvmpack.read_members(output)["ui.json"])
        self.assertEqual({n["id"]: n["bounds"] for n in studio.walk(ui["root"])}, expected)

    def test_viewport_drag_back_preserves_signed_package(self):
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

        key = Ed25519PrivateKey.generate()
        signed = kvmpack.build(kvmpack.read_members(studio.USB_CONSOLE), key)
        source = self.tmp / "signed.kvmpack"
        source.write_bytes(signed)
        self.editor.open(source, key.public_key())
        self.drag_pointer_viewport((0, 18), (0, 0))
        output = self.tmp / "unchanged.kvmpack"
        with patch.object(studio.filedialog, "asksaveasfilename", return_value=output):
            self.editor.package()
        self.assertEqual(output.read_bytes(), signed)

    def test_status_chip_role_round_trip(self):
        ed = self.editor
        ed.select("status")
        self.assertEqual(ed.fields["chip_role"].get(), "info")
        for role, symbol in kvmpack.CHIP_SYMBOLS.items():
            ed.fields["chip_role"].set(role)
            ed.apply_widget()
            self.assertEqual(ed.nodes["status"]["props"], {"text": "Pending", "status": role})
            items = ed.canvas.find_withtag("node:status")
            self.assertIn(symbol, [ed.canvas.itemcget(i, "text") for i in items if ed.canvas.type(i) == "text"])
        unchanged = ed.current.members.copy()
        ed.fields["chip_role"].set("unknown")
        with self.assertRaises(ValueError):
            ed.apply_widget()
        self.assertEqual(unchanged, ed.current.members)
        ed.select("complete")
        self.assertTrue(ed.combos["chip_role"].instate(["disabled"]))

    def test_palette_matches_schema_and_respects_declared_permissions(self):
        ed = self.editor
        schema = json.loads((kvmpack.SCHEMAS / "kvmpack-ui.schema.json").read_bytes())
        self.assertEqual(set(ed.widget_kind["values"]), set(schema["$defs"]["node"]["properties"]["type"]["enum"]))
        for kind in ed.widget_kind["values"]:
            with self.subTest(kind=kind):
                old = ed.current.members.copy()
                if kind in kvmpack.USB_SURFACES:
                    with self.assertRaisesRegex(ValueError, "monitor_controls"):
                        ed.current.add_widget(kind)
                    self.assertEqual(old, ed.current.members)
                    continue
                if kind == "ChartViewer":
                    with patch.object(studio.filedialog, "askopenfilenames", return_value=()):
                        ed.add(kind)
                    self.assertEqual(old, ed.current.members)
                    continue
                if kind in ("AutopilotPanel", "RadioPanel"):
                    with self.assertRaises(ValueError):
                        ed.current.add_widget(kind)
                    self.assertEqual(old, ed.current.members)
                    continue
                if kind in ("AxisPad", "Slider", "StreamViewport"):
                    with self.assertRaisesRegex(ValueError, "Manifest"):
                        ed.add(kind)
                    self.assertEqual(old, ed.current.members)
                    example = "cursor-magnifier" if kind == "StreamViewport" else "flight-yoke"
                    project = studio.Project(kvmpack.read_members(studio.EXAMPLES / example))
                    project.add_widget(kind)
                    project.package()
                    continue
                ed.widget_kind.set(kind)
                ed.add_button.invoke()
                self.assertEqual(ed.nodes[ed.selected]["type"], kind)
                self.assertEqual(old["manifest.json"], ed.current.members["manifest.json"])
                self.assertEqual(old["bindings.json"], ed.current.members["bindings.json"])
        kvmpack.validate_scene(ed.current.members)

    def test_chart_viewer_imports_two_images_and_preserves_project_on_failure(self):
        from PIL import Image
        editor = self.editor
        ui, bindings = (editor.current.doc(name) for name in ("ui.json", "bindings.json"))
        ui["root"]["children"] = []
        for key in ("actions", "connections", "state"):
            bindings[key] = []
        editor.current.update({"ui.json":studio.formatted(ui), "bindings.json":studio.formatted(bindings)})
        editor.refresh()
        images = [self.tmp / "Taxi.jpg", self.tmp / "Apron.webp"]
        for path in images:
            Image.new("RGB", (1000, 600), (100, 60, 20)).save(path)
        with patch.object(studio.filedialog, "askopenfilenames", return_value=images):
            editor.add("ChartViewer")
        node = editor.nodes[editor.selected]
        self.assertEqual([c["title"] for c in node["props"]["charts"]], ["Taxi","Apron"])
        kvmpack.validate_scene(editor.current.members)
        self.assertIn("data:image/png;base64,",kvmpack.preview_html(editor.current.members))
        before = dict(editor.current.members)
        with self.assertRaises(ValueError):
            editor.current.add_widget("ChartViewer", ui["root"]["id"], images)
        self.assertEqual(editor.current.members, before)

    def test_nested_palette_drag_and_parent_bounds(self):
        ed = self.editor
        self.root.geometry("1150x720+20000+20000")
        self.root.deiconify()
        self.root.update()
        ed.add("Row")
        row = ed.selected
        ed.fields["x"].set("100")
        ed.fields["y"].set("220")
        ed.apply_widget()
        ed.parent_choice.set(row)
        ed.add("Text")
        child = ed.selected
        rect = ed.canvas.coords(ed.canvas.find_withtag("node:" + child)[0])
        self.assertAlmostEqual(rect[0], ed.ox + 100 * ed.scale)
        self.assertAlmostEqual(rect[1], ed.oy + (32 + 220) * ed.scale)
        x, y = round((rect[0] + rect[2]) / 2), round((rect[1] + rect[3]) / 2)
        ed.canvas.event_generate("<Motion>", x=x, y=y)
        ed.canvas.event_generate("<ButtonPress-1>", x=x, y=y)
        ed.canvas.event_generate("<B1-Motion>", x=x + 900, y=y + 600)
        ed.canvas.event_generate("<ButtonRelease-1>", x=x + 900, y=y + 600)
        self.assertEqual(ed.nodes[child]["bounds"], {"x": 140, "y": 104, "w": 180, "h": 56})
        ed.choose(row)
        before = ed.current.members.copy()
        ed.fields["w"].set("200")
        with self.assertRaises(ValueError):
            ed.apply_widget()
        self.assertEqual(before, ed.current.members)

    def test_json_tabs_preview_and_required_permissions(self):
        ed = self.editor
        ed.open(studio.EXAMPLES / "flight-instruments")
        ed.select("heading")
        ed.fields["source"].set("ias")
        ed.apply_widget()
        self.assertEqual(
            next(
                c["from"]
                for c in ed.current.doc("bindings.json")["connections"]
                if c["to"] == "heading.value"
            ),
            "ias",
        )
        manifest = ed.current.doc("manifest.json")
        manifest["title"] = "Cockpit </script>"
        tab = ed.documents["manifest.json"]
        tab.delete("1.0", "end")
        tab.insert("1.0", json.dumps(manifest))
        ed.validate()
        with patch.object(studio.messagebox, "showinfo") as dialog:
            ed.permissions()
            self.assertIn("host.telemetry.read", dialog.call_args.args[1])
            self.assertIn("telemetry.sim", dialog.call_args.args[1])
        ed.preview_dir = self.tmp
        with patch.object(studio.webbrowser, "open", return_value=True) as browser:
            ed.simulate()
            self.assertEqual(browser.call_args.args, ((self.tmp / "preview-1.html").as_uri(),))
        html = (self.tmp / "preview-1.html").read_text(encoding="utf-8")
        self.assertNotIn("Cockpit </script>", html)
        self.assertIn("Demo telemetry", html)
        tab.insert("end", "garbage")
        ed.run(ed.validate)
        self.assertNotIn("validated", ed.status.get())
        self.assertEqual(ed.current.manifest["title"], "Cockpit </script>")

    def test_validate_selects_the_offending_binding_in_the_draft(self):
        ed = self.editor
        ed.open(studio.EXAMPLES / "flight-instruments")
        tab = ed.documents["bindings.json"]
        original = tab.get("1.0", "end-1c")
        broken = original.replace('"from": "hdg"', '"from": "unknown_source"')
        tab.delete("1.0", "end")
        tab.insert("1.0", broken)
        ed.run(ed.validate)
        self.assertIn("$.connections[2].from [BINDING_SOURCE]", ed.status.get())
        self.assertEqual(ed.notebook.select(), str(tab.master))
        self.assertEqual(tab.get("insert", "insert + 16 chars"), '"unknown_source"')
        self.assertEqual(tab.get("1.0", "end-1c"), broken)
        self.assertEqual(ed.current.doc("bindings.json")["connections"][2]["from"], "hdg")
        tab.delete("insert", "insert + 16 chars")
        tab.insert("insert", '"hdg"')
        ed.run(ed.validate)
        self.assertIn("validated", ed.status.get())

    def test_portable_outputs_and_card_retries(self):
        ed = self.editor
        ed.add("Text")
        ed.fields["label"].set("Saved pending inspector edit")
        source = self.tmp / "source"
        archive = self.tmp / "edited.kvmpack"
        report = self.tmp / "report.json"
        with patch.object(
            studio.filedialog, "asksaveasfilename", side_effect=[source, archive, report]
        ):
            ed.save_source()
            ed.package()
            ed.diagnostics()
        self.assertEqual(kvmpack.read_members(source), ed.current.members)
        self.assertIn(b"Saved pending inspector edit", ed.current.members["ui.json"])
        self.assertEqual(kvmpack.read_members(archive), ed.current.members)
        self.assertEqual(json.loads(report.read_text())["package_bytes"], archive.stat().st_size)
        with patch.object(studio.filedialog, "askdirectory", return_value=self.tmp):
            ed.install()
            installer = self.tmp / f"pack-{ed.current.manifest['id']}-{ed.current.manifest['version']}.kvmpack"
            self.assertEqual(installer.read_bytes(), archive.read_bytes())
            with self.assertRaises(FileExistsError):
                ed.install()
            self.assertEqual(installer.read_bytes(), archive.read_bytes())

    def test_edit_and_repair_a_7b_png_budget_before_installation(self):
        from PIL import Image
        ed = self.editor
        folder = self.tmp / "project"
        shutil.copytree(studio.EXAMPLES / "flight-checklist", folder)
        assets = folder / "assets"
        assets.mkdir()
        Image.new("RGBA", (512, 1), (25, 70, 155, 255)).save(assets / "sample.png")
        manifest = ed.current.doc("manifest.json")
        manifest["limits"]["max_memory_kib"] = 49
        (folder / "manifest.json").write_bytes(studio.formatted(manifest))
        ed.open(folder)
        self.assertIn("assets/sample.png needs at least 63 KiB", ed.status.get())
        with patch.object(paired_install, "dialog") as transfer, patch.object(studio.filedialog, "asksaveasfilename") as save:
            for command in (ed.validate, ed.package, ed.install_paired):
                with self.assertRaises(kvmpack.ProfileBudgetError):
                    command()
            save.assert_not_called()
            transfer.assert_not_called()
        report = ed.current.diagnostics()
        self.assertEqual((report["status"], report["max_memory_kib"], report["minimum_load_kib"]), ("invalid", 49, 63))
        self.assertNotIn("sample.png", json.dumps(report))
        tab = ed.documents["manifest.json"]
        manifest["limits"]["max_memory_kib"] = 63
        tab.delete("1.0", "end")
        tab.insert("1.0", json.dumps(manifest))
        ed.validate()
        with patch.object(paired_install, "dialog") as transfer:
            ed.install_paired()
        transfer.assert_called_once_with(ed.root, ed.current.package())
        self.assertEqual(ed.current.members["assets/sample.png"], (assets / "sample.png").read_bytes())

    def test_verified_package_keeps_publisher_bytes_for_card_and_paired_install(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        ed = self.editor
        key = Ed25519PrivateKey.generate()
        pem = self.tmp / "publisher.pem"
        def save_public(private):
            pem.write_bytes(private.public_key().public_bytes(serialization.Encoding.PEM,
                                                              serialization.PublicFormat.SubjectPublicKeyInfo))

        members = dict(ed.current.members)
        icon = (SDK.parents[1] / "design/assets/control-icons/video.png").read_bytes()
        members["assets/media/video.png"] = icon
        signed = kvmpack.build(members, key)
        package = self.tmp / "signed.kvmpack"
        package.write_bytes(signed)
        previous = ed.current
        with patch.object(studio.filedialog, "askopenfilename", return_value=""):
            ed.open(package)
        self.assertIs(ed.current, previous)
        save_public(Ed25519PrivateKey.generate())
        with patch.object(studio.filedialog, "askopenfilename", return_value=pem):
            with self.assertRaisesRegex(ValueError, "untrusted package signature"):
                ed.open(package)
            self.assertIs(ed.current, previous)
            save_public(key)
            ed.open(package)
        self.assertIn("Verified publisher signature", ed.status.get())
        with patch.object(studio.filedialog, "askdirectory", return_value=self.tmp):
            ed.install()
        self.assertEqual(next(self.tmp.glob("pack-*.kvmpack")).read_bytes(), signed)
        with patch.object(paired_install, "dialog") as transfer:
            ed.install_paired()
            transfer.assert_called_once_with(self.root, signed)

        ed.select("title")
        ed.fields["label"].set("Preflight checks")
        ed.apply_widget()
        with patch.object(paired_install, "dialog") as transfer:
            with self.assertRaisesRegex(ValueError, "changes the signed package"):
                ed.install_paired()
            transfer.assert_not_called()
        for name, box in ed.documents.items():
            box.delete("1.0", "end")
            box.insert("1.0", members[name].decode())
        ed.sync()
        self.assertEqual(ed.current.package(), signed)
        ed.select("title")
        ed.fields["label"].set("Preflight checks")
        source = self.tmp / "editable"
        with patch.object(studio.filedialog, "asksaveasfilename", return_value=source):
            ed.save_source()
        self.assertIn("unsigned source", ed.status.get())
        derivative = self.tmp / "edited.kvmpack"
        derivative.write_bytes(ed.current.package())
        edited = kvmpack.read_members(derivative)
        self.assertIn(b"Preflight checks", edited["ui.json"])
        self.assertEqual(edited["assets/media/video.png"], icon)
        self.assertEqual(kvmpack.read_members(package, key.public_key()), members)

    def test_install_cli_uses_verified_archive_and_paired_id(self):
        archive = self.tmp / "install.kvmpack"
        source = self.editor.current.package()
        archive.write_bytes(source)
        args = ["kvmpack", "install", "--device", "Pair-7B", str(archive)]
        with patch.object(sys, "argv", args), patch.object(paired_install, "start") as start, \
             patch.object(paired_install, "finish") as finish:
            kvmpack.main()
            start.assert_called_once_with("Pair-7B")
            finish.assert_called_once_with(start.return_value, source)
        archive.write_bytes(b"broken")
        with patch.object(sys, "argv", args), patch.object(paired_install, "start") as start:
            with self.assertRaises(studio.BadZipFile):
                kvmpack.main()
            start.assert_not_called()

    def test_paired_dialog_follows_client_progress_before_exit(self):
        archive = self.editor.current.package()
        (self.tmp / "expected").write_bytes(archive)
        helper = """
import pathlib, sys, time
assert sys.stdin.buffer.read() == pathlib.Path("expected").read_bytes()
for phase in ("transfer", "review", "committing"):
    print(phase, flush=True)
    while not pathlib.Path(phase).exists():
        time.sleep(.01)
"""
        real_popen = subprocess.Popen
        seen = []
        children = []

        def launch(command, **kwargs):
            self.assertEqual(command[1:], ["--pack-install", "Pair-7B"])
            child = real_popen([sys.executable, "-I", "-c", helper], cwd=self.tmp, **kwargs)
            children.append(child)
            return child

        def observe():
            top = next(w for w in self.root.winfo_children() if isinstance(w, tk.Toplevel))
            body, = top.winfo_children()
            options, label, actions = body.winfo_children()
            send, close = actions.winfo_children()
            if not children:
                send.invoke()
            message = label.cget("text")
            for key, text in paired_install.PHASES.items():
                if message == text and key not in seen:
                    seen.append(key)
                    seen.append((str(close.cget("text")), children[0].poll(), options.instate(["disabled"])))
                    (self.tmp / key.decode()).touch()
            if message == "Installation confirmed by the device.":
                seen.append(("confirmed", children[0].poll()))
                close.invoke()
            else:
                self.root.after(15, observe)

        def timeout():
            for top in self.root.winfo_children():
                if isinstance(top, tk.Toplevel):
                    top.destroy()

        timer = self.root.after(8000, timeout)
        self.root.after(0, observe)
        with patch.object(paired_install, "paired_devices", return_value=[{"name":"Test", "id":"Pair-7B"}]), \
             patch.object(paired_install, "client_path", return_value="KVMpad.Client.exe"), \
             patch.object(subprocess, "Popen", side_effect=launch):
            self.editor.install_paired()
        self.root.after_cancel(timer)
        self.assertEqual(seen, [b"transfer", ("Cancel", None, True), b"review", ("Cancel", None, True),
                                b"committing", ("Close", None, True), ("confirmed", 0)])

    def test_cli_stages_two_versions_as_reviewable_root_installers(self):
        root = self.tmp / "card"
        root.mkdir()
        source = self.tmp / "project"
        shutil.copytree(studio.EXAMPLES / "flight-checklist", source)
        args = ["kvmpack", "stage", str(source), "--card", str(root)]
        for version in ("1.0.0", "1.1.0"):
            manifest = self.editor.current.doc("manifest.json")
            manifest["version"] = version
            (source / "manifest.json").write_bytes(studio.formatted(manifest))
            with patch.object(sys, "argv", args):
                kvmpack.main()
            dest = root / f"pack-{manifest['id']}-{version}.kvmpack"
            self.assertEqual(kvmpack.read_members(dest), kvmpack.read_members(source))
        self.assertEqual(len(list(root.iterdir())), 2)
        self.assertFalse((root / "KVPACKS").exists())
        with patch.object(sys, "argv", args), self.assertRaises(FileExistsError):
            kvmpack.main()

    def test_source_save_failure_leaves_existing_project_intact(self):
        ed = self.editor
        source = self.tmp / "project"
        with patch.object(studio.filedialog, "asksaveasfilename", return_value=source):
            with patch.object(studio, "write_file", side_effect=OSError("test interruption")):
                with self.assertRaises(OSError):
                    ed.save_source()
            self.assertEqual(list(self.tmp.iterdir()), [])
            ed.save_source()
            with self.assertRaises(ValueError):
                ed.save_source()
        self.assertEqual(kvmpack.read_members(source), ed.current.members)


if __name__ == "__main__":
    unittest.main()
