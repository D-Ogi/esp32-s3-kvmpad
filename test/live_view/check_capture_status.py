# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Feed the production Windows broker's capture reply into the production board worker."""
import argparse
import asyncio
from contextlib import nullcontext
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import AsyncMock, Mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "hosts/windows"))
from broker import MAG, Services, Session
from media import CaptureError


async def host_reply(allow, stage=None, progress=False, pointer_label=None):
    messages = []

    class Socket:
        transport = Mock()

        async def send(self, message):
            messages.append(message)

    capture = Mock()
    capture.open = AsyncMock(return_value=r"\\.\DISPLAY1")
    if progress:
        async def enter_monitor(codec, mode, pointer_wait):
            if pointer_label:
                await pointer_wait(pointer_label)
            else:
                await pointer_wait()
            return pointer_label or r"\\.\DISPLAY1"
        capture.open.side_effect = enter_monitor
    approval = AsyncMock(return_value=allow)
    if stage == "approval_timeout":
        approval.side_effect = TimeoutError
    elif stage:
        capture.open.side_effect = CaptureError(stage, 0x800705B4)
    capture.wait_closed = asyncio.Event().wait
    capture.close = AsyncMock()
    session = Session(Socket(), MAG, Services(approval, lambda text: None, lambda: capture, {}))
    try:
        session.sequence = 1  # The control reader accepted START before dispatching it.
        await session.operate({"type": "START", "seq": 1, "mode": "PIXEL_1X", "formats": ["jpeg"],
                               "crop_v": 1, "source_crop": [512, 512], "present_v": 1, "max_fps": 10,
                               **({"progress_v": 1} if progress else {}),
                               **({"pointer_label_v": 1} if pointer_label else {})})
        if progress:
            assert [json.loads(m)["type"] for m in messages] == ["START_PROGRESS", "START_PROGRESS", "STARTED"]
            return messages
        assert len(messages) == 1
        return messages[0]
    finally:
        session.stop()
        await session.finish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--out", type=Path, help="Keep the fixture executable and platform headers here")
    args = parser.parse_args()
    started = asyncio.run(host_reply(True))
    refused = asyncio.run(host_reply(False))
    cursorwait = asyncio.run(host_reply(True, "cursorwait"))
    picked_window = asyncio.run(host_reply(True, "pickedwindow"))
    assert json.loads(picked_window) == {"type": "DENIED", "seq": 1, "reason": "pickedwindow"}
    assert len(started.encode()) > 125 and json.loads(started)["resource"] == r"\\.\DISPLAY1"
    assert json.loads(refused)["type"] == "DENIED"
    wrong_crop = json.loads(started)
    wrong_crop["source_crop"][0] = 511
    unpaced = json.loads(started)
    for field in ("present_v", "max_fps", "start_fps"):
        del unpaced[field]
    cases = [
        ("success", started, "stopped"),
        ("late_selection", started, "stopped"),
        ("borrow", started, "stopped"),
        ("borrow_bind", started, "bind_failed"),
        ("unpaced", json.dumps(unpaced), "stopped"),
        ("denied", refused, "host_denied"),
        ("cursorwait", cursorwait, "cursorwait"),
        ("picked_window", picked_window, "pickedwindow"),
        ("unknown_reason", '{"type":"DENIED","seq":1,"reason":"private text"}', "host_denied"),
        ("cursor_review", started, "cursor"),
        ("cursor_bind", started, "cursor"),
        ("cursor_control", started, "cursor"),
        ("cursor_data", started, "cursor"),
        ("cursor_freeze", started, "cursor"),
        ("source_timeout_data", started, "host_frame_timeout"),
        ("source_timeout_control", started, "host_frame_timeout"),
        ("mapping_review", started, "display_mapping"),
        ("mapping_before_review", started, "display_mapping"),
        ("mapping_bind", started, "display_mapping"),
        ("mapping_control", started, "display_mapping"),
        ("mapping_data", started, "display_mapping"),
        ("pointer_position_data", started, "pointer_position"),
        ("pointer_position_control", started, "pointer_position"),
        ("bad_schema", json.dumps(wrong_crop), "invalid_reply"),
        ("bad_json", "{", "invalid_reply"),
        ("control", started, "control_failed"),
        ("send", started, "request_failed"),
        ("timeout", started, "host_timeout"),
        ("lost", started, "control_lost"),
        ("allow_timeout", started, "allow_timeout"),
        ("resources", started, "resources"),
        ("decoder", started, "decoder_failed"),
        ("bind", started, "bind_failed"),
    ]
    picker, pointer, approved = asyncio.run(host_reply(True, progress=True))
    cases.extend([
        ("progress_ok", "\n".join((picker, pointer, approved)), "stopped"),
        ("progress_skip_pointer", "\n".join((picker, approved)), "stopped"),
        ("progress_repeat", "\n".join((picker, picker)), "invalid_reply"),
        ("progress_expiry", "\n".join((picker, pointer)), "host_timeout"),
        ("unexpected_progress", picker, "invalid_reply"),
    ])
    monitor = r"\\.\DISPLAY2"
    picker, named, selected = asyncio.run(host_reply(True, progress=True, pointer_label=monitor))
    assert json.loads(named)["resource"] == json.loads(selected)["resource"] == monitor

    def with_resource(message, label):
        return json.dumps({**json.loads(message), "resource": label})

    cases.extend([
        ("progress_label_match", "\n".join((picker, named, selected)), "stopped", monitor, monitor),
        ("progress_label_changed", "\n".join((picker, named, approved)), "invalid_reply", monitor),
        ("progress_label_empty", "\n".join((picker, with_resource(pointer, ""), approved)), "stopped"),
        ("progress_label_missing", "\n".join((picker, pointer, approved)), "stopped"),
        ("progress_label_picker", with_resource(picker, monitor), "invalid_reply"),
        ("progress_label_stale", "\n".join((picker, json.dumps({**json.loads(named), "seq": 0}))), "invalid_reply"),
        ("progress_label_unexpected_stop", "\n".join((picker, named, cursorwait)), "cursorwait", monitor),
        ("progress_legacy", "\n".join((picker, named, approved)), "stopped"),
        ("label_without_progress", approved, "stopped"),
    ])
    longest = r"\\.\DISPLAY99999"
    cases.append(("progress_label_long", "\n".join((picker, with_resource(pointer, longest),
                  with_resource(approved, longest))), "stopped", longest, longest))
    for bad in (None, 2, "DISPLAY2", monitor.lower(), monitor + "\0", monitor + "\n",
                r"\\.\DISPLAY", r"\\.\DISPLAY123456", r"\\.\DISPLAY２"):
        cases.append(("progress_label_invalid", "\n".join((picker, with_resource(pointer, bad))), "invalid_reply"))
    denials = {"approval_timeout": "approval_timeout", "selection_timeout": "selection_timeout", "cancel": "cancel",
               "cursor": "cursor",
               "geometry": "display_mapping", "topology": "display_mapping", "cursorpos": "pointer_position"}
    for stage, expected in denials.items():
        denial = asyncio.run(host_reply(True, stage))
        assert json.loads(denial) == {"type": "DENIED", "seq": 1, "reason": stage}
        cases.append((stage, denial, expected))
    components = ROOT / "components"
    with (nullcontext(args.out) if args.out else tempfile.TemporaryDirectory()) as tmp:
        out = Path(tmp)
        out.mkdir(exist_ok=True, parents=True)
        for name in ("esp_timer.h", "esp_transport_ws.h", "esp_transport_ssl.h", "esp_heap_caps.h",
                     "freertos/FreeRTOS.h", "freertos/task.h", "mbedtls/platform_util.h"):
            header = out / name
            header.parent.mkdir(exist_ok=True)
            header.touch()
        includes = [out, components / "kvmbridge"] + [components / d / "include" for d in
                    ("kvmbridge", "live_view", "pack_runtime", "remote_protocol", "secure_remote", "secure_remote/pairing")]
        exe = out / ("capture-status.exe" if os.name == "nt" else "capture-status")
        subprocess.run([args.cc, "-std=gnu11", "-O1", *["-I" + str(path) for path in includes],
                        Path(__file__).with_name("capture_status_fixture.c"), components / "kvmbridge/crop_wire.c",
                        components / "live_view/live_message.c", components / "remote_protocol/src/remote_json.c",
                        "-o", exe], check=True)
        for variant, reply, expected, *labels in cases:
            result = subprocess.run([exe, variant, expected, *labels], input=reply, text=True, stdout=subprocess.PIPE, check=True)
            assert result.stdout.strip() == expected
        print(f"{len(cases)} capture paths passed. STARTED {len(started.encode())} bytes, two PONGs, "
              "epoch-bound pointer hints, matching display review before image BIND, fixed capture reasons retained while telemetry remains connected.")


if __name__ == "__main__":
    main()
