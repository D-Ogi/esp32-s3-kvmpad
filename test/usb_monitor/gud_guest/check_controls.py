# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Check GUD layouts and real evdev registration on an isolated Linux USB gadget."""

import argparse
import base64
import hashlib
import subprocess
import tarfile
from pathlib import Path

from run import build


def descriptor_cases():
    device = bytes.fromhex("1201000200000040ffffffff000101020301")
    vendor = bytes.fromhex("0904000001ff00000007050102400000")
    keyboard = bytes.fromhex("09040100010301010009211101000122410007058203080001")
    mouse = bytes.fromhex("090402000103010200092111010001224d0007058303080001")
    single = device + bytes.fromhex("0902190001010080fa") + vendor
    dual = device + bytes.fromhex("09024b0003010080fa") + vendor + keyboard + mouse
    yield "display", single, 1, 0, True
    yield "required-missing", single, 1, 1, False
    yield "controls", dual, 3, 1, True
    yield "controls-optional", dual, 3, 0, True
    yield "interfaces-reversed", dual[:-50] + mouse + keyboard, 3, 1, False
    yield "extra-descriptor", dual + b"\x02\xff", 3, 1, False
    lab_device = bytes.fromhex("12011002ef02014009120100030101020301")
    lab_config = bytes.fromhex("0902530003010080fa080b0003ff000000")
    lab = lab_device + lab_config + vendor + keyboard + mouse
    yield "bench-iad", lab, 3, 1, True
    yield "bench-iad-optional", lab, 3, 0, True
    yield "bench-wrong-count", lab, 1, 0, False
    yield "bench-no-iad", lab_device + dual[18:], 3, 1, False
    yield "production-with-bench-iad", device + lab[18:], 3, 1, False
    for i in range(len(lab)):
        damaged = bytearray(lab)
        damaged[i] ^= 1
        yield f"bench-changed-{i}", damaged, 3, 1, False
    # Change every byte in the configuration length/count and every interface,
    # HID descriptor and endpoint. This includes a physical endpoint remap.
    for i in [17, 20, 21, 22, *range(27, len(dual))]:
        damaged = bytearray(dual)
        damaged[i] ^= 1
        yield f"changed-{i}", damaged, 3, 1, False


def put_bytes(path, data):
    return f"printf %s '{base64.b64encode(data).decode()}' | base64 -d > {path}\n"


def event_test():
    script = r'''
# All HID writes below go only to this guest's dummy_udc.0 gadget.
test "$(ls /sys/class/udc)" = dummy_udc.0
printf 'ffff ffff ff\n' > /sys/bus/usb/drivers/gud/new_id
for i in $(seq 1 80); do [ -c /dev/fb0 ] && break; sleep 0.1; done
test -c /dev/fb0
keys=$(basename "$D:1.1"/0003:*/input/input*/event*)
mouse=$(basename "$D:1.2"/0003:*/input/input*/event*)
for case in bad-key bad-motion good; do
    /fixture console-ack-input /dev/fb0 "/dev/input/$keys" "/dev/input/$mouse" 15 \
        < /tmp/probe-commands > "/tmp/events-$case" &
    observer=$!
    for i in $(seq 1 80); do grep -q '^GUD_LCD_READY tty1$' "/tmp/events-$case" && break; sleep 0.1; done
    grep -q '^GUD_INPUT_READY$' "/tmp/events-$case"
    if [ "$case" = bad-key ]; then
        # F14 before F13 must fail, and process exit must release both grabs.
        printf '\000\000\151\000\000\000\000\000' > /dev/hidg0
        if wait "$observer"; then exit 1; fi
        grep -qx 'GUD_REJECT input-sequence' /tmp/events-bad-key
        printf '\000\000\000\000\000\000\000\000' > /dev/hidg0
        sleep 0.1
        continue
    fi
'''
    for usage in range(0x68, 0x74):
        for key in (usage, 0):
            raw = bytes((0, 0, key, 0, 0, 0, 0, 0))
            script += put_bytes("/dev/hidg0", raw)
    script += r'''
    if [ "$case" = bad-motion ]; then
        # Starts in the correct direction but reverses before reaching +300.
        printf '\000\177\000\000\000' > /dev/hidg1
        printf '\000\201\000\000\000' > /dev/hidg1
        if wait "$observer"; then exit 1; fi
        grep -qx 'GUD_REJECT input-sequence' /tmp/events-bad-motion
        continue
    fi
'''
    for x, y in [(127, 0), (127, 0), (46, 0), (-127, 0), (-127, 0), (-46, 0),
                 (0, 127), (0, 73), (0, -127), (0, -73)]:
        script += put_bytes("/dev/hidg1", bytes((0, x & 255, y & 255, 0, 0)))
    script += r'''
    wait "$observer" || { cat /tmp/events-good; exit 1; }
    grep -qx GUD_INPUT_PASS /tmp/events-good
    grep -qx 'GUD_LCD_READY tty2' /tmp/events-good
done
echo 'PASS guest-only fixed HID sequence and rejection; grabs released'
'''
    return "printf 'ACK tty1\\nACK tty2\\n' > /tmp/probe-commands\n" + script


