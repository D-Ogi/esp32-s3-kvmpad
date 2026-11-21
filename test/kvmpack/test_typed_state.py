# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run identical JSON, including invalid literals, through the pack compiler, device IR and offline preview."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "tools/kvmpack"))
import kvmpack


class TypedState(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = Path(tempfile.gettempdir()) / ("kvmpad-state-" + uuid.uuid4().hex)
        cls.folder.mkdir()
        cls.addClassCleanup(shutil.rmtree, cls.folder)
        cls.native = cls.folder / ("scene.exe" if os.name == "nt" else "scene")
        compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
        cls.has_compiler = bool(compiler)
        if compiler:
            scene = REPO / "components/pack_runtime"
            protocol = REPO / "components/remote_protocol"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                            "-I", str(scene / "include"), "-I", str(protocol / "include"),
                            str(HERE / "host/scene_vectors.c"), str(scene / "pack_scene.c"),
                            str(protocol / "src/remote_json.c"), "-lm", "-o", str(cls.native)], check=True)

    def files(self, kind, initial, assigned, bounds, numeric=False):
        files = kvmpack.read_members(REPO / "tools/kvmpack/examples/flight-checklist")
        b = json.loads(files["bindings.json"])
        b["state"] = [dict(id="check", type=kind, initial=initial, **bounds)]
        for action, value in zip(b["actions"], (assigned, initial)):
            action["args"]["value"] = value
        if numeric:
            ui = json.loads(files["ui.json"])
            label = next(n for n in ui["root"]["children"] if n["id"] == "item")
            label.update(type="ValueTile", props={"title":"Local", "unit":"kt"})
            files["ui.json"] = json.dumps(ui).encode()
            b["connections"].append({"from":"check", "to":"item.value", "transform":"identity"})
        files["bindings.json"] = json.dumps(b).encode()
        for name, data in files.items():
            (self.folder / name).write_bytes(data)
        return files

    def test_magnifier_mode_handoff(self):
        def files_for(mode, state_name="capture_mode", initial="OVERVIEW", zoom="ZOOM_2X",
                      consent=True, other_profile=False, number=False):
            files=kvmpack.read_members(REPO / "tools/kvmpack/examples/cursor-magnifier")
            bindings=json.loads(files["bindings.json"])
            state=bindings["state"][0]
            state.update(id=state_name, initial=0 if number else initial, type="int" if number else "string")
            for action in bindings["actions"][:3]:
                action["args"]["name"]=state_name
                if number: action["args"]["value"]=0
            if not number: bindings["actions"][1]["args"]["value"]=zoom
            bindings["actions"][3]["args"].update(mode=mode, requires_foreground_consent=consent)
            files["bindings.json"]=json.dumps(bindings).encode()
            if other_profile:
                manifest=json.loads(files["manifest.json"])
                manifest["id"]="org.kvmpad.test.other"
                files["manifest.json"]=json.dumps(manifest).encode()
            for name, data in files.items(): (self.folder/name).write_bytes(data)
            return files

        modes=["OVERVIEW","ZOOM_2X","PIXEL_1X"]
        taps=[*modes,"OVERVIEW"]
        accepted=[({"mode":mode},[mode]*4) for mode in modes]
        accepted.extend([
            ({"mode":"$state.capture_mode"},taps),
            ({"mode":"$state.crop","state_name":"crop"},taps),
            ({"mode":"$state.capture_mode","initial":"SIDEWAYS","zoom":"ZZ"},
             ["invalid","invalid","PIXEL_1X","OVERVIEW"]),
            ({"mode":"PIXEL_1X","other_profile":True},["PIXEL_1X"]*4),
        ])
        for case, result in accepted:
            with self.subTest(case=case):
                kvmpack.validate_scene(files_for(**case))
                if self.has_compiler:
                    subprocess.run([self.native,"magnifier",self.folder,*result],check=True)
                if os.environ.get("KVM_STATE_VIEWS"):
                    subprocess.run([os.environ["KVM_STATE_VIEWS"],"--magnifier-modes",self.folder,*result],check=True)
        for case in [{"mode":"NOPE"},{"mode":"$state.missing"},{"mode":"$state."},{"mode":True},
                     {"mode":"$state.capture_mode","number":True},{"mode":"OVERVIEW","consent":False}]:
            with self.subTest(case=case):
                files=files_for(**case)
                with self.assertRaises(ValueError): kvmpack.validate_scene(files)
                if self.has_compiler: subprocess.run([self.native,"reject",self.folder],check=True)

    def test_composite_usb_scene_contract(self):
        if not self.has_compiler:
            self.skipTest("A native compiler is required")
        source = REPO / "extensions/usb-monitor-console"
        for name in ("manifest.json", "ui.json", "bindings.json"):
            shutil.copyfile(source / name, self.folder / name)
        subprocess.run([self.native, "accept", self.folder], check=True)
        for filename, path, value in [
            ("manifest.json", ("usb", "mode"), "static_superset"),
            ("manifest.json", ("required_permissions",), ["usb.monitor", "hid.mouse"]),
            ("manifest.json", ("usb", "requires"), ["usb.monitor", "hid.keyboard"]),
            ("ui.json", ("root", "children", 0, "children", 2, "type"), "TouchpadSurface"),
            ("ui.json", ("root", "children", 0, "children", 2, "bounds", "w"), 439),
            ("ui.json", ("root", "children", 1, "children", 0, "props"), {"fit": "stretch"}),
        ]:
            with self.subTest(path=path, value=value):
                document = json.loads((source / filename).read_bytes())
                target = document
                for part in path[:-1]:
                    target = target[part]
                target[path[-1]] = value
                (self.folder / filename).write_text(json.dumps(document), encoding="utf-8")
                subprocess.run([self.native, "reject", self.folder], check=True)
                shutil.copyfile(source / filename, self.folder / filename)

    def test_screen_switcher(self):
        def node(key, kind, x, y, w, h, **rest):
            return dict(id=key, type=kind, bounds=dict(x=x, y=y, w=w, h=h), **rest)

        folders = []
        for orientation, width, height in [("landscape", 1024, 520), ("portrait", 600, 944)]:
            folder = self.folder / orientation
            folder.mkdir(exist_ok=True)
            files = kvmpack.read_members(REPO / "tools/kvmpack/examples/flight-checklist")
            root = node("checklist", "Column", 0, 0, width, height)
            # Deliberately store children in a different order than the picker.
            root["children"] = [
                node("setup", "Column", 0, 0, width, height, props={"title": "Setup"}, children=[
                    node("config", "Text", 40, 40, 220, 64, props={"text": "Setup controls"})]),
                node("radio", "Column", 0, 0, width, height, props={"title": "Radio"}, children=[
                    node("set", "Button", 40, 40, 160, 64, props={"label": "Tune"}, events={"tap": "tune"}),
                    node("station", "Text", 220, 40, 240, 64, props={"text": "Standby"})])]
            ui = dict(version=1, orientation=orientation, screens=["radio","setup"], root=root)
            bindings = {
                "version": 1, "sources": [], "state": [dict(id="frequency", type="string", initial="Standby")],
                "connections": [{"from": "frequency", "to": "station.text", "transform": "identity"}],
                "actions": [dict(id="tune", trigger="set.tap", type="state.set", capability="ui.basic",
                                 args={"name": "frequency", "value": "121.500 MHz"})]}
            files.update({"ui.json": json.dumps(ui).encode(), "bindings.json": json.dumps(bindings).encode()})
            kvmpack.validate_scene(files)
            for name, data in files.items():
                (folder / name).write_bytes(data)
            folders.append(folder)
        if os.environ.get("KVM_STATE_VIEWS"):
            subprocess.run([os.environ["KVM_STATE_VIEWS"], "--screens", *folders], check=True)

    def test_chip_roles(self):
        files = kvmpack.read_members(REPO / "tools/kvmpack/examples/flight-checklist")
        ui = json.loads(files["ui.json"])
        chips = []
        for i, role in enumerate((*kvmpack.CHIP_SYMBOLS, "default")):
            props = {"label": role}
            if role != "default":
                props["status"] = role
            chips.append(dict(id=f"chip_{i}", type="StatusChip", props=props,
                              bounds=dict(x=24 + 360 * (i // 4), y=24 + 82 * (i % 4), w=260, h=64)))
        ui["root"]["children"] = chips
        files["ui.json"] = json.dumps(ui).encode()
        files["bindings.json"] = b'{"version":1,"sources":[],"state":[],"connections":[],"actions":[]}'
        kvmpack.validate_scene(files)
        for name, data in files.items():
            (self.folder / name).write_bytes(data)
        if os.environ.get("KVM_STATE_VIEWS"):
            subprocess.run([os.environ["KVM_STATE_VIEWS"], "--chips", self.folder], check=True)
        chips[0]["props"]["status"] = "unknown"
        files["ui.json"] = json.dumps(ui).encode()
        (self.folder / "ui.json").write_bytes(files["ui.json"])
        with self.assertRaises(ValueError):
            kvmpack.validate_scene(files)
        if self.has_compiler:
            subprocess.run([self.native, "reject", self.folder], check=True)

    def test_local_controls(self):
        def node(key, kind, x, y, w, h, **rest):
            return dict(id=key, type=kind, bounds=dict(x=x,y=y,w=w,h=h), **rest)
        ui = {"version":1, "root":node("checklist","Column",0,0,1024,568,children=[
            node("pad","TouchArea",10,10,200,100,events={"tap":"tap","long_press":"hold","drag":"drag"}),
            node("dial","Dial",245,10,144,160,props={"min":-100,"max":100,"value":0},events={"rotate":"rot"}),
            node("tabs","TabStrip",10,210,200,54,props={"pages":["one","two"]},events={"change":"tab"}),
            node("chip","StatusChip",450,10,170,65,props={"status":"success"}),
            node("one","Column",10,280,220,100,children=[node("label","Text",5,5,200,50,props={"text":"Page one"})]),
            node("two","Column",10,280,220,100,children=[node("other","Text",5,5,200,50,props={"text":"Page two"})])
        ])}
        bindings = {"version":1,"sources":[],"state":[{"id":"n","type":"int","initial":0,"min":-100,"max":200}],
                    "connections":[{"from":"n","to":"chip.text","transform":"identity"}],"actions":[]}
        for key, trigger, value in [("tap","pad.tap",1),("hold","pad.long_press",2),("drag","pad.drag",{"event":"x"}),
                                    ("rot","dial.rotate",{"event":"value"}),("tab","tabs.change",{"event":"value"})]:
            bindings["actions"].append(dict(id=key,trigger=trigger,type="state.set",capability="ui.basic",
                                            args={"name":"n","value":value}))
        def write(u, b):
            files=kvmpack.read_members(REPO / "tools/kvmpack/examples/flight-checklist")
            files["ui.json"], files["bindings.json"]=json.dumps(u).encode(),json.dumps(b).encode()
            for name, data in files.items():
                (self.folder / name).write_bytes(data)
            return files
        files=write(ui,bindings)
        kvmpack.validate_scene(files)
        html=self.folder/"controls.html"
        html.write_text(kvmpack.preview_html(files),encoding="utf-8")
        if os.environ.get("KVM_CONTROL_FIXTURE"):
            out=Path(os.environ["KVM_CONTROL_FIXTURE"])
            for name,data in files.items(): (out/name).write_bytes(data)
            (out/html.name).write_bytes(html.read_bytes())
        if os.environ.get("KVM_STATE_VIEWS"):
            subprocess.run([os.environ["KVM_STATE_VIEWS"],"--controls",self.folder],check=True)
        if shutil.which("node"):
            subprocess.run(["node",HERE/"local_controls.cjs",html],check=True)
        from copy import deepcopy
        cases=[("ui", [0,"events","rotate"],"rot"), ("ui",[1,"props","min"],True),
               ("ui",[1,"props","value"],101), ("ui",[4,"children",0,"bounds","w"],221),
               ("ui",[2,"props","tabs"],["repeat","repeat"]),("ui",[2,"props","tabs"],["a"]*5),
               ("ui",[2,"props","pages"],["one","one"]),("ui",[2,"props","pages"],["label","two"]),
               ("ui",[2,"props","selected"],2),("ui",[3,"props","status"],"custom"),
               ("ui",[3,"props","label"],"A\0B"),("state",[0,"max"],198),
               ("actions",[2,"args","value"],{"event":"value"}),("actions",[3,"args","value"],{"event":"x"}),
               ("actions",[0,"type"],"ui.navigate"),("actions",[1,"args","value"],{"event":"y"})]
        for where, path, value in cases:
            with self.subTest(where=where,path=path,value=value):
                u,b=deepcopy(ui),deepcopy(bindings)
                obj=u["root"]["children"] if where=="ui" else b[where]
                for k in path[:-1]: obj=obj[k]
                obj[path[-1]]=value
                files=write(u,b)
                with self.assertRaises(ValueError): kvmpack.validate_scene(files)
                if self.has_compiler: subprocess.run([self.native,"reject",self.folder],check=True)

    def test_values_and_bindings(self):
        cases = [
            ("string", "", "Zażółć", {}, ["", "Zażółć"]),
            ("bool", False, True, {}, ["false", "true"]),
            ("int", -(1 << 63), (1 << 63) - 1, {"min":-(1 << 63), "max":(1 << 63) - 1},
             ["-9223372036854775808", "9223372036854775807"]),
            ("int", -1, 3, {"min":-1.5, "max":3.25}, ["-1", "3"]),
            ("float", 0, -0.125, {"min":-0.125, "max":0}, ["0", "-0.125"]),
            ("float", 1.25e-9, 1.7976931348623157e308, {}, ["1.25e-09", "1.79769313486232e+308"]),
            ("float", -0.0, 123456789012344.5, {}, ["0", "123456789012344"]),
        ]
        for kind, initial, assigned, bounds, pair in cases:
            with self.subTest(kind=kind, initial=initial):
                numeric = kind in ("int", "float")
                files = self.files(kind, initial, assigned, bounds, numeric)
                kvmpack.validate_scene(files)
                html = self.folder / "preview.html"
                html.write_text(kvmpack.preview_html(files), encoding="utf-8")
                expected = pair + pair[:1]
                if self.has_compiler:
                    actual = subprocess.check_output([self.native, "show-state", self.folder])
                    self.assertEqual(actual.decode("utf-8").splitlines(), expected)
                if os.environ.get("KVM_STATE_VIEWS"):
                    actual = subprocess.check_output([os.environ["KVM_STATE_VIEWS"], "--state", self.folder])
                    self.assertEqual(actual.decode("utf-8").splitlines()[-3:], expected)
                if shutil.which("node"):
                    script = """
const {fixture} = require(process.argv[1]);
const {widget} = fixture(process.argv[2]);
function read() {return [widget('status').children[0].textContent, widget('item').children[1]?.textContent]}
const values = [read()];
for (const id of ['complete', 'reset']) {widget(id).fire('click'); values.push(read())}
process.stdout.write(JSON.stringify(values));
"""
                    rows = json.loads(subprocess.check_output(["node", "-e", script, str(HERE / "studio_preview.cjs"), html]))
                    self.assertEqual([row[0] for row in rows], expected)
                    if numeric:
                        self.assertEqual([row[1] for row in rows], [v + " kt" for v in expected])
        print(f"{len(cases)} accepted profiles with initial, tap and reset. "
              f"Native IR={self.has_compiler}, LVGL={bool(os.environ.get('KVM_STATE_VIEWS'))}, "
              f"preview={bool(shutil.which('node'))}.")

    def test_invalid_declarations_and_actions(self):
        cases = [
            ("int", True, 1, {}), ("int", 1, True, {}), ("bool", True, 1, {}), ("float", False, 0.5, {}),
            ("float", 1.5, "1.5", {}), ("int", 1.0, 2, {}), ("string", "ok", 3, {}),
            ("string", "ok", "bad\0", {}), ("string", "é" * 49, "", {}),
            ("bool", False, True, {"min":0}), ("int", 1, 3, {"max":2.99}), ("float", 1, 0, {"min":2, "max":1}),
            ("int", 1 << 63, 0, {}), ("int", -(1 << 63) - 1, 0, {}),
            ("int", 1 << 53, (1 << 53) + 1, {"max":1 << 53}),
            ("float", float("nan"), 0, {}), ("float", 0, float("inf"), {}), ("float", 0, 1, {"max":float("inf")}),
            ("float", 10**400, 0, {}), ("float", 0, 1, {"min":-(10**400)}),
            ("string", "no", "yes", {}),  # incompatible numeric binding
        ]
        for index, (kind, initial, assigned, bounds) in enumerate(cases):
            with self.subTest(index=index):
                files = self.files(kind, initial, assigned, bounds, numeric=index==len(cases)-1)
                with self.assertRaises(ValueError):
                    kvmpack.validate_scene(files)
                if self.has_compiler:
                    subprocess.run([self.native, "reject", self.folder], check=True)
        print(f"{len(cases)} invalid declarations, bindings and assignments rejected.")

    def transform_scene(self, name, params=None):
        files = self.files("float", 0, 1, {})
        bindings = json.loads(files["bindings.json"])
        connection = bindings["connections"][0]
        connection["transform"] = name
        if params is not None:
            connection["params"] = params
        files["bindings.json"] = json.dumps(bindings).encode()
        (self.folder / "bindings.json").write_bytes(files["bindings.json"])
        return files

    def test_transforms(self):
        cases = [
            ("identity", None, [-3, 0.25], ["-3", "0.25"]),
            ("clamp", None, [-2, .5, 3], ["0", "0.5", "1"]),
            ("clamp", {"min":-2, "max":-1}, [-3, 0], ["-2", "-1"]),
            ("scale", {"factor":-2}, [4, 1.7e308], ["-8", "Unavailable"]),
            ("scale", None, [123456789012344.5], ["123456789012344"]),
            ("offset", {"amount":-5}, [4], ["-1"]),
            ("round", None, [-2.5, -.49, 3.5], ["-3", "0", "4"]),
            ("enum_map", None, [0, 1, 2, .5], ["Off", "On", "Unavailable", "Unavailable"]),
            ("enum_map", {"entries":[{"code":-4,"label":"Gear down"},{"code":10,"label":"Gear up"}]},
             [-4, 10, 0], ["Gear down", "Gear up", "Unavailable"]),
            ("format_number", None, [-.125, -.001, 1, 1e30], ["-0.13", "0.00", "1.00", "Unavailable"]),
            ("format_number", {"digits":0}, [-2.5, .5, 0], ["-3", "1", "0"]),
            ("boolean_threshold", {"threshold":-2}, [-2.1, -2], ["false", "true"]),
            ("hysteresis", {"low":-1,"high":2}, [1, 2, 0, -1, 2, "Stale", 1, 3, "Unavailable", 0],
             ["false", "true", "true", "false", "true", "Stale", "false", "true", "Unavailable", "false"]),
            ("lowpass", {"tau_ms":100}, [0, 10, 10, "Stale", 7, "Unavailable", -2],
             ["0", "6.32120558828558", "8.64664716763387", "Stale", "7", "Unavailable", "-2"]),
        ]
        for name, params, values, expected in cases:
            with self.subTest(name=name, params=params):
                files = self.transform_scene(name, params)
                kvmpack.validate_scene(files)
                # Cross the uint32 millisecond wrap on firmware; browser clocks remain monotonic.
                ticks = [(1 << 32) - 50 + 100 * i for i in range(len(values))]
                if self.has_compiler:
                    lines = "".join(f"{t % (1 << 32)} {x}\n" for t, x in zip(ticks, values))
                    actual = subprocess.check_output([self.native, "eval", self.folder], input=lines.encode())
                    self.assertEqual(actual.decode().splitlines(), expected)
                if shutil.which("node"):
                    html = self.folder / "preview.html"
                    html.write_text(kvmpack.preview_html(files), encoding="utf-8")
                    script = """
const {fixture} = require(process.argv[1]);
const {widget,run} = fixture(process.argv[2]);
// Use a fresh session after the automatic initial state application.
run('filters.clear()');
const output = JSON.parse(process.argv[3]).map(([t,x]) => {
    const stale = typeof x === "string" ? x : "";
    run('updateConnection(pack.bindings.connections[0],'+JSON.stringify(x)+',undefined,'+t+','+JSON.stringify(stale)+')');
    return widget('status').children[0].textContent;
});
process.stdout.write(JSON.stringify(output));
"""
                    actual = subprocess.check_output(["node", "-e", script, str(HERE / "studio_preview.cjs"), html,
                                                      json.dumps(list(zip(ticks, values)))])
                    self.assertEqual(json.loads(actual), expected)
        print(f"{len(cases)} transform vectors cover all 10 operations, time wrap, overflow and loss of freshness.")

    def test_invalid_transforms(self):
        cases = [
            ("scale", {"min":0}), ("round", {"digits":2}), ("identity", {"factor":1}),
            ("clamp", {"min":2}), ("hysteresis", {"low":1,"high":1}), ("lowpass", {"tau_ms":0}),
            ("lowpass", {"tau_ms":60001}), ("lowpass", {"tau_ms":1.0}), ("format_number", {"digits":7}),
            ("scale", {"factor":10**400}), ("offset", {"amount":False}),
            ("enum_map", {"entries":[{"code":3,"label":"A"},{"code":3,"label":"B"}]}),
            ("enum_map", {"entries":[{"code":0,"label":"é" * 25}]}),
            ("enum_map", {"entries":[{"code":1.0,"label":"x"}]}), ("enum_map", {"entries":[]}),
            ("eval", {}),
        ]
        for name, params in cases:
            with self.subTest(name=name, params=params):
                files = self.transform_scene(name, params)
                with self.assertRaises(ValueError):
                    kvmpack.validate_scene(files)
                if self.has_compiler:
                    subprocess.run([self.native, "reject", self.folder], check=True)

    def test_provider_freshness_in_view(self):
        files = self.files("float", 0, 1, {})
        inst = kvmpack.read_members(REPO / "tools/kvmpack/examples/flight-instruments")
        manifest = json.loads(inst["manifest.json"])
        manifest["id"] = "net.example.instrument.single"
        manifest["entry_view"] = json.loads(files["ui.json"])["root"]["id"]
        files["manifest.json"] = json.dumps(manifest).encode()
        bindings = json.loads(files["bindings.json"])
        bindings["sources"] = [{"id":"sensor", "topic":"sim.airspeed_kt", "type":"float", "max_age_ms":100}]
        bindings["connections"] = [
            {"from":"sensor", "to":"status.text", "transform":"lowpass", "params":{"tau_ms":100}},
            {"from":"sensor", "to":"item.text", "transform":"hysteresis", "params":{"low":1,"high":2}},
        ]
        files["bindings.json"] = json.dumps(bindings).encode()
        kvmpack.validate_scene(files)
        for name, data in files.items():
            (self.folder / name).write_bytes(data)
        if os.environ.get("KVM_STATE_VIEWS"):
            subprocess.run([os.environ["KVM_STATE_VIEWS"], "--telemetry", self.folder], check=True)
        if shutil.which("node"):
            html = self.folder / "preview.html"
            html.write_text(kvmpack.preview_html(files), encoding="utf-8")
            script = """
const {fixture}=require(process.argv[1]), assert=require("node:assert/strict");
const {widget,run}=fixture(process.argv[2], {now:()=>0});
const reading=()=>widget("status").children[0].textContent, flag=()=>widget("item").children[0].textContent;
assert.equal(reading(),"Unavailable");
run("readings.set('sensor',{value:0,time:0});refresh(0);readings.set('sensor',{value:3,time:60});refresh(60)");
assert.ok(Number(reading())>0 && Number(reading())<3); assert.equal(flag(),"true");
run("refresh(160)"); assert.equal(flag(),"true");
run("refresh(161)"); assert.equal(reading(),"Stale"); assert.equal(flag(),"Stale");
run("readings.set('sensor',{value:1.5,time:200});refresh(200)");
assert.equal(reading(),"1.5"); assert.equal(flag(),"false");
run("refresh(3200)"); assert.equal(reading(),"Unavailable");
run("readings.set('sensor',{value:13,time:3300});refresh(3300)");
assert.equal(reading(),"13"); assert.equal(flag(),"true");
run("openPack();readings.set('sensor',{value:1.5,time:0});refresh(0)");
assert.equal(reading(),"1.5"); assert.equal(flag(),"false");
"""
            subprocess.run(["node", "-e", script, str(HERE / "studio_preview.cjs"), html], check=True)

    def test_caps_and_cycles(self):
        files = self.files("float", 0, 1, {})
        ui = json.loads(files["ui.json"])
        bindings = json.loads(files["bindings.json"])
        for i in range(64):
            ui["root"]["children"].append({"id":f"output{i}", "type":"Text", "bounds":{"x":0,"y":0,"w":8,"h":8}})
        files["ui.json"] = json.dumps(ui).encode()
        conn = [{"from":"check", "to":f"output{i}.text", "transform":"enum_map",
                 "params":{"entries":[{"code":0,"label":"Off"}]}} for i in range(64)]
        for alteration in ("valid", "enum-budget", "cycle", "duplicate", "bind-budget"):
            with self.subTest(alteration=alteration):
                bindings["connections"] = json.loads(json.dumps(conn))
                if alteration == "enum-budget":
                    bindings["connections"][63]["params"]["entries"].append({"code":1,"label":"On"})
                if alteration == "duplicate":
                    bindings["connections"][1]["to"] = "output0.text"
                if alteration == "cycle":
                    bindings["state"] += [{"id":f"output{i}.text","type":"float","initial":0} for i in range(2)]
                    for i in range(2):
                        bindings["connections"][i]["from"] = f"output{1-i}.text"
                if alteration == "bind-budget":
                    bindings["connections"].append({"from":"check", "to":"status.text", "transform":"identity"})
                files["bindings.json"] = json.dumps(bindings).encode()
                for name, data in files.items():
                    (self.folder / name).write_bytes(data)
                if alteration == "valid":
                    kvmpack.validate_scene(files)
                    if self.has_compiler:
                        subprocess.run([self.native, "eval", self.folder], input=b"", check=True)
                else:
                    with self.assertRaises(ValueError):
                        kvmpack.validate_scene(files)
                    if self.has_compiler:
                        subprocess.run([self.native, "reject", self.folder], check=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
