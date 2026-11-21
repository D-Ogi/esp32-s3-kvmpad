# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Build the USB monitor worker and LVGL view against an existing sanitized simulator build."""
import argparse
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sim-build", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--ninja", default="ninja")
parser.add_argument("--picture-check", action="store_true", help="Exercise the engineering JPEG comparison with a decoder fixture")
args = parser.parse_args()
test = Path(__file__).resolve().parent
repo = test.parents[1]
component = repo / "components/usb_monitor"
build = args.sim_build.resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
commands = subprocess.check_output([args.ninja, "-C", build, "-t", "commands", "kvm_sim"], text=True).splitlines()
compile = [c for c in commands if " -c " in c]
base = next(c for c in compile if c.endswith("/kvm_sim.c")).split(" -MD ")[0]
compiler = base.split(" -", 1)[0].strip('"')
env["PATH"] = str(Path(compiler).parent) + os.pathsep + env["PATH"]
env["TEMP"] = env["TMP"] = str(out)
for name in ("esp_log.h","esp_heap_caps.h","freertos/FreeRTOS.h","freertos/task.h","freertos/idf_additions.h","freertos/semphr.h"):
    path = out / name
    path.parent.mkdir(exist_ok=True)
    path.write_text('#include "test_port.h"\n', encoding="ascii")
(out / "esp_timer.h").write_text(
    '#include <stdint.h>\n#include <time.h>\n'
    'static inline int64_t esp_timer_get_time(void) { struct timespec t; '
    'clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }\n',
    encoding="ascii")
extra = [out, test, component, component/"include", repo/"components/live_view/include", repo/"main", repo/"simulator/lvgl_sdl"]
base += "".join(' -I"'+p.as_posix()+'"' for p in extra)
if args.picture_check:
    base += " -DKVM_USB_PICTURE_CHECK=1"
sources = [(base, p) for p in [test/"view_test.c",component/"usb_monitor.c",component/"monitor_wire.c",component/"monitor_view.c"]]
for file in ("ui_theme.c", "kvm_control_icons.c", "ui_test_env.c", "sim_image.c"):
    cmd = next(c for c in compile if c.endswith("/" + file))
    flags, src = cmd.split(" -c ",1)
    sources.append((flags.split(" -MD ")[0], Path(src.strip('"'))))
def compile_object(flags, source):
    obj = out/(source.stem+".o")
    cmd = flags+' -o "'+obj.as_posix()+'" -c "'+source.as_posix()+'"'
    subprocess.run(cmd, cwd=build, env=env, check=True, shell=os.name != "nt")
    return obj


objects = [compile_object(flags, source) for flags, source in sources]
pipeline = out/("monitor-pipeline.exe" if os.name == "nt" else "monitor-pipeline")
subprocess.run([compiler, "-fsanitize=address,undefined", compile_object(base, test/"pipeline_test.c"),
                out/"usb_monitor.o", out/"monitor_wire.o", "-lpthread", "-o", pipeline], env=env, check=True)
subprocess.run([pipeline], cwd=out, env=env, check=True, timeout=20)
libs = re.search(r"build kvm_sim(?:\.exe)?: [^|]+?\| ([^|]+) \|\|", (build/"build.ninja").read_text())[1].split()
exe = out/("monitor-view.exe" if os.name == "nt" else "monitor-view")
subprocess.run([compiler,"-fsanitize=address,undefined",*objects,*[build/lib for lib in libs],"-lpthread","-o",exe],env=env,check=True)
subprocess.run([exe,out/"usb-monitor.png",out/"offline-frame.bin"],cwd=out,env=env,check=True,timeout=60)