def checks(inputs, events):
    script = "set -eu\nVIRTUAL=1\n"
    for label, data, count, require, ok in descriptor_cases():
        script += put_bytes("/tmp/descriptors", data)
        script += (f"passed=0; /physical-init --check-layout /tmp/descriptors {count} 01 {require} && passed=1\n"
                   f'test "$passed" = {int(ok)} || {{ echo "FAIL layout {label}"; exit 1; }}\n')
    script += "echo 'PASS exact GUD USB descriptor cases'\n"

    # Only the dummy device needs usb_f_hid. The host HID core, generic HID,
    # usbhid and evdev are all built into the pinned kernel.
    with tarfile.open(inputs / "kernel.tar") as archive:
        name = next(m for m in archive.getnames() if m.endswith("/usb_f_hid.ko.zst"))
        compressed = archive.extractfile(name).read()
    if hashlib.sha256(compressed).hexdigest() != "69c89a9b237670f7a4ce4580587cc62910b94797e85e73e5b8461d25c7f790a8":
        raise ValueError("Unexpected usb_f_hid module from the prepared signed kernel")
    module = subprocess.check_output(["zstd", "-qdc"], input=compressed)
    # Keep encoded shell arguments small. No physical USB is exposed to QEMU.
    for start in range(0, len(module), 3072):
        script += put_bytes("/tmp/module.part", module[start:start + 3072])
        script += "cat /tmp/module.part >> /tmp/usb_f_hid.ko\n"
    script += """. /virtual-device
printf '\\n' > "$G/UDC"
insmod /tmp/usb_f_hid.ko
"""
    # Boot-class reports accepted by the standard Linux HID stack. The actual
    # board's interface metadata is checked above; configfs assigns its own strings/endpoints.
    reports = {
        "keyboard": "05010906a101050719e029e715002501950875018102950175088101050819012905950575019102"
                    "950175039101050719002aff00150026ff00950675088100c0",
        "mouse": "05010902a1010901a100050919012905150025019505750181029501750381010501093009311581"
                 "257f95027508810609381581257f950175088106050c0a38021581257f950175088106c0c0",
    }
    for protocol, (role, report) in enumerate(reports.items(), 1):
        script += f'f=$G/functions/hid.{role}\nmkdir "$f"\necho 1 > "$f/subclass"\necho {protocol} > "$f/protocol"\n'
        script += 'echo 8 > "$f/report_length"\necho 1 > "$f/no_out_endpoint"\n'
        script += put_bytes('"$f/report_desc"', bytes.fromhex(report))
        script += f'ln -s "$f" "$G/configs/c.1/{role}"\n'
    script += r'''
printf '%s\n' dummy_udc.0 > "$G/UDC"
D=/sys/bus/usb/devices/1-1
for i in $(seq 1 80); do [ -e "$D:1.2" ] && break; sleep 0.1; done
test "$(cat "$D/idVendor"):$(cat "$D/idProduct")" = ffff:ffff
test "$(cat "$D:1.0/bInterfaceClass")" = ff
test "$(cat "$D:1.1/bInterfaceProtocol")" = 01
test "$(cat "$D:1.2/bInterfaceProtocol")" = 02
/physical-init --check-input "$D:1.1" keyboard
/physical-init --check-input "$D:1.2" mouse
echo 'PASS virtual composite GUD + two Linux HID input devices'
'''
    return script + (event_test() if events else "") + "exit 0\n"


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("inputs", type=Path)
    cli.add_argument("work", type=Path)
    cli.add_argument("--clang", default="clang")
    cli.add_argument("--qemu", default="qemu-system-x86_64")
    cli.add_argument("--exercise-events", action="store_true",
                     help="generate fixed test reports only on the guest-local dummy gadget")
    args = cli.parse_args()
    inputs, work = args.inputs.resolve(), args.work.resolve()
    work.mkdir(parents=True)
    cmd = build(inputs, work, args.clang, args.qemu, checks(inputs, args.exercise_events), virtual=True)
    log = work / "guest.log"
    with log.open("wb") as output:
        result = subprocess.run(cmd, stdout=output, stderr=subprocess.STDOUT, timeout=90)
    text = log.read_text("utf-8", errors="replace")
    if args.exercise_events and "PASS guest-only fixed HID sequence and rejection" not in text:
        raise SystemExit(f"Input observer qualification failed. See {log}")
    for marker in ("PASS exact GUD USB descriptor cases", "GUD_HID keyboard", "GUD_HID mouse",
                   "PASS virtual composite GUD + two Linux HID input devices"):
        if result.returncode != 0 or marker not in text:
            raise SystemExit(f"Missing {marker}. See {log}")
    print(f"PASS {len(list(descriptor_cases()))} descriptor cases and two Linux HID registrations. {log}")


if __name__ == "__main__":
    main()
