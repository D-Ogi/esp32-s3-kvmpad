# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

"""Pin registry versions across the component manager's two Kconfig passes."""
from pathlib import Path
import sys

import yaml


if __name__ == "__main__":
    lock, destination = map(Path, sys.argv[1:])
    dependencies = yaml.safe_load(lock.read_text(encoding="utf-8"))["dependencies"]
    constraints = [f"{name}=={package['version']}\n" for name, package in dependencies.items()
                   if package["source"]["type"] == "service"]
    destination.write_text("".join(constraints), encoding="utf-8")
