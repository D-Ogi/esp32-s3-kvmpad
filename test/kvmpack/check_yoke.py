# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check the desktop/device yoke contracts and run matching offline previews."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/kvmpack"))
import kvmpack


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path, help="native test_yoke_input from the LVGL simulator")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--node", default="node")
    args = parser.parse_args()
    exe = args.exe.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    folder = out / "source"
    folder.mkdir(exist_ok=True)
    base = kvmpack.read_members(ROOT / "tools/kvmpack/examples/flight-yoke")

    def variant(what, change):
        data = json.loads(base[what])
        change(data)
        return {**base, what: json.dumps(data).encode()}

    def write(files):
        for name, data in files.items():
            (folder / name).write_bytes(data)

    def calibration(ui):
        pad = ui["root"]["children"][0]["props"]
        del pad["spring_center"]  # Exercise the default on both consumers.
        pad.update(dead_zone_percent=20, curve="cubic", invert_y=True)

    renamed = variant("manifest.json", lambda d: d.update(id="org.example.camera.control", title="Camera controls"))
    ui, bindings = (json.loads(base[n]) for n in ("ui.json", "bindings.json"))
    button = next(w for w in ui["root"]["children"] if w["id"] == "hat_n")
    button.update(id="look_forward", events={"press":"look_ahead", "release":"stop_looking"})
    for a in bindings["actions"]:
        if a["trigger"].startswith("hat_n."):
            event = a["trigger"].split(".")[1]
            a.update(id=button["events"][event], trigger=f"look_forward.{event}")
    renamed.update({"ui.json":json.dumps(ui).encode(), "bindings.json":json.dumps(bindings).encode()})
    for name, files in (("linear", base), ("calibrated", variant("ui.json", calibration)), ("renamed", renamed)):
        kvmpack.validate_scene(files)
        write(files)
        command = [exe, folder]
        if name == "calibrated":
            command.append("--calibrated")
        subprocess.run(command, check=True)
        (out / (name + ".html")).write_text(kvmpack.preview_html(files), encoding="utf8")

    def action(doc, name):
        return next(a for a in doc["actions"] if a["id"] == name)

    def unpaired(ui):
        next(w for w in ui["root"]["children"] if w["id"] == "hat_n")["events"].pop("release")

    broken = variant("ui.json", unpaired)
    trimmed = json.loads(base["bindings.json"])
    trimmed["actions"].remove(action(trimmed, "hat_n_up"))
    broken["bindings.json"] = json.dumps(trimmed).encode()
    cases = [("unpaired hat", broken)]
    for name, value in (("hat_n_down", -1), ("hat_n_down", 8), ("hat_n_up", 7),
                        ("hat_n_down", True), ("hat_n_down", "1"), ("hat_n_down", 9)):
        cases.append((f"{name} {value!r}", variant("bindings.json",
                      lambda doc: action(doc, name)["args"].update(hat=value))))
    cases.append(("mixed binding", variant("bindings.json", lambda d: action(d, "hat_n_down")["args"].update(axes={}))))
    for key, value in (("dead_zone_percent", 100), ("dead_zone_percent", -1), ("dead_zone_percent", 1.5),
                       ("dead_zone_percent", False), ("curve", "unknown"), ("invert_y", 1)):
        cases.append((f"{key} {value!r}", variant("ui.json",
                      lambda d: d["root"]["children"][0]["props"].update({key:value}))))
    cases.append(("slider dead zone", variant("ui.json",
                  lambda d: d["root"]["children"][1]["props"].update(dead_zone_percent=10))))
    for name, files in cases:
        try:
            kvmpack.validate_scene(files)
        except ValueError:
            pass
        else:
            raise AssertionError(f"desktop accepted {name}")
        write(files)
        assert subprocess.run([exe, folder, "--validate"]).returncode == 1, f"device parser accepted {name}"
    subprocess.run([args.node, Path(__file__).with_name("yoke_preview.cjs"), out], check=True)
    print(f"PASS: both validators accepted 3 profiles, including renamed package, widget and actions, "
          f"and rejected {len(cases)} malformed profiles")


if __name__ == "__main__":
    main()
