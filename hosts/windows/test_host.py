# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Synthetic loopback checks. These tests never start the Windows picker or a simulator."""
import asyncio
import ctypes
import datetime
import ipaddress
import os
import http.server
import json
from pathlib import Path
import shutil
import secrets
import random
import ssl
import subprocess
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib
from types import SimpleNamespace
from urllib.parse import parse_qs, urlsplit
from unittest.mock import AsyncMock, Mock, patch

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed, InvalidMessage

from adapters import OBS, MISSING_TOPIC, TOPICS, Telemetry, XPlane, obs_auth, subscriptions
from broker import (Broker, CAPTURE_SCOPE, CURSOR_WAIT, DISPLAY_PICKER, DISPLAY_REVIEW, GAUGES, IMAGE_OPEN, IMAGE_SENT, IMAGE_SHARING,
                    MAG, STUDIO, TELEMETRY_SCOPE, Services, Session, listen, receive)
from host import Foreground
import media
from paired_host import board_context, host_identity
from media import Capture, CaptureError, CINEPAK_BYTES, CROP_PREFIX, FORMATS, FrameRate, encode
from framing import Receiver
from studio import Assembly, Studio, frames


def identity(directory, name):
    private = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
            .public_key(private.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=1))
            .not_valid_after(now + datetime.timedelta(hours=1))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), critical=False)
            .sign(private, hashes.SHA256()))
    pem = directory / (name + ".pem")
    pem.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key = directory / (name + ".key")
    key.write_bytes(private.private_bytes(serialization.Encoding.PEM,
                    serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return pem, key


def tls_credentials(board_cert, host_cert, host_key):
    pin, ctx = board_context(ssl.PEM_cert_to_DER_cert(board_cert.read_text(encoding="ascii")))
    ctx.load_cert_chain(host_cert, host_key)
    return pin, ctx


class Synthetic:
    def __init__(self):
        self.calls = 0
        self.revoked = False
        self.exit = asyncio.Event()

    async def open(self, codec, mode, on_cursor_wait=None):
        self.codec = codec
        return "Display test"

    async def frame(self, stream, seq):
        assert not self.revoked
        self.calls += 1
        return encode(self.codec, stream, seq, 0, b"test payload")

    def revoke(self):
        self.revoked = True
        self.exit.set()

    async def wait_closed(self):
        await self.exit.wait()

    async def require_selected(self):
        if self.exit.is_set():
            raise CaptureError("cursor")

    async def pause(self):
        pass

    async def close(self):
        self.revoke()


class BrokerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        path = Path(tempfile.gettempdir()) / ("kvmpad-synthetic-" + secrets.token_hex(12))
        path.mkdir()
        self.addCleanup(path.rmdir)
        hp, hk = identity(path, "host")
        bp, bk = identity(path, "board")
        for item in (hp, hk, bp, bk):
            self.addCleanup(item.unlink)
        pin, context = tls_credentials(bp, hp, hk)
        self.client = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        self.client.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
        self.client.load_verify_locations(cafile=hp)
        self.client.load_cert_chain(bp, bk)
        self.bare = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        self.bare.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
        self.bare.load_verify_locations(cafile=hp)
        self.fake = Synthetic()
        self.permit = True
        self.requested = []
        self.gate = asyncio.Event()
        self.gate.set()

        async def approve(profile, message):
            self.requested.append(profile)
            await self.gate.wait()
            return self.permit

        self.host = Broker(pin, Services(approve, lambda _: None, lambda: self.fake, {}))
        self.server = await listen(self.host, "127.0.0.1", 0, context)
        self.base = "wss://127.0.0.1:" + str(self.server.sockets[0].getsockname()[1])

    async def asyncTearDown(self):
        self.host.stop()
        self.server.close()
        await self.server.wait_closed()

    async def socket(self, path, context=None):
        return await connect(self.base + path, ssl=context or self.client, server_hostname="localhost",
                             proxy=None, open_timeout=2, close_timeout=.3)

    async def hello(self, profile=MAG, scope=None, **features):
        ws = await self.socket("/control")
        hello = dict(type="HELLO", protocol="kvmbridge", version=1, profile=profile, **features)
        if scope is not None:
            hello["scope"] = scope
        await ws.send(json.dumps(hello))
        reply = json.loads(await ws.recv())
        self.assertEqual(reply["type"], "SESSION")
        self.assertEqual(reply.get("scope"),scope)
        self.assertEqual(reply.get("composite_v"), features.get("composite_v"))
        return ws, reply["session"]

    async def start(self, ws):
        await ws.send(json.dumps(dict(type="START", seq=1, mode="PIXEL_1X", formats=["jpeg"])))

    async def test_real_tls_framing_stop_and_token_replay(self):
        ctrl, token = await self.hello()
        await self.start(ctrl)
        reply = json.loads(await ctrl.recv())
        self.assertEqual(reply["type"], "STARTED")
        self.assertEqual(self.fake.calls, 0)
        image = await self.socket("/image")
        await image.send(json.dumps(dict(type="BIND", session=token)))
        self.assertEqual(bytes(Receiver("jpeg", reply["stream"]).receive(await image.recv())), b"test payload")
        await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
        await asyncio.wait_for(image.wait_closed(), 2)
        self.assertTrue(self.fake.revoked)
        await ctrl.wait_closed()
        await asyncio.sleep(.03)
        other, new_token = await self.hello()
        self.assertNotEqual(new_token, token)
        stale = await self.socket("/image")
        await stale.send(json.dumps(dict(type="BIND", session=token)))
        with self.assertRaises(ConnectionClosed):
            await stale.recv()
        self.assertEqual(self.requested, [MAG])
        await other.close()

    async def test_presentation_negotiation_fails_before_consent(self):
        crop = dict(crop_v=1, source_crop=[512, 512])
        for extra in (dict(present_v=1, max_fps=5), dict(**crop, present_v=True, max_fps=5),
                      dict(**crop, max_fps=5), dict(**crop, present_v=1),
                      *(dict(**crop, present_v=1, max_fps=value) for value in (0, 11, True, 5.0))):
            with self.subTest(extra=extra):
                ctrl, _ = await self.hello()
                await ctrl.send(json.dumps(dict(type="START", seq=1, mode="PIXEL_1X", formats=["jpeg"], **extra)))
                self.assertEqual(json.loads(await ctrl.recv())["type"], "DENIED")
                await ctrl.wait_closed()
                await asyncio.sleep(.01)
        self.assertEqual(self.requested, [])

    async def test_revoked_capture_can_reopen_while_its_old_picker_finishes_cleanup(self):
        close_started, allow_exit, old_closed = asyncio.Event(), asyncio.Event(), asyncio.Event()
        old_capture = self.fake
        async def slow_close():
            close_started.set()
            await allow_exit.wait()
            old_closed.set()
        old_capture.close = slow_close
        ctrl, old_token = await self.hello()
        await self.start(ctrl)
        self.assertEqual(json.loads(await ctrl.recv())["type"], "STARTED")
        image = await self.socket("/image")
        await image.send(json.dumps(dict(type="BIND", session=old_token)))
        await image.recv()
        try:
            await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
            await asyncio.wait_for(asyncio.gather(ctrl.wait_closed(), image.wait_closed(), close_started.wait()), 2)
            self.assertTrue(old_capture.revoked)
            self.fake = Synthetic()
            fresh_ctrl, fresh_token = await self.hello()
            self.assertNotEqual(fresh_token, old_token)
            allow_exit.set()
            await asyncio.wait_for(old_closed.wait(), 1)
            await self.start(fresh_ctrl)
            self.assertEqual(json.loads(await fresh_ctrl.recv())["type"], "STARTED")
            stale = await self.socket("/image")
            await stale.send(json.dumps(dict(type="BIND", session=old_token)))
            with self.assertRaises(ConnectionClosed):
                await stale.recv()
            fresh_image = await self.socket("/image")
            await fresh_image.send(json.dumps(dict(type="BIND", session=fresh_token)))
            self.assertIsInstance(await fresh_image.recv(), bytes)
            self.assertEqual(self.requested, [MAG, MAG])
            await fresh_ctrl.send(json.dumps(dict(type="STOP", seq=2)))
            await asyncio.wait_for(asyncio.gather(fresh_ctrl.wait_closed(), fresh_image.wait_closed()), 2)
        finally:
            allow_exit.set()

    async def test_windows_display_selection_and_first_presented_frame_have_separate_statuses(self):
        statuses = []
        self.host.services.status = statuses.append
        ctrl, token = await self.hello()
        async def frame(stream, seq):
            return await self.fake.frame(stream, seq), "00" * 48
        self.fake.frame_with_crop = frame
        await ctrl.send(json.dumps(dict(type="START", seq=1, formats=["jpeg"], mode="PIXEL_1X",
                                       source_crop=[512, 512], crop_v=1, present_v=1, max_fps=10)))
        stream = json.loads(await ctrl.recv())["stream"]
        self.assertIn(DISPLAY_PICKER, statuses)
        self.assertNotIn(CURSOR_WAIT, statuses)
        self.assertEqual(statuses[-1], DISPLAY_REVIEW)
        self.assertEqual(self.fake.calls, 0)
        data = await self.socket("/image")
        await data.send(json.dumps(dict(type="BIND", session=token)))
        for seq, shown in enumerate((False, True)):
            self.assertEqual(json.loads(await data.recv())["type"], "CROP1")
            self.assertIsInstance(await data.recv(), bytes)
            self.assertIn(IMAGE_OPEN, statuses)
            self.assertIn(IMAGE_SENT, statuses)
            self.assertNotIn(IMAGE_SHARING, statuses)
            await data.send(json.dumps(dict(type="FRAME_DONE", stream=stream, frame=seq, shown=shown)))
            expected = IMAGE_SHARING if shown else "KVMpad received an image but could not display it."
            async with asyncio.timeout(1):
                while expected not in statuses:
                    await asyncio.sleep(.005)
        self.host.stop()
        await asyncio.gather(ctrl.wait_closed(), data.wait_closed())

    async def test_picker_failure_reports_its_stage_without_copying_external_error_text(self):
        statuses = []
        self.host.services.status = statuses.append
        self.fake.open = AsyncMock(side_effect=ValueError("sensitive selection name"))
        ctrl, _ = await self.hello()
        await self.start(ctrl)
        self.assertEqual(json.loads(await ctrl.recv()), {"type": "DENIED", "seq": 1})
        await ctrl.wait_closed()
        self.assertIn("Windows could not use the selected display", statuses[-1])
        self.assertNotIn("sensitive", "\n".join(statuses))
        self.assertNotIn(IMAGE_SHARING, statuses)
        self.assertTrue(self.fake.revoked)

    async def test_native_capture_reason_is_preserved_before_and_after_the_board_opens_the_channel(self):
        for stage in ("select", "frame", "closed"):
            with self.subTest(stage=stage):
                statuses = []
                self.host.services.status = statuses.append
                self.fake = Synthetic()
                error = CaptureError(stage, 0x80004002)
                if stage == "select":
                    self.fake.open = AsyncMock(side_effect=error)
                elif stage == "frame":
                    self.fake.frame = AsyncMock(side_effect=error)
                else:
                    async def fail_closed():
                        await self.fake.exit.wait()
                        raise error
                    self.fake.wait_closed = fail_closed
                ctrl, token = await self.hello()
                await self.start(ctrl)
                self.assertEqual(json.loads(await ctrl.recv())["type"], "DENIED" if stage == "select" else "STARTED")
                if stage != "select":
                    data = await self.socket("/image")
                    await data.send(json.dumps(dict(type="BIND", session=token)))
                    if stage == "closed":
                        await data.recv()
                        self.fake.exit.set()
                    await asyncio.wait_for(data.wait_closed(), 1)
                await asyncio.wait_for(ctrl.wait_closed(), 1)
                self.assertEqual(statuses[-1], str(error))
                async with asyncio.timeout(1):
                    while self.host.session:
                        await asyncio.sleep(.005)

    async def test_capture_errors_reach_the_board_before_review_during_review_and_while_streaming(self):
        for phase, stage, code in (("entry","cursorwait",None), ("entry","cursor",None), ("entry","pickedwindow",None),
                                   ("review","cursor",4001), ("frame","cursor",4001),
                                   ("frame","frame_timeout",4002),
                                   ("entry","topology",None), ("entry","geometry",None), ("entry","cursorpos",None),
                                   ("review","topology",4003), ("review","cursorpos",4004),
                                   ("frame","topology",4003), ("frame","geometry",4003), ("frame","cursorpos",4004)):
            with self.subTest(phase=phase, stage=stage):
                self.fake = Synthetic()
                statuses = []
                self.host.services.status = statuses.append
                issue = CaptureError(stage, 0x800704C7)
                if phase == "entry":
                    self.fake.open = AsyncMock(side_effect=issue)
                elif phase == "review":
                    async def fail_after_selection():
                        await self.fake.exit.wait()
                        raise issue
                    self.fake.wait_closed = fail_after_selection
                else:
                    self.fake.frame = AsyncMock(side_effect=issue)
                ctrl, token = await self.hello()
                await self.start(ctrl)
                reply = json.loads(await ctrl.recv())
                if phase == "entry":
                    self.assertEqual(reply, {"type": "DENIED", "seq": 1, "reason": stage})
                else:
                    self.assertEqual(reply["type"], "STARTED")
                    if phase == "review":
                        self.fake.exit.set()
                    else:
                        data = await self.socket("/image")
                        await data.send(json.dumps(dict(type="BIND", session=token)))
                        await asyncio.wait_for(data.wait_closed(), 2)
                        self.assertEqual((data.close_code, data.close_reason), (code, ""))
                await asyncio.wait_for(ctrl.wait_closed(), 2)
                if phase != "entry":
                    self.assertEqual((ctrl.close_code, ctrl.close_reason), (code, ""))
                self.assertTrue(self.fake.revoked)
                self.assertEqual(statuses[-1], str(issue))

    async def test_board_does_not_bind_after_display_selection(self):
        statuses = []
        self.host.services.status = statuses.append
        ctrl, _ = await self.hello()
        with patch("broker.BIND_TIMEOUT", .05):
            await self.start(ctrl)
            self.assertEqual(json.loads(await ctrl.recv())["type"], "STARTED")
            await asyncio.wait_for(ctrl.wait_closed(), 1)
        self.assertEqual(statuses[-1], "KVMpad did not open the image channel after display selection.")
        self.assertEqual(self.fake.calls, 0)

    async def test_picked_second_display_has_its_own_pointer_entry_deadline(self):
        await self.second_display_entry(named_helper=False, named_board=False)

    async def test_system_picker_is_the_pc_decision_for_cursor_magnifier(self):
        self.host.services.system_picker = True
        entered, selected = asyncio.Event(), asyncio.Event()

        async def choose(codec, mode, on_cursor_wait):
            self.fake.codec = codec
            entered.set()
            await selected.wait()
            return r"\\.\DISPLAY2"

        self.fake.open = choose
        self.host.services.approve = AsyncMock(side_effect=AssertionError("Unexpected second PC prompt"))
        ctrl, token = await self.hello()
        await ctrl.send(json.dumps(dict(type="START", seq=1, mode="PIXEL_1X", formats=["jpeg"], progress_v=1)))
        self.assertEqual(json.loads(await ctrl.recv()), dict(type="START_PROGRESS", seq=1, stage="picker"))
        await asyncio.wait_for(entered.wait(), 2)
        self.assertEqual(self.fake.calls, 0)
        selected.set()
        reply = json.loads(await ctrl.recv())
        self.assertEqual((reply["type"], reply["resource"]), ("STARTED", r"\\.\DISPLAY2"))
        self.assertEqual(self.fake.calls, 0)
        image = await self.socket("/image")
        await image.send(json.dumps(dict(type="BIND", session=token)))
        self.assertTrue((await asyncio.wait_for(image.recv(), 2)).endswith(b"test payload"))
        self.host.services.approve.assert_not_awaited()

    async def test_canceling_system_picker_sends_denied_without_opening_image(self):
        self.host.services.system_picker = True
        self.fake.open = AsyncMock(side_effect=CaptureError("cancel"))
        ctrl, _ = await self.hello()
        await self.start(ctrl)
        self.assertEqual(json.loads(await ctrl.recv()), dict(type="DENIED", seq=1, reason="cancel"))
        self.assertEqual(self.fake.calls, 0)
        self.assertEqual(self.requested, [])

    async def test_pointer_hint_does_not_change_legacy_progress_fields(self):
        await self.second_display_entry(named_helper=True, named_board=False)

    async def test_selected_display_hint_precedes_entry_and_local_consent(self):
        await self.second_display_entry(named_helper=True, named_board=True)

    async def test_named_board_accepts_a_legacy_pointer_hint(self):
        await self.second_display_entry(named_helper=False, named_board=True)

    async def test_pointer_label_requires_supported_picker_progress(self):
        for versions in ({"pointer_label_v": 1}, {"progress_v": 1, "pointer_label_v": True},
                         {"progress_v": 1, "pointer_label_v": 2}):
            ctrl, _ = await self.hello()
            await ctrl.send(json.dumps(dict(type="START", seq=1, mode="PIXEL_1X", formats=["jpeg"], **versions)))
            self.assertEqual(json.loads(await ctrl.recv())["type"], "DENIED")
            await ctrl.wait_closed()
        self.assertEqual(self.fake.calls, 0)

    async def second_display_entry(self, named_helper, named_board):
        from media import child

        statuses = []
        self.host.services.status = statuses.append
        capture = Capture(None, None)
        self.host.services.capture = lambda: capture
        program = r'''
import os, struct, sys
assert os.environ['KVMPAD_PICKER_PROGRESS'] == '1'
assert os.environ['KVMPAD_PICKER_LABEL'] == '1'
inp, out = sys.stdin.buffer, sys.stdout.buffer
out.write(b'KCP2 CURSOR ' + rb'\\.\DISPLAY2' + b'\n' if sys.argv[1] == 'named' else b'KCP1 CURSOR\n')
assert inp.read(1) == b'+'  # Synthetic pointer entry on the already picked display.
out.write(b'\\\\.\\DISPLAY2\n')
assert inp.read(1) == b'F'
out.write(struct.pack('<I', 5) + b'frame')
assert inp.read(1) == b'F'
sys.stderr.buffer.write(b'KCE1 cursor 800704C7\n')
sys.exit(3)
'''
        def launch(*args, diagnostics=False):
            return child(sys.executable, '-u', '-c', program, 'named' if named_helper else 'legacy',
                         diagnostics=diagnostics)

        ctrl, token = await self.hello()
        with patch('media.child', launch), patch('media.PICKER_TIMEOUT', .6), patch('media.CURSOR_ENTRY_TIMEOUT', 3):
            extra = {"pointer_label_v": 1} if named_board else {}
            await ctrl.send(json.dumps(dict(type="START", seq=1, mode="PIXEL_1X", formats=["jpeg"], progress_v=1, **extra)))
            for stage in ("picker", "pointer"):
                resource = {"resource": r"\\.\DISPLAY2"} if stage == "pointer" and named_board and named_helper else {}
                self.assertEqual(json.loads(await asyncio.wait_for(ctrl.recv(), 2)),
                                 {"type": "START_PROGRESS", "seq": 1, "stage": stage, **resource})
            async with asyncio.timeout(2):
                while capture.process is None:
                    await asyncio.sleep(.01)
            with self.assertRaises(TimeoutError):
                await asyncio.wait_for(ctrl.recv(), .8)
            self.assertFalse(capture.selected or capture.revoked)
            expected = "Move the pointer onto DISPLAY2 and keep it there." if named_helper else CURSOR_WAIT
            self.assertEqual(statuses[-2:], [DISPLAY_PICKER, expected])
            capture.process.stdin.write(b'+')
            capture.process.stdin.flush()
            reply = json.loads(await asyncio.wait_for(ctrl.recv(), 2))
            self.assertEqual((reply['type'], reply['resource']), ('STARTED', r'\\.\DISPLAY2'))
            self.assertEqual(statuses[-1], DISPLAY_REVIEW)
            data = await self.socket('/image')
            await data.send(json.dumps(dict(type='BIND', session=token)))
            self.assertTrue((await asyncio.wait_for(data.recv(), 2)).endswith(b'frame'))
            await asyncio.wait_for(asyncio.gather(ctrl.wait_closed(), data.wait_closed()), 2)
            self.assertEqual((ctrl.close_code, data.close_code), (4001, 4001))
            self.assertTrue(capture.revoked)

    async def test_freeze_receives_cursor_exit_before_picker_watcher_runs(self):
        await self.cursor_exit_before_control("FREEZE")

    async def test_resume_requires_new_consent_after_pointer_exit_while_frozen(self):
        old_token = await self.cursor_exit_before_control("RESUME")
        self.host.services.capture = lambda: self.fake
        self.fake.open = AsyncMock(wraps=self.fake.open)
        self.gate.clear()
        ctrl, fresh_token = await self.hello()
        self.assertNotEqual(fresh_token, old_token)
        await self.start(ctrl)
        async with asyncio.timeout(2):
            while len(self.requested) < 2:
                await asyncio.sleep(.005)
        self.assertEqual(self.requested, [MAG, MAG])
        self.fake.open.assert_not_awaited()
        self.gate.set()
        self.assertEqual(json.loads(await ctrl.recv())["resource"], "Display test")
        self.fake.open.assert_awaited_once()
        stale = await self.socket("/image")
        await stale.send(json.dumps(dict(type="BIND", session=old_token)))
        with self.assertRaises(ConnectionClosed):
            await stale.recv()
        self.assertEqual(self.fake.calls, 0)
        image = await self.socket("/image")
        # BIND is sent by the board only after its local Allow display.
        await image.send(json.dumps(dict(type="BIND", session=fresh_token)))
        self.assertIsInstance(await image.recv(), bytes)
        await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
        await asyncio.wait_for(asyncio.gather(ctrl.wait_closed(), image.wait_closed()), 2)

    async def cursor_exit_before_control(self, command):
        from media import child

        capture = Capture(None, None)
        self.host.services.capture = lambda: capture
        capture.wait_closed = asyncio.Event().wait
        statuses = []
        self.host.services.status = statuses.append
        program = r'''
import struct, sys
inp, out = sys.stdin.buffer, sys.stdout.buffer
out.write(rb'\\.\DISPLAY2' + b'\n')
assert inp.read(1) == b'M'
out.write(struct.pack('<4siiiiHHHHB3x', b'KCM1', 256, 256, 0, 0, 512, 512, 0, 0, 0))
out.write(struct.pack('<I', 5) + b'frame')
if sys.argv[1] == 'RESUME':
    assert inp.read(1) == b'P'
assert inp.read(1) == b'+'
sys.stderr.buffer.write(b'KCE1 cursor 800704C7\n')
sys.exit(3)
'''
        def launch(*args, diagnostics=False):
            return child(sys.executable, '-u', '-c', program, command, diagnostics=diagnostics)

        ctrl, token = await self.hello()
        with patch('media.child', launch):
            await ctrl.send(json.dumps(dict(type='START', seq=1, mode='PIXEL_1X', formats=['jpeg'],
                                            crop_v=1, source_crop=[512, 512], present_v=1, max_fps=5)))
            self.assertEqual(json.loads(await ctrl.recv())['resource'], r'\\.\DISPLAY2')
        data = await self.socket('/image')
        await data.send(json.dumps(dict(type='BIND', session=token)))
        self.assertEqual(json.loads(await data.recv())['type'], 'CROP1')
        self.assertTrue((await data.recv()).endswith(b'frame'))
        if command == "RESUME":
            await ctrl.send(json.dumps(dict(type="FREEZE", seq=2)))
            self.assertEqual(json.loads(await ctrl.recv())["type"], "FROZEN")
        # The board holds the next frame credit and the picker watcher is still waiting.
        capture.process.stdin.write(b'+')
        capture.process.stdin.flush()
        await asyncio.to_thread(capture.process.wait, 2)
        await asyncio.sleep(.11)
        await ctrl.send(json.dumps(dict(type=command, seq=3 if command == "RESUME" else 2)))
        for ws in (ctrl, data):
            try:
                reply = await asyncio.wait_for(ws.recv(), 2)
            except ConnectionClosed:
                pass
            else:
                self.fail(f"Expired capture replied to {command}: {reply!r}")
            self.assertEqual((ws.close_code, ws.close_reason), (4001, ''))
        self.assertIn('pointer left the selected display', statuses[-1])
        self.assertTrue(capture.revoked)
        return token

    async def test_board_stop_during_pointer_entry_explains_the_stage(self):
        waiting = asyncio.Event()
        statuses = []
        self.host.services.status = statuses.append

        async def wait_for_entry(codec, mode, on_cursor_wait):
            await on_cursor_wait()
            waiting.set()
            await self.fake.exit.wait()

        self.fake.open = wait_for_entry
        ctrl, _ = await self.hello()
        await self.start(ctrl)
        await asyncio.wait_for(waiting.wait(), 2)
        await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
        await asyncio.wait_for(ctrl.wait_closed(), 2)
        self.assertTrue(self.fake.revoked)
        self.assertIn("waiting for the pointer", statuses[-1])
        self.assertNotIn(DISPLAY_REVIEW, statuses)
        self.assertEqual(self.fake.calls, 0)

    async def test_board_stop_between_selection_and_bind_keeps_the_stage_visible(self):
        statuses = []
        self.host.services.status = statuses.append
        ctrl, _ = await self.hello()
        await self.start(ctrl)
        self.assertEqual(json.loads(await ctrl.recv())["type"], "STARTED")
        await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
        await asyncio.wait_for(ctrl.wait_closed(), 1)
        self.assertEqual(statuses[-1], "KVMpad ended the request before opening the image channel.")
        self.assertEqual(self.fake.calls, 0)

    async def test_board_silence_after_first_image_reports_missing_ack(self):
        statuses = []
        self.host.services.status = statuses.append
        ctrl, token = await self.hello()
        self.fake.frame_with_crop = AsyncMock(return_value=(b"image", "00" * 48))
        await ctrl.send(json.dumps(dict(type="START", seq=1, formats=["jpeg"], mode="PIXEL_1X",
                                       source_crop=[512, 512], crop_v=1, present_v=1, max_fps=10)))
        self.assertEqual(json.loads(await ctrl.recv())["type"], "STARTED")
        data = await self.socket("/image")
        with patch("broker.FRAME_ACK_TIMEOUT", .1):
            await data.send(json.dumps(dict(type="BIND", session=token)))
            self.assertEqual(json.loads(await data.recv())["type"], "CROP1")
            self.assertEqual(await data.recv(), b"image")
            await asyncio.wait_for(data.wait_closed(), 1)
        self.assertEqual(statuses[-1], "KVMpad did not acknowledge the last image in time.")
        self.assertNotIn(IMAGE_SHARING, statuses)
        await ctrl.wait_closed()

    async def test_credited_images_outlive_the_first_control_request_deadline(self):
        # Only the initial 100-second wait is shortened. The real control and image WebSockets,
        # foreground approval, frame pacing and presentation acknowledgements run normally.
        async def short_initial_wait(ws, idle):
            return await receive(ws, .15 if idle == 100 else idle)

        async def frame(stream, seq):
            return await self.fake.frame(stream, seq), "00" * 48
        self.fake.frame_with_crop = frame
        with patch("broker.receive", short_initial_wait), patch("broker.CAPTURE_START_TIMEOUT", .25):
            ctrl, token = await self.hello()
            await ctrl.send(json.dumps(dict(type="START", seq=1, formats=["jpeg"], mode="PIXEL_1X",
                                           source_crop=[512, 512], crop_v=1, present_v=1, max_fps=5)))
            stream = json.loads(await ctrl.recv())["stream"]
            data = await self.socket("/image")
            await data.send(json.dumps(dict(type="BIND", session=token)))
            for sequence in range(5):
                self.assertEqual(json.loads(await asyncio.wait_for(data.recv(), 2))["type"], "CROP1")
                self.assertIsInstance(await data.recv(), bytes)
                await data.send(json.dumps(dict(type="FRAME_DONE", stream=stream, frame=sequence, shown=True)))
            self.assertFalse(self.fake.revoked)
            await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
            await asyncio.wait_for(asyncio.gather(ctrl.wait_closed(), data.wait_closed()), 1)
            self.assertTrue(self.fake.revoked)

    async def test_start_budget_covers_all_three_steps_and_an_unanswered_approval_is_identified(self):
        from broker import APPROVAL_TIMEOUT
        from media import CURSOR_ENTRY_TIMEOUT, PICKER_TIMEOUT

        self.gate.clear()
        self.fake.open = AsyncMock()
        ctrl, _ = await self.hello()
        loop = asyncio.get_running_loop()
        started = loop.time()
        with patch("broker.APPROVAL_TIMEOUT", .4):
            await self.start(ctrl)
            async with asyncio.timeout(2):
                while not self.requested:
                    await asyncio.sleep(.005)
            deadline = self.host.sessions[CAPTURE_SCOPE].start_timeout.when()
            self.assertGreater(deadline - started, APPROVAL_TIMEOUT + PICKER_TIMEOUT + CURSOR_ENTRY_TIMEOUT)
            self.assertEqual(json.loads(await ctrl.recv()), {"type": "DENIED", "seq": 1, "reason": "approval_timeout"})
            await asyncio.wait_for(ctrl.wait_closed(), 2)
        self.fake.open.assert_not_awaited()

    async def test_pending_approval_still_expires_and_releases_the_control_connection(self):
        self.gate.clear()
        statuses = []
        self.host.services.status = statuses.append
        with patch("broker.CAPTURE_START_TIMEOUT", .08):
            ctrl, _ = await self.hello()
            await self.start(ctrl)
            await asyncio.wait_for(ctrl.wait_closed(), 1)
        self.assertEqual(statuses[-1], "The HOST request timed out before the data channel was ready.")
        self.assertEqual(self.fake.calls, 0)
        self.assertIsNone(self.host.session)

    async def test_presentation_rejects_invalid_or_duplicate_credit(self):
        for change in ({"stream": 0}, {"frame": -1}, {"frame": 1}, {"frame": False},
                       {"shown": 1}, {"extra": 1}, {"type": "OTHER"}, {}):
            with self.subTest(change=change):
                self.fake = Synthetic()
                async def metadata(stream, seq):
                    return await self.fake.frame(stream, seq), "00" * 48
                self.fake.frame_with_crop = metadata
                ctrl, token = await self.hello()
                await ctrl.send(json.dumps(dict(type="START", seq=1, mode="PIXEL_1X", formats=["jpeg"],
                                                crop_v=1, source_crop=[512, 512], present_v=1, max_fps=10)))
                stream = json.loads(await ctrl.recv())["stream"]
                data = await self.socket("/image")
                await data.send(json.dumps(dict(type="BIND", session=token)))
                await data.recv()
                await data.recv()
                await asyncio.sleep(.23)
                self.assertEqual(self.fake.calls, 1)
                credit = dict(type="FRAME_DONE", stream=stream, frame=0, shown=True)
                if not change:
                    await data.send(json.dumps(credit))
                await data.send(json.dumps({**credit, **change}))
                await asyncio.wait_for(ctrl.wait_closed(), 1)
                await asyncio.wait_for(data.wait_closed(), 1)
                self.assertTrue(self.fake.revoked)
                await asyncio.sleep(.01)

    async def test_provider_rejects_read_contract_and_telemetry_stays_open(self):
        class Rejected:
            async def samples(self, rates, deliver):
                raise ValueError("Provider rejected requested fields")
        self.host.services.providers["sim.xplane"] = Rejected()
        ctrl, token = await self.hello(GAUGES)
        await ctrl.send(json.dumps(dict(type="SUBSCRIBE", seq=1, source="sim.xplane", quality_v=1,
                                        topics=[dict(name="sim.altitude_ft", rate_hz=10)])))
        self.assertEqual(json.loads(await ctrl.recv())["quality_v"], 1)
        data = await self.socket("/telemetry")
        await data.send(json.dumps(dict(type="BIND", session=token)))
        async with asyncio.timeout(1):
            while True:
                msg = json.loads(await data.recv())
                if msg["quality"] == "ERROR":
                    self.assertIsNone(msg["value"])
                    break
        await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
        await data.wait_closed()

    async def test_invalid_quality_negotiation_never_asks_for_consent(self):
        self.host.services.providers["sim.xplane"] = object()
        for version in (True, None, 0, 2, "1"):
            ctrl, _ = await self.hello(GAUGES)
            await ctrl.send(json.dumps(dict(type="SUBSCRIBE", seq=1, source="sim.xplane", quality_v=version,
                                            topics=[dict(name="sim.altitude_ft", rate_hz=2)])))
            self.assertEqual(json.loads(await ctrl.recv())["type"], "DENIED")
            await ctrl.wait_closed()
            await asyncio.sleep(.01)
        self.assertEqual(self.requested, [])

    async def test_denial_and_wrong_profile(self):
        self.permit = False
        ws, _ = await self.hello()
        await self.start(ws)
        self.assertEqual(json.loads(await ws.recv())["type"], "DENIED")
        await ws.wait_closed()
        await asyncio.sleep(.03)
        ws, _ = await self.hello(GAUGES)
        await self.start(ws)
        self.assertEqual(json.loads(await ws.recv())["type"], "DENIED")
        self.assertEqual(self.requested, [MAG])
        self.assertEqual(self.fake.calls, 0)

    async def test_preempt_pending_consent(self):
        self.gate.clear()
        ws, _ = await self.hello()
        await self.start(ws)
        await ws.send(json.dumps(dict(type="STOP", seq=2)))
        await asyncio.wait_for(ws.wait_closed(), 1)
        self.gate.set()
        self.assertEqual(self.fake.calls, 0)

    async def test_freeze_preserves_session_and_local_stop_while_frozen(self):
        statuses = []
        self.host.services.status = statuses.append
        ctrl, token = await self.hello()
        await self.start(ctrl)
        await ctrl.recv()
        data = await self.socket("/image")
        await data.send(json.dumps(dict(type="BIND", session=token)))
        await data.recv()
        await asyncio.sleep(.12)
        await ctrl.send(json.dumps(dict(type="FREEZE", seq=2)))
        self.assertEqual(json.loads(await ctrl.recv())["type"], "FROZEN")
        self.assertIn("frozen", statuses[-1].lower())
        count = self.fake.calls
        await asyncio.sleep(.3)
        self.assertEqual(self.fake.calls, count)
        resume = asyncio.Event()
        original = self.fake.frame
        async def next_frame(stream, sequence):
            await resume.wait()
            return await original(stream, sequence)
        self.fake.frame = next_frame
        await ctrl.send(json.dumps(dict(type="RESUME", seq=3)))
        self.assertEqual(json.loads(await ctrl.recv())["type"], "RESUMED")
        self.assertIn("resuming", statuses[-1].lower())
        resume.set()
        self.assertIsInstance(await data.recv(), bytes)
        await asyncio.sleep(.12)
        self.assertEqual(statuses[-1], IMAGE_SHARING)
        await ctrl.send(json.dumps(dict(type="FREEZE", seq=4)))
        self.assertEqual(json.loads(await ctrl.recv())["type"], "FROZEN")
        self.fake.revoke()  # The native picker Stop or OS-session loss exits its helper.
        await asyncio.wait_for(data.wait_closed(), 1)
        await asyncio.wait_for(ctrl.wait_closed(), 1)

    async def test_repeated_unshown_images_retire_the_confirmed_rate_until_a_new_presentation(self):
        statuses = []
        self.host.services.status = statuses.append

        async def frame(stream, sequence):
            return await self.fake.frame(stream, sequence), "00" * 48

        self.fake.frame_with_crop = frame
        ctrl, token = await self.hello()
        await ctrl.send(json.dumps(dict(type="START", seq=1, formats=["jpeg"], mode="PIXEL_1X",
                                        source_crop=[512, 512], crop_v=1, present_v=1, max_fps=5)))
        stream = json.loads(await ctrl.recv())["stream"]
        data = await self.socket("/image")
        await data.send(json.dumps(dict(type="BIND", session=token)))
        session = self.host.sessions[CAPTURE_SCOPE]
        receiver = Receiver("jpeg", stream)

        async def feedback(sequence, shown):
            self.assertEqual(json.loads(await asyncio.wait_for(data.recv(), 2))["type"], "CROP1")
            receiver.receive(await data.recv())
            self.assertEqual(receiver.sequence, sequence)
            await data.send(json.dumps(dict(type="FRAME_DONE", stream=stream, frame=sequence, shown=shown)))
            async with asyncio.timeout(1):
                while session.outstanding == sequence:
                    await asyncio.sleep(.005)

        await feedback(0, True)
        await feedback(1, True)
        confirmed = statuses[-1]
        self.assertIn("fps confirmed by KVMpad", confirmed)
        await feedback(2, False)
        self.assertEqual(statuses[-1], confirmed)
        await feedback(3, True)
        await feedback(4, False)
        self.assertIn("fps confirmed by KVMpad", statuses[-1])
        await feedback(5, False)
        self.assertIn("Waiting for KVMpad to display a new image", statuses[-1])
        self.assertTrue(session.displayed)
        self.assertFalse(session.presentations)
        count = len(statuses)
        await feedback(6, False)
        self.assertEqual(len(statuses), count)
        await feedback(7, True)
        self.assertEqual(statuses[-1], IMAGE_SHARING)
        self.assertEqual(len(session.presentations), 1)
        await feedback(8, True)
        self.assertIn("fps confirmed by KVMpad", statuses[-1])
        await ctrl.send(json.dumps(dict(type="STOP", seq=2)))
        await asyncio.wait_for(data.wait_closed(), 1)

    async def test_resume_reports_live_only_after_the_board_displays_a_new_frame(self):
        statuses = []
        self.host.services.status = statuses.append
        async def frame(stream, sequence):
            return await self.fake.frame(stream, sequence), "00" * 48
        self.fake.frame_with_crop = frame
        ctrl, token = await self.hello()
        await ctrl.send(json.dumps(dict(type="START", seq=1, formats=["jpeg"], mode="PIXEL_1X",
                                       source_crop=[512,512], crop_v=1, present_v=1, max_fps=5)))
        stream = json.loads(await ctrl.recv())["stream"]
        data = await self.socket("/image")
        await data.send(json.dumps(dict(type="BIND", session=token)))
        receiver = Receiver("jpeg", stream)
        async def image(sequence):
            self.assertEqual(json.loads(await asyncio.wait_for(data.recv(), 2))["type"], "CROP1")
            receiver.receive(await data.recv())
            self.assertEqual(receiver.sequence, sequence)
        async def done(sequence, shown):
            await data.send(json.dumps(dict(type="FRAME_DONE",stream=stream,frame=sequence,shown=shown)))
        await image(0)
        await done(0, True)
        await image(1)
        self.assertEqual(statuses[-1], IMAGE_SHARING)
        timings = self.host.sessions[CAPTURE_SCOPE].presentations
        self.assertEqual(len(timings), 1)
        await ctrl.send(json.dumps(dict(type="FREEZE", seq=2)))
        self.assertEqual(json.loads(await ctrl.recv()), {"type":"FROZEN","seq":2,"last_frame":1})
        self.assertIn("frozen", statuses[-1].lower())
        await asyncio.sleep(.12)
        await ctrl.send(json.dumps(dict(type="RESUME",seq=3)))
        self.assertEqual(json.loads(await ctrl.recv())["type"], "RESUMED")
        await image(2)
        await done(1, True)  # Delayed feedback belongs to the picture frozen earlier.
        await asyncio.sleep(.05)
        self.assertIn("resuming", statuses[-1].lower())
        self.assertFalse(timings)
        await done(2, False)
        await image(3)
        self.assertIn("could not display", statuses[-1])
        self.assertFalse(timings)
        await done(3, True)
        async with asyncio.timeout(1):
            while statuses[-1] != IMAGE_SHARING:
                await asyncio.sleep(.005)
        self.assertEqual(len(timings), 1)
        await ctrl.send(json.dumps(dict(type="STOP",seq=4)))
        await asyncio.wait_for(data.wait_closed(), 1)

    async def test_unfinished_fragment_times_out(self):
        ws = await self.socket("/control")
        wait = asyncio.Event()
        async def partial():
            yield '{"type":'
            await wait.wait()
            yield '"HELLO"}'
        sending = asyncio.create_task(ws.send(partial()))
        try:
            await asyncio.wait_for(ws.wait_closed(), 3)
            self.assertIsNone(self.host.session)
        finally:
            wait.set()
            await asyncio.gather(sending, return_exceptions=True)

    async def test_sequence_and_oversize(self):
        ws, _ = await self.hello()
        await ws.send(json.dumps(dict(type="START", seq=0, mode="PIXEL_1X", formats=["jpeg"])))
        await asyncio.wait_for(ws.wait_closed(), 1)
        await asyncio.sleep(.03)
        ws = await self.socket("/control")
        await ws.send(["A" * 2048, "B" * 2049])
        await asyncio.wait_for(ws.wait_closed(), 1)
        self.assertEqual(self.requested, [])

    async def test_both_certificate_validation_and_exact_peer_pin(self):
        with self.assertRaises((ConnectionClosed, InvalidMessage, OSError, EOFError)):
            async with connect(self.base + "/control", ssl=self.bare, server_hostname="localhost",
                               open_timeout=1, proxy=None) as ws:
                await ws.recv()
        self.host.pin = bytes(32)
        ws = await self.socket("/control")
        with self.assertRaises(ConnectionClosed):
            await ws.recv()
        self.assertFalse(self.host.session)

    async def test_control_loss_stops_image_and_data_loss_stops_control(self):
        for drop in ("control", "image"):
            self.fake = Synthetic()
            ws, token = await self.hello()
            await self.start(ws)
            await ws.recv()
            image = await self.socket("/image")
            await image.send(json.dumps(dict(type="BIND", session=token)))
            await image.recv()
            first, other = (ws, image) if drop == "control" else (image, ws)
            await first.close()
            await asyncio.wait_for(other.wait_closed(), 1)
            self.assertTrue(self.fake.revoked)
            await asyncio.sleep(.03)

    async def test_action_id_deduplicated(self):
        class Studio:
            count = 0
            async def action(self, name):
                self.count += 1
                return {"status":"SUCCESS", "recording":True}
        studio = Studio()
        self.host.services.providers["obs"] = studio
        ws, _ = await self.hello(STUDIO)
        msg = json.dumps(dict(type="ACTION", seq=1, source="obs", action="obs.record.start"))
        await ws.send(msg)
        self.assertEqual(json.loads(await ws.recv())["type"], "ACCEPTED")
        self.assertTrue(json.loads(await ws.recv())["recording"])
        await ws.send(msg)
        await asyncio.wait_for(ws.wait_closed(), 1)
        self.assertEqual(studio.count, 1)

    async def test_authored_profile_sessions(self):
        profile = "net.example.custom.cockpit"
        pitch = "sim.attitude.pitch_deg"
        approvals = []

        async def approve(identity, purpose):
            approvals.append((identity,purpose))
            return self.permit

        async def samples(topics, deliver):
            self.assertEqual(dict(topics), {pitch: 20})
            deliver({pitch: -7.5}, time.monotonic())
            await asyncio.Event().wait()

        self.host.services.approve = approve
        self.host.services.providers["sim.xplane"] = Mock(samples=samples)
        capture = "host.screen.region"
        telemetry = "host.telemetry.read"
        subscribe = dict(type="SUBSCRIBE", seq=1, source="sim.xplane", topics=[dict(name=pitch, rate_hz=20)])
        ws, token = await self.hello(profile,telemetry)
        await ws.send(json.dumps(subscribe))
        self.assertEqual(json.loads(await ws.recv())["type"], "SUBSCRIBED")
        chan = await self.socket("/telemetry")
        await chan.send(json.dumps(dict(type="BIND",session=token)))
        async with asyncio.timeout(2):
            while True:
                sample = json.loads(await chan.recv())
                self.assertEqual(sample["topic"],pitch)
                if sample["quality"] == "VALID":
                    break
        self.assertEqual(sample["value"], -7.5)
        await ws.send(json.dumps(dict(type="STOP", seq=2)))
        await chan.wait_closed()
        await ws.wait_closed()
        await asyncio.sleep(.03)
        self.assertEqual(approvals,[(profile,f"Read {pitch} from sim.xplane")])

        ws, token = await self.hello(profile,capture)
        await self.start(ws)
        reply = json.loads(await ws.recv())
        self.assertEqual(reply["type"], "STARTED")
        self.assertEqual(self.fake.calls,0)
        chan = await self.socket("/image")
        await chan.send(json.dumps(dict(type="BIND",session=token)))
        self.assertEqual(bytes(Receiver("jpeg",reply["stream"]).receive(await chan.recv())), b"test payload")
        await ws.send(json.dumps(dict(type="STOP", seq=2)))
        await chan.wait_closed()
        await ws.wait_closed()
        self.assertEqual(approvals[-1][0],profile)
        self.assertTrue(self.fake.revoked)

        start = dict(type="START", seq=1, mode="PIXEL_1X", formats=["jpeg"])
        action = dict(type="ACTION", seq=1, source="obs", action="obs.record.start")
        for scope, forbidden in [(telemetry,start), (capture,subscribe), (telemetry,action), (capture,action)]:
            await asyncio.sleep(.03)
            ws, _ = await self.hello(profile,scope)
            await ws.send(json.dumps(forbidden))
            self.assertEqual(json.loads(await ws.recv())["type"], "DENIED")
            await ws.wait_closed()
        self.assertEqual(len(approvals),2)
        self.permit = False
        await asyncio.sleep(.03)
        ws, _ = await self.hello(profile,telemetry)
        await ws.send(json.dumps(subscribe))
        self.assertEqual(json.loads(await ws.recv())["type"], "DENIED")
        await ws.wait_closed()
        self.assertEqual(approvals[-1], approvals[0])

    async def test_authored_profile_hello_bounds(self):
        for profile, extra in [
                ("net.example.profile", {}), ("net.example.profile", {"scope":"host.actions"}),
                ("net.example.profile", {"scope":None}), (STUDIO, {"scope":"host.screen.region"}),
                ("net.1.profile", {"scope":"host.telemetry.read"}), ("a.b."+"x"*97, {"scope":"host.screen.region"})]:
            ws = await self.socket("/control")
            await ws.send(json.dumps(dict(type="HELLO",protocol="kvmbridge",version=1,profile=profile,**extra)))
            with self.assertRaises(ConnectionClosed):
                await ws.recv()
        ws,_ = await self.hello("a.b."+"x"*96,"host.screen.region")
        await ws.close()
        self.assertEqual(self.requested,[])

    async def test_composite_reserves_only_two_complementary_scopes(self):
        profile = "net.example.flight-deck"

        async def refused(name=profile, **options):
            ws = await self.socket("/control")
            await ws.send(json.dumps(dict(type="HELLO", protocol="kvmbridge", version=1, profile=name, **options)))
            with self.assertRaises(ConnectionClosed):
                await asyncio.wait_for(ws.recv(), 1)

        for bad in (False, None, 0, 2, "1", 1.0):
            await refused(scope=TELEMETRY_SCOPE, composite_v=bad)
        await refused(MAG, composite_v=1)
        await refused(STUDIO, scope=CAPTURE_SCOPE, composite_v=1)
        await refused(scope="host.actions", composite_v=1)
        single, _ = await self.hello(profile, TELEMETRY_SCOPE)
        await refused(scope=CAPTURE_SCOPE, composite_v=1)
        await single.close()
        async with asyncio.timeout(1):
            while self.host.sessions:
                await asyncio.sleep(.005)

        # Reserve before SUBSCRIBE and before either user's decision.
        instruments, token = await self.hello(profile, TELEMETRY_SCOPE, composite_v=1)
        await refused(scope=TELEMETRY_SCOPE, composite_v=1)
        await refused(scope=CAPTURE_SCOPE)
        await refused("net.example.other", scope=CAPTURE_SCOPE, composite_v=1)
        await refused(MAG)
        await refused(STUDIO)
        display, other = await self.hello(profile, CAPTURE_SCOPE, composite_v=1)
        self.assertNotEqual(token, other)
        await refused(scope=CAPTURE_SCOPE, composite_v=1)
        await asyncio.gather(display.close(), instruments.close())
        self.assertEqual(self.requested, [])

    async def test_composite_approval_queue_preserves_capture_status(self):
        with (patch("host.tk.Tk"), patch("host.ttk.Frame"), patch("host.ControlAppearance"),
              patch("host.ttk.Label", side_effect=lambda *a, **kw: Mock()),
              patch("host.ttk.Button", side_effect=lambda *a, **kw: Mock())):
            ui = Foreground(self.host.pin)
        ui.broker = self.host
        review_started = asyncio.Queue()

        async def review(profile, purpose):
            review_started.put_nowait(purpose)
            return await ui.approve(profile, purpose)

        self.host.services.approve = review
        self.host.services.status = ui.status
        self.host.services.providers["sim.xplane"] = Mock()
        profile = "net.example.flight-deck"
        display, token = await self.hello(profile, CAPTURE_SCOPE, composite_v=1)
        instruments, _ = await self.hello(profile, TELEMETRY_SCOPE, composite_v=1)
        await self.start(display)
        capture_purpose = await asyncio.wait_for(review_started.get(), 1)
        self.assertIn("choose an entire monitor", capture_purpose)
        self.assertIn("Pixel 1x · JPEG · Requested 512 × 512 px · On KVMpad 512 × 512 px", capture_purpose)
        await instruments.send(json.dumps(dict(type="SUBSCRIBE", seq=1, source="sim.xplane",
                               topics=[dict(name="sim.altitude_ft", rate_hz=10)])))
        instrument_purpose = await asyncio.wait_for(review_started.get(), 1)
        ui.review.config.assert_called_with(text=f"{profile}\n{capture_purpose}")
        ui.decide(True)
        self.assertEqual(json.loads(await display.recv())["type"], "STARTED")
        ui.review.config.assert_called_with(text=f"{profile}\n{instrument_purpose}")
        ui.allow.state.assert_called_with(["!disabled"])
        ui.label.config.assert_called_with(text=DISPLAY_REVIEW)
        image = await self.socket("/image")
        await image.send(json.dumps(dict(type="BIND", session=token)))
        await image.recv()
        ui.decide(False)
        self.assertEqual(json.loads(await instruments.recv())["type"], "DENIED")
        await instruments.wait_closed()
        self.assertIsInstance(await image.recv(), bytes)
        self.assertIn(IMAGE_SHARING, ui.label.config.call_args.kwargs["text"])
        self.assertIn("Instrument sharing was declined", ui.label.config.call_args.kwargs["text"])
        ui.stop()
        await asyncio.wait_for(asyncio.gather(display.wait_closed(), image.wait_closed()), 1)
        ui.label.config.assert_called_with(text="Stopped")
        self.assertTrue(self.fake.revoked)

    async def test_composite_telemetry_quality_image_credit_and_revoke(self):
        profile, topic = "net.example.flight-deck", "sim.altitude_ft"
        source_ready = asyncio.Event()
        source_stopped = asyncio.Event()
        observations = []
        statuses = []
        self.host.services.status = statuses.append

        async def samples(rates, deliver):
            self.assertEqual(rates, {topic: 10})
            observations.append(deliver)
            source_ready.set()
            try:
                await asyncio.Event().wait()
            finally:
                source_stopped.set()

        async def frame_with_crop(stream, seq):
            return await self.fake.frame(stream, seq), "00" * 48

        async def bind(path, token):
            ws = await self.socket(path)
            await ws.send(json.dumps(dict(type="BIND", session=token)))
            return ws

        counters = []
        async def quality(expected):
            async with asyncio.timeout(1):
                while True:
                    sample = json.loads(await telemetry.recv())
                    self.assertEqual(sample["seq"], len(counters) + 1)
                    self.assertEqual(sample["topic"], topic)
                    counters.append(sample["quality"])
                    if sample["quality"] == expected:
                        return sample

        self.fake.frame_with_crop = frame_with_crop
        self.host.services.providers["sim.xplane"] = Mock(samples=samples)
        measures, read_token = await self.hello(profile, TELEMETRY_SCOPE, composite_v=1)
        await measures.send(json.dumps(dict(type="SUBSCRIBE", seq=1, quality_v=1, source="sim.xplane",
                                  topics=[dict(name=topic, rate_hz=10)])))
        self.assertEqual(json.loads(await measures.recv())["type"], "SUBSCRIBED")
        telemetry = await bind("/telemetry", read_token)
        await asyncio.wait_for(source_ready.wait(), 1)
        self.assertIsNone((await quality("UNAVAILABLE"))["value"])

        # An independent capture denial leaves the provider and its transport alone.
        self.permit = False
        denied, rejected = await self.hello(profile, CAPTURE_SCOPE, composite_v=1)
        await self.start(denied)
        self.assertEqual(json.loads(await denied.recv())["type"], "DENIED")
        await denied.wait_closed()
        self.assertEqual(self.fake.calls, 0)
        self.assertIn("Sharing instruments", statuses[-1])
        self.assertIn("Display sharing was declined", statuses[-1])
        self.permit = True
        async with asyncio.timeout(1):
            while CAPTURE_SCOPE in self.host.sessions:
                await asyncio.sleep(.005)
        display, write_token = await self.hello(profile, CAPTURE_SCOPE, composite_v=1)
        self.assertEqual(len({write_token, read_token, rejected}), 3)
        await display.send(json.dumps(dict(type="START", seq=1, formats=["jpeg"], mode="PIXEL_1X",
                                     crop_v=1, source_crop=[512, 512], present_v=1, max_fps=10)))
        stream = json.loads(await display.recv())["stream"]
        for token in (read_token, rejected):
            wrong = await bind("/image", token)
            with self.assertRaises(ConnectionClosed):
                await asyncio.wait_for(wrong.recv(), 1)
        self.assertEqual(self.fake.calls, 0)
        self.assertIn(DISPLAY_REVIEW, statuses[-1])
        self.assertIn("Sharing instruments", statuses[-1])

        image = await bind("/image", write_token)
        receiver = Receiver("jpeg", stream)
        async def picture():
            self.assertEqual(json.loads(await image.recv())["type"], "CROP1")
            self.assertEqual(bytes(receiver.receive(await image.recv())), b"test payload")

        await picture()
        # One pending image. A previously sampled source value ages, faults and recovers in that interval.
        observations[0]({topic: 4200}, time.monotonic() - 2.65)
        self.assertEqual((await quality("STALE"))["value"], 4200)
        self.assertIsNone((await quality("UNAVAILABLE"))["value"])
        observations[0]({topic: float("nan")}, time.monotonic())
        self.assertIsNone((await quality("ERROR"))["value"])
        observations[0]({topic: 4210}, time.monotonic())
        self.assertEqual((await quality("VALID"))["value"], 4210)
        self.assertEqual(self.fake.calls, 1)
        await image.send(json.dumps(dict(type="FRAME_DONE", stream=stream, frame=0, shown=True)))
        await asyncio.wait_for(picture(), 2)
        self.assertEqual(receiver.sequence, 1)
        self.assertEqual(self.fake.calls, 2)
        await image.close()
        await asyncio.wait_for(display.wait_closed(), 1)
        self.assertTrue(self.fake.revoked)
        self.assertFalse(source_stopped.is_set())
        self.assertIn("Sharing instruments", statuses[-1])
        self.assertIn("closed the data connection", statuses[-1])
        observations[0]({topic: 4220}, time.monotonic())
        async with asyncio.timeout(1):
            while (await quality("VALID"))["value"] != 4220:
                pass
        async with asyncio.timeout(1):
            while CAPTURE_SCOPE in self.host.sessions:
                await asyncio.sleep(.005)
        self.fake = Synthetic()
        display, fresh = await self.hello(profile, CAPTURE_SCOPE, composite_v=1)
        await self.start(display)
        self.assertEqual(json.loads(await display.recv())["type"], "STARTED")
        image = await bind("/image", fresh)
        self.assertIsInstance(await image.recv(), bytes)
        # The physical HOST Stop revokes both grants even when all four sockets are live.
        self.host.stop()
        await asyncio.wait_for(asyncio.gather(measures.wait_closed(), telemetry.wait_closed(),
                                              display.wait_closed(), image.wait_closed(), source_stopped.wait()), 1)
        self.assertTrue(self.fake.revoked)
        self.assertEqual(statuses[-1], "Stopped")

    @unittest.skipUnless(os.environ.get("KVMPAD_TEST_SURFACE") and shutil.which("ffmpeg"),
                         "Set KVMPAD_TEST_SURFACE to the offscreen fixture and provide FFmpeg")
    async def test_composite_native_jpeg_with_six_xplane_datarefs(self):
        from test_loopback_telemetry import RATES, Simulator
        from test_loopback_pixels import require_fixture, send
        from test_presentation_pacing import picture

        surface = require_fixture(Path(os.environ["KVMPAD_TEST_SURFACE"]))
        ffmpeg = shutil.which("ffmpeg")
        endpoint = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Simulator)
        endpoint.daemon_threads = True
        endpoint.fixture = SimpleNamespace(phase="A", gets=0, lookups={"A": set(), "B": set()}, stale_id=0)
        threading.Thread(target=endpoint.serve_forever, daemon=True).start()
        self.addCleanup(endpoint.server_close)
        self.addAsyncCleanup(asyncio.to_thread, endpoint.shutdown)
        self.host.services.providers["sim.xplane"] = XPlane(endpoint.server_port)
        self.host.services.capture = lambda: Capture(surface, ffmpeg)
        profile = "net.example.flight-deck"
        image_ctrl, image_token = await self.hello(profile, CAPTURE_SCOPE, composite_v=1)
        source_ctrl, read_token = await self.hello(profile, TELEMETRY_SCOPE, composite_v=1)
        await send(source_ctrl, type="SUBSCRIBE", seq=1, quality_v=1, source="sim.xplane",
                   topics=[dict(name=name, rate_hz=rate) for name, rate in RATES.items()])
        self.assertEqual(json.loads(await source_ctrl.recv())["type"], "SUBSCRIBED")
        telemetry = await self.socket("/telemetry")
        await send(telemetry, type="BIND", session=read_token)
        counts = dict.fromkeys(RATES, 0)

        async def collect():
            async for message in telemetry:
                sample = json.loads(message)
                if sample["quality"] == "VALID":
                    counts[sample["topic"]] += 1

        reader = asyncio.create_task(collect())
        try:
            with patch.dict(os.environ, KVMPAD_TEST_GPU="warp", KVMPAD_TEST_LAYOUT="paced"):
                await send(image_ctrl, type="START", seq=1, formats=["jpeg"], mode="PIXEL_1X",
                           crop_v=1, source_crop=[512, 512], present_v=1, max_fps=10)
                started = json.loads(await asyncio.wait_for(image_ctrl.recv(), 5))
            stream = started["stream"]
            image = await self.socket("/image")
            await send(image, type="BIND", session=image_token)
            receiver = Receiver("jpeg", stream)
            async def receive_frame(sequence):
                record = json.loads(await asyncio.wait_for(image.recv(), 3))
                self.assertEqual(record["type"], "CROP1")
                raw = bytes.fromhex(record["data"])
                wire = await asyncio.wait_for(image.recv(), 1)
                payload = bytes(receiver.receive(wire))
                self.assertEqual(receiver.sequence, sequence)
                self.assertEqual(struct.unpack_from("<IIII", raw, 32), (stream, sequence,
                                 struct.unpack_from("<I", wire, 28)[0], zlib.crc32(raw[:44])))
                if sequence in (0, 8):
                    _, _, _, left, top, *_ = CROP_PREFIX.unpack(raw[:32])
                    landmarks = await asyncio.to_thread(picture, ffmpeg, "jpeg", payload, left, top)
                    self.assertGreater(landmarks, 100)
                return len(payload)

            sizes = []
            for index in range(8):
                sizes.append(await receive_frame(index))
                if index < 7:
                    await send(image, type="FRAME_DONE", stream=stream, frame=index, shown=True)
            self.assertGreater(min(counts.values()), 1)
            before_stop = dict(counts)
            await send(source_ctrl, type="STOP", seq=2)
            await asyncio.wait_for(source_ctrl.wait_closed(), 2)
            await send(image, type="FRAME_DONE", stream=stream, frame=7, shown=True)
            sizes.append(await receive_frame(8))
            self.assertEqual(self.requested, [profile, profile])
            print(f"Composite loopback: {len(sizes)} JPEGs ({min(sizes)} to {max(sizes)} bytes); "
                  f"valid samples during the first eight pictures: {before_stop}")
        finally:
            self.host.stop()
            await asyncio.gather(reader, return_exceptions=True)

    async def test_subscribe_with_drawn_out_fragment_and_missing_provider(self):
        class Source:
            async def samples(self, names, deliver):
                deliver({"sim.airspeed_kt":123.4}, time.monotonic())
                await asyncio.Event().wait()
        self.host.services.providers["sim.xplane"] = Source()
        ws, token = await self.hello(GAUGES)
        await ws.send(json.dumps(dict(type="SUBSCRIBE", seq=1, source="sim.xplane",
                    topics=[{"name":"sim.airspeed_kt","rate_hz":10}])))
        self.assertEqual(json.loads(await ws.recv())["type"], "SUBSCRIBED")
        data = await self.socket("/telemetry")
        bind = json.dumps(dict(type="BIND", session=token))
        await data.send([bind[:10], bind[10:]])
        for _ in range(4):
            sample = json.loads(await data.recv())
            if sample["quality"] == "VALID":
                break
        self.assertEqual(sample["value"], 123.4)
        await ws.close()


class OfflineCaptureTests(unittest.IsolatedAsyncioTestCase):
    async def test_confirmed_frame_rate_and_request_to_lcd_acknowledgement(self):
        status = Mock()
        session = Session(Mock(), MAG, Services(None, status, None, {}))
        try:
            for i in range(42):
                now = 100 + i * .25
                # Only the last two frames have slow acknowledgements. Both lie inside
                # the 32-picture window, so nearest-rank p95 must include the first.
                session.frame_started = now - (.24 if i >= 40 else .075)
                session.record_presentation(now)
                if i < 19:
                    self.assertNotIn("p95", session.status_text)
            self.assertEqual(len(session.presentations), 32)
            self.assertIn("4.0 fps confirmed by KVMpad", session.status_text)
            self.assertIn("p95 240 ms", session.status_text)
            self.assertLessEqual(status.call_count, 6)
        finally:
            session.deadline.cancel()

    async def test_stalled_crop_write_expires_image_before_binary_send(self):
        session = Session(Mock(), MAG, Services(None, Mock(), None, {}))
        session.kind, session.stream, session.crop_v = "/image", 4, True
        session.bind_timeout = Mock()
        session.capture = Mock()
        clock = 100.
        message = encode("h264-intra", 4, 0, 100000, b"synthetic")
        async def capture(stream, sequence):
            nonlocal clock
            clock += .7
            return message, "00" * 48
        async def write(data):
            nonlocal clock
            if isinstance(data, bytes):
                raise AssertionError("A stale image reached the binary WebSocket send")
            clock += .4
        channel = Mock()
        channel.recv = AsyncMock(side_effect=asyncio.Event().wait)
        channel.send = AsyncMock(side_effect=write)
        session.capture.frame_with_crop = AsyncMock(side_effect=capture)
        with patch("broker.time") as timer:
            timer.monotonic.side_effect = lambda: clock
            await session.bind(channel)
        self.assertEqual([json.loads(call.args[0])["type"] for call in channel.send.await_args_list
                          if isinstance(call.args[0], str)], ["CROP1"])
        self.assertEqual(channel.send.await_count, 1)
        self.assertEqual(session.last_image_seq, -1)
        self.assertFalse(session.active)
        self.assertIn("expired", session.status_text)
        channel.transport.abort.assert_called_once()

    async def test_denial_never_opens_picker(self):
        ws = Mock()
        ws.send = AsyncMock()
        factory = Mock()
        session = Session(ws, MAG, Services(AsyncMock(return_value=False), Mock(), factory, {}))
        await session.operate({"type":"START", "seq":1, "formats":["jpeg"], "mode":"PIXEL_1X"})
        factory.assert_not_called()
        self.assertFalse(session.active)
        self.assertEqual(json.loads(ws.send.await_args.args[0])["type"], "DENIED")

    async def test_slow_image_channel_limits_capture_demand(self):
        session = Session(Mock(), MAG, Services(None, Mock(), None, {}))
        session.kind, session.stream = "/image", 4
        session.capture = Mock()
        session.capture.frame = AsyncMock(return_value=encode("jpeg", 4, 0, 0, b"x"))
        sending, release = asyncio.Event(), asyncio.Event()
        async def send_frame(_message):
            sending.set()
            await release.wait()
        session.channel = Mock()
        session.channel.send = send_frame
        session.producer = asyncio.create_task(session.produce())
        try:
            await asyncio.wait_for(sending.wait(), 2)
            await asyncio.sleep(.25)  # longer than the ordinary 5 fps demand interval
            session.capture.frame.assert_awaited_once_with(4, 0)
        finally:
            session.stop()
            await asyncio.gather(session.producer, return_exceptions=True)
        session.capture.revoke.assert_called_once()

    async def test_stale_encoded_frames_are_discarded_before_transmission(self):
        session = Session(Mock(), MAG, Services(None, Mock(), None, {}))
        session.kind, session.stream = "/image", 4
        session.capture, session.channel = Mock(), Mock()
        session.channel.send = AsyncMock()
        now = 0
        async def delayed_frame(stream, sequence):
            nonlocal now
            self.assertEqual((stream, sequence), (4, 0))
            now += 1.1
            return b"expired"
        session.capture.frame = AsyncMock(side_effect=delayed_frame)
        try:
            with patch("broker.time") as clock:
                clock.monotonic.side_effect = lambda: now
                with self.assertRaisesRegex(ValueError, "No usable fresh frame"):
                    await session.produce()
            self.assertEqual(session.capture.frame.await_count, 6)
            session.channel.send.assert_not_awaited()
            self.assertEqual(session.rate.fps, 1)
        finally:
            session.stop()

    async def test_h264_cautiously_restores_quality(self):
        capture = Capture(None, None)
        capture.process = Mock()
        capture.process.poll.return_value = None
        capture.selected = True
        capture.width = capture.height = 512
        capture.codec, capture.quality_step = "h264-intra", 2
        with patch("media.exchange", return_value=b"raw"), patch.object(capture, "encode_h264", new_callable=AsyncMock) as encoder:
            encoder.return_value = b"a" * 32
            for seq in range(11):
                await capture.frame(1, seq)
            self.assertEqual(capture.quality_step, 2)
            encoder.return_value = b"a" * 50000
            await capture.frame(1, 11)
            self.assertEqual(capture.small_frames, 0)
            encoder.return_value = b"a" * 32
            for seq in range(12, 24):
                await capture.frame(1, seq)
            self.assertEqual(capture.quality_step, 1)

    def test_presentation_rate_bounds_and_reset(self):
        for ceiling in (1, 3, 5, 10):
            rate = FrameRate(ceiling)
            self.assertEqual(rate.fps, min(5, ceiling))
            for _ in range(100):
                rate.observe(.01, True)
            self.assertEqual(rate.fps, ceiling)
            rate.observe(.4, True)
            self.assertLessEqual(rate.fps, 2)
            for _ in range(12):
                rate.observe(.6, False)
            self.assertEqual(rate.fps, 1)
            rate.reset()
            self.assertEqual(rate.fps, min(5, ceiling))

    @unittest.skipUnless(shutil.which("ffmpeg"), "FFmpeg unavailable")
    async def test_h264_oversize_is_dropped_then_quality_recovers(self):
        capture = Capture(None, shutil.which("ffmpeg"))
        self.addAsyncCleanup(capture.close_encoder)
        capture.process = Mock()
        capture.process.poll.return_value = None
        capture.selected = True
        capture.width = capture.height = 512
        capture.codec, capture.quality_step = "h264-intra", 0
        pixels = random.Random(83).randbytes(512 * 512 * 4)
        with patch("media.exchange", return_value=pixels):
            self.assertIsNone(await capture.frame(4, 0))
            for _ in range(4):
                message = await capture.frame(4, 0)
                if message is not None:
                    break
        self.assertIsNotNone(message)
        payload = Receiver("h264-intra", 4).receive(message)
        self.assertTrue(0 < len(payload) <= 98304)


class ProviderTests(unittest.IsolatedAsyncioTestCase):
    @unittest.skipUnless(os.environ.get("KVMPAD_TEST_STUDIO_DLL"), "Build the portable firmware Studio fixture")
    async def test_firmware_studio_wire_interop(self):
        lib=ctypes.CDLL(os.environ["KVMPAD_TEST_STUDIO_DLL"])
        lib.peer_receive.argtypes=[ctypes.c_char_p]
        lib.peer_receive.restype=lib.peer_next.restype=lib.peer_action.restype=lib.peer_finished.restype=ctypes.c_bool
        lib.peer_reset()
        async def write(wire):
            self.assertTrue(lib.peer_receive(wire), f"Firmware rejected opcode {wire[2]}")
        class Obs:
            async def action(self,name):
                return {"status":"SUCCESS","recording":True}
        link=Studio(write,Obs(),True)
        buf=ctypes.create_string_buffer(64)
        self.assertTrue(lib.peer_next(buf))
        await link.receive(buf.raw)
        self.assertTrue(lib.peer_next(buf))
        await link.receive(buf.raw)
        self.assertTrue(lib.peer_action())
        self.assertTrue(lib.peer_next(buf))
        await link.receive(buf.raw)
        await link.pending
        self.assertTrue(lib.peer_finished())
        await link.close(bye=True)

    async def test_usb_studio_handshake_feedback_and_retry(self):
        class FakeObs:
            calls = 0
            async def action(self, name):
                self.calls += 1
                return {"status":"SUCCESS", "recording":True}
        sent = []
        async def write(wire):
            self.assertEqual(len(wire), 64)
            self.assertEqual(wire[:2], bytes((0x23, 1)))
            sent.append(wire)
        obs = FakeObs()
        link = Studio(write, obs, mute=True)
        async def deliver(op, seq, payload, flags=0):
            for report in frames(0x22, op, seq, payload, flags):
                await link.receive(report)
        await deliver(1, 0xfffe, struct.pack("<BBHI", 1, 0, 0x3f, 42))
        self.assertEqual([w[2] for w in sent], [2, 0x14, 0x15, 0x15])
        self.assertEqual(struct.unpack_from("<I", sent[0], 15)[0], 42)
        await deliver(3, 0xffff, struct.pack("<I", 123))
        self.assertEqual(sent[-1][2], 4)
        req = struct.pack("<HHI", 0x101, 1, 7)
        await deliver(0x10, 0, req, 1)
        await link.pending
        self.assertEqual([w[2] for w in sent[-5:]], [0x21,0x11,0x13,0x13,0x12])
        self.assertEqual(struct.unpack_from("<IHI", sent[-1], 11), (7,0,1))
        await deliver(0x10, 0, req, 1)
        self.assertEqual(obs.calls, 1)
        await deliver(0x10, 1, struct.pack("<HHI", 0xfff, 2, 8), 1)
        self.assertEqual(struct.unpack_from("<IB", sent[-1], 11), (8,0))
        await link.close()

    def test_usb_crc_padding_fragment_order_and_timeout(self):
        reports = list(frames(0x22, 0x10, 9, bytes(range(102))))
        a = Assembly()
        self.assertIsNone(a.read(reports[0], 0))
        self.assertEqual(a.read(reports[1], .01)[-1], bytes(range(102)))
        with self.assertRaises(ValueError):
            Assembly().read(reports[1], 0)
        a = Assembly()
        a.read(reports[0], 0)
        with self.assertRaises(ValueError):
            a.read(reports[1], .31)
        corrupt = bytearray(reports[0])
        corrupt[21] ^= 0x20
        with self.assertRaises(ValueError):
            Assembly().read(corrupt, 0)

    def test_listener_leaf_matches_its_encrypted_key_and_interface(self):
        address = ipaddress.IPv4Address("192.168.7.2")
        cert, key = host_identity(address, "test-only")
        self.assertIn(b"ENCRYPTED PRIVATE KEY", key)
        private = serialization.load_pem_private_key(key, password=b"test-only")
        self.assertEqual(private.public_key().public_numbers(), cert.public_key().public_numbers())
        san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        self.assertEqual(san.get_values_for_type(x509.IPAddress), [address])
    def test_four_qualities_and_legacy_fallback(self):
        name, other = list(TOPICS)[:2]
        for invalid in (None, False, {}, [], "19", float("inf"), float("nan"), 10**1000):
            for version in (False, True):
                with self.subTest(value=type(invalid), quality_v=version):
                    t = Telemetry("sim.xplane", {name:10, other:10}, version)
                    t.update({name:invalid, other:42}, 0)
                    def states(now):
                        return [(m["quality"], m["value"]) for m in t.messages(now)]
                    fault = ("ERROR" if version else "UNAVAILABLE", None)
                    self.assertEqual(states(0), [fault, ("VALID", 42)])
                    self.assertEqual(states(1), [fault, ("STALE", 42)])
                    self.assertEqual(states(3), [("UNAVAILABLE", None)]*2)
                    t.update({name:0, other:MISSING_TOPIC}, 3.2)
                    self.assertEqual(states(3.2), [("VALID", 0), ("UNAVAILABLE", None)])

    def test_stale_units_validation(self):
        name = "sim.heading_deg"
        t = Telemetry("sim.xplane", {name: 10})
        t.update({name: -2}, 1)
        for now, quality, value in ((1, "VALID", 358), (1.125, "VALID", 358), (2, "STALE", 358), (4, "UNAVAILABLE", None)):
            m, = t.messages(now)
            self.assertEqual((m["quality"], m["value"], m["unit"]), (quality, value, "deg"))
            self.assertEqual((m["ts_mono_ms"],m["age_ms"]), (1000,int((now-1)*1000)))
        for topics in ([{"name": name, "rate_hz": 1}]*17, [{"name":name,"rate_hz":20}],
                       [{"name":name,"rate_hz":True}], [{"name":"arbitrary","rate_hz":1}]):
            with self.assertRaises(ValueError):
                subscriptions(topics)

    async def test_obs_challenge_request_and_feedback(self):
        received = []
        async def fake(ws):
            await ws.send(json.dumps({"op":0,"d":{"rpcVersion":1,"authentication":{"salt":"s","challenge":"c"}}}))
            identify = json.loads(await ws.recv())
            self.assertEqual(identify["d"]["authentication"], obs_auth("test","s","c"))
            await ws.send(json.dumps({"op":2,"d":{"negotiatedRpcVersion":1}}))
            for _ in range(2):
                req = json.loads(await ws.recv())["d"]
                received.append(req["requestType"])
                await ws.send(json.dumps({"op":7,"d":dict(req, requestStatus={"result":True},
                                                         responseData={"outputActive":True})}))
        async with serve(fake, "127.0.0.1", 0, subprotocols=["obswebsocket.json"]) as server:
            obs = OBS("test", port=server.sockets[0].getsockname()[1])
            result = await obs.action("obs.record.start")
            self.assertEqual(result, {"status":"SUCCESS","recording":True})
            self.assertEqual(received, ["StartRecord", "GetRecordStatus"])

    async def test_xplane_real_http_lookup_and_samples(self):
        commands = []
        class Fake(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *args):
                pass
            def do_GET(self):
                path = urlsplit(self.path)
                if path.path.endswith(("/datarefs", "/commands")):
                    values = parse_qs(path.query)["filter[name]"]
                    result = [{"id":i, "name":n} for i,n in enumerate(values)]
                else:
                    result = 101.25
                b = json.dumps({"data":result}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
            def do_POST(self):
                commands.append((self.path, json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            provider = XPlane(server.server_port)
            self.assertEqual(await asyncio.to_thread(provider.read, TOPICS), dict.fromkeys(TOPICS, 101.25))
            self.assertEqual(await provider.action("sim.flaps.up"), {"status":"UNVERIFIED"})
            self.assertEqual(commands, [("/api/v2/command/0/activate", {"duration":0})])
        finally:
            await asyncio.to_thread(server.shutdown)
            server.server_close()
            thread.join()

    @unittest.skipUnless(os.environ.get("KVMPAD_TEST_PICKER"), "Set KVMPAD_TEST_PICKER to the built executable")
    async def test_cinepak_preserves_every_vector_that_fits_the_codebook(self):
        def gray(value):
            return bytes((value, value, value, 255))

        for count in (1, 255, 256, 257):
            with self.subTest(count=count):
                # Each 4x4 block has four uniform gray quadrants. The 257th differs from the
                # first only in the fourth quadrant, so a single-channel key would miss it.
                vectors = [bytes((i % 256, (255 - i) % 256, (i * 7) % 256, 80 + i // 256, 0, 0))
                           for i in range(count)]
                tiles = [(gray(v[0]) * 2 + gray(v[1]) * 2, gray(v[2]) * 2 + gray(v[3]) * 2) for v in vectors]
                raster = bytearray()
                for by in range(135):
                    row = [tiles[(by * 240 + bx) % count] for bx in range(240)]
                    for pair in (0, 1):
                        raster.extend(b"".join(tile[pair] for tile in row) * 2)
                result = await asyncio.to_thread(subprocess.run,
                    [os.environ["KVMPAD_TEST_PICKER"], "--encode-cinepak"], input=raster,
                    capture_output=True, check=True, timeout=5)
                packet = result.stdout
                self.assertEqual(len(packet), CINEPAK_BYTES)
                book = [packet[26 + i*6 : 32 + i*6] for i in range(256)]
                indices = packet[26 + 256*6 + 4:]
                self.assertEqual(len(indices), 240 * 135)
                actual = [book[i] for i in indices]
                if count <= 256:
                    mismatches = sum(value != vectors[i % count] for i, value in enumerate(actual))
                    self.assertEqual(mismatches, 0)
                else:
                    self.assertLessEqual(len(set(actual)), 256)

    @unittest.skipUnless(os.environ.get("KVMPAD_TEST_PICKER"), "Set KVMPAD_TEST_PICKER to the built executable")
    async def test_native_encoders(self):
        for name in ("jpeg", "cinepak-intra"):
            fmt = FORMATS[name]
            with self.subTest(name=name):
                row = b"".join(bytes((x % 256, (x//8) % 256, 200, 255)) for x in range(fmt.width))
                cmd = [os.environ["KVMPAD_TEST_PICKER"], "--encode-" + name.split("-")[0]]
                start = time.perf_counter()
                result = await asyncio.to_thread(subprocess.run, cmd, input=row * fmt.height * 8,
                                                 capture_output=True, check=True, timeout=5)
                elapsed = time.perf_counter() - start
                stride = len(result.stdout) // 8
                payload = result.stdout[:stride]
                self.assertEqual(payload * 8, result.stdout)
                if name == "jpeg":
                    self.assertEqual(struct.unpack_from("<I", payload)[0], len(payload) - 4)
                    payload = payload[4:]
                self.assertTrue(0 < len(payload) <= 98304)
                if name == "jpeg":
                    self.assertTrue(payload.startswith(b"\xff\xd8") and payload.endswith(b"\xff\xd9"))
                else:
                    self.assertEqual(len(payload), CINEPAK_BYTES)
                    self.assertEqual(int.from_bytes(payload[1:4], "big"), len(payload))
                    self.assertEqual(payload[4:11], bytes.fromhex("03c0021c000110"))
                print(f"{name}: {8/elapsed:.2f} synthetic encodes/s, {len(payload)} bytes")

    @unittest.skipUnless(os.environ.get("KVMPAD_TEST_PICKER"), "Set KVMPAD_TEST_PICKER to the built executable")
    async def test_detailed_jpeg_obeys_byte_budget(self):
        pixels = random.Random(27).randbytes(512 * 512 * 4)
        result = await asyncio.to_thread(subprocess.run, [os.environ["KVMPAD_TEST_PICKER"], "--encode-jpeg"],
                                         input=pixels, capture_output=True, check=True, timeout=3)
        length, = struct.unpack_from("<I", result.stdout)
        self.assertEqual(length, len(result.stdout) - 4)
        self.assertTrue(0 < length <= 98304)

    @unittest.skipUnless(shutil.which("ffmpeg"), "FFmpeg unavailable")
    async def test_independent_h264(self):
        capture = Capture(None, shutil.which("ffmpeg"))
        self.addAsyncCleanup(capture.close_encoder)
        capture.width, capture.height, capture.quality_step = 512, 512, 0
        pixels = bytes.fromhex("a5a5ffff") * 512**2
        start = time.perf_counter()
        first_encoder = None
        for index in range(8):
            payload = await capture.encode_h264(pixels)
            if index == 0:
                first_encoder = capture.encoder
            self.assertIs(capture.encoder, first_encoder)
            self.assertTrue(0 < len(payload) <= 98304)
            nals = [n[0] & 31 for n in payload.split(b"\0\0\1") if n and n != b"\0"]
            self.assertTrue({7,8,5} <= set(nals))
        print(f"h264-intra: {8/(time.perf_counter()-start):.2f} synthetic encodes/s, {len(payload)} bytes")
        capture.process = Mock()
        await capture.pause()
        self.assertIsNone(capture.encoder)
        if first_encoder:
            self.assertIsNotNone(first_encoder.poll())
        await capture.encode_h264(pixels)
        resumed = capture.encoder
        capture.quality_step += 1
        await capture.encode_h264(pixels)
        if resumed:
            self.assertIsNot(resumed, capture.encoder)
            self.assertIsNotNone(resumed.poll())

    @unittest.skipUnless(shutil.which("ffmpeg"), "FFmpeg unavailable")
    async def test_h264_encoder_without_packet_statistics(self):
        capture = Capture(None, shutil.which("ffmpeg"))
        self.addAsyncCleanup(capture.close_encoder)
        capture.width, capture.height, capture.quality_step = 64, 64, 0
        launch = media.child
        def older_encoder(*args):
            return launch(*("-unsupported-kvmpad-test" if arg == "-stats_mux_pre" else arg for arg in args))
        with patch("media.child", side_effect=older_encoder) as starts:
            for _ in range(2):
                self.assertTrue(await capture.encode_h264(bytes(64 * 64 * 4)))
                self.assertFalse(capture.sized_packets)
                self.assertIsNone(capture.encoder)
            self.assertEqual(starts.call_count, 3)

    async def test_h264_cancel_reaps_waiting_encoder(self):
        capture = Capture(None, None)
        self.addAsyncCleanup(capture.close_encoder)
        capture.width, capture.height, capture.quality_step = 64, 64, 0
        launch = media.child
        started = asyncio.Event()
        loop = asyncio.get_running_loop()
        def stalled(*args):
            process = launch(sys.executable, "-I", "-c", "import sys; sys.stdin.buffer.read()")
            loop.call_soon_threadsafe(started.set)
            return process
        with patch("media.child", side_effect=stalled):
            task = asyncio.create_task(capture.encode_h264(bytes(64 * 64 * 4)))
            await asyncio.wait_for(started.wait(), 3)
            async with asyncio.timeout(3):
                while capture.encoder is None:
                    await asyncio.sleep(.001)
            process = capture.encoder
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
        self.assertIsNone(capture.encoder)
        self.assertIsNotNone(process.poll())
        self.assertTrue(process.stdin.closed and process.stdout.closed)


if __name__ == "__main__":
    unittest.main(verbosity=2)
