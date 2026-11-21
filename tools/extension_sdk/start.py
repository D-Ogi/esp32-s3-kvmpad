# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Open offline Extension Studio, or forward a command to its packaged CLI."""
import json
from pathlib import Path
import runpy
import sys
import sysconfig


def python_target():
    return {
        "implementation": sys.implementation.name,
        "version": f"{sys.version_info.major}.{sys.version_info.minor}",
        "platform": sysconfig.get_platform(),
        "free_threaded": bool(sysconfig.get_config_var("Py_GIL_DISABLED")),
    }


def main():
    directory = Path(__file__).resolve().parent
    target = json.loads((directory / "runtime.json").read_text(encoding="utf-8"))["target"]
    if target != python_target():
        suffix = " (free-threaded)" if target["free_threaded"] else ""
        sys.exit(f"This SDK requires {target['implementation']} {target['version']}{suffix} on {target['platform']}.")
    studio = directory / "tools/kvmpack"
    sys.path[:0] = [str(studio), str(directory / "lib")]
    if len(sys.argv) == 1:
        sys.argv.append("studio")
    runpy.run_path(str(studio / "kvmpack.py"), run_name="__main__")


if __name__ == "__main__":
    main()
