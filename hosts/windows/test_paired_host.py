# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise the paired CLIENT pipe, listener identity, mTLS and grant lifetime on loopback."""
import asyncio
import base64
import io
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidHandshake

import host
from broker import GAUGES
from paired_host import listener_identity, read_board
from test_host import identity


class LocalUI:
    def __init__(self, pin):
        self.root = SimpleNamespace(destroy=lambda: None)

    def status(self, text):
        pass

    async def approve(self, profile, message):
        return profile == GAUGES

    async def run(self):
        await asyncio.Event().wait()


class LocalPlane:
    async def samples(self, rates, update):
        update(dict.fromkeys(rates, 123.0), time.monotonic())
        await asyncio.Event().wait()


@unittest.skipUnless(os.name == "nt", "Windows HOST")
class PairedHostTests(unittest.IsolatedAsyncioTestCase):
    async def test_only_the_bundled_picker_is_the_pc_consent_for_a_paired_host(self):
        for name, leaf, direct in (("kvmpad-capture.exe", b"paired", True),
                                   ("custom.exe", b"paired", False),
                                   ("kvmpad-capture.exe", None, False)):
            with self.subTest(name=name, paired=bool(leaf)):
                picker = self.folder / name
                picker.touch()
                ui = Mock()
                args = SimpleNamespace(msfs=None, obs=False, ffmpeg="", picker=picker, port=0)
                try:
                    with patch.object(host, "__file__", str(self.folder / "host.py")), \
                         patch.object(host, "Foreground", return_value=ui), \
                         patch.object(host, "listen", side_effect=OSError("test listener closed")):
                        with self.assertRaisesRegex(OSError, "test listener closed"):
                            await host.run_listener(args, "127.0.0.1", b"", None, leaf)
                    self.assertEqual(ui.broker.services.system_picker, direct)
                    self.assertEqual(ui.root.iconify.call_count, int(direct))
                finally:
                    picker.unlink()

    async def asyncSetUp(self):
        self.folder = Path(tempfile.gettempdir()) / ("kvmpad-pipe-test-" + secrets.token_hex(12))
        self.folder.mkdir()
        self.addCleanup(self.folder.rmdir)
        self.board, self.key = identity(self.folder, "board")
        self.other, self.other_key = identity(self.folder, "other")
        for p in (self.board, self.key, self.other, self.other_key):
            self.addCleanup(p.unlink)
        self.der = ssl.PEM_cert_to_DER_cert(self.board.read_text(encoding="ascii"))
        self.request = {"board_address": "127.0.0.1", "board_der": base64.b64encode(self.der).decode("ascii")}

    async def child(self, request, port=0, fixture=True):
        env = dict(os.environ, TEMP=str(self.folder), TMP=str(self.folder))
        entry = [__file__, "--fixture", str(port)] if fixture else [
            str(Path(host.__file__)), "--paired-client", "--port", str(port)]
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        child = subprocess.Popen([sys.executable, "-B", *entry],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 env=env, startupinfo=startup, creationflags=subprocess.CREATE_NO_WINDOW)

        async def cleanup():
            if child.poll() is None:
                child.kill()
            await asyncio.to_thread(child.wait, 5)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()

        self.addAsyncCleanup(cleanup)
        child.stdin.write(json.dumps(request).encode("ascii") + b"\n")
        child.stdin.flush()
        return child

    async def start(self, fixture=True):
        child = await self.child(self.request, fixture=fixture)
        line = await asyncio.wait_for(asyncio.to_thread(child.stdout.readline), 12)
        self.assertTrue(line, child.stderr.read().decode() if not line else "")
        reply = json.loads(line)
        self.assertEqual(set(reply), {"port", "leaf"})
        self.assertTrue(0 < reply["port"] <= 65535)
        self.assertFalse(list(self.folder.glob("kvmpad-host-*.pem")))
        return child, reply["port"], base64.b64decode(reply["leaf"], validate=True)

    def client(self, leaf, other=False):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
        ctx.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(leaf))
        ctx.load_cert_chain(self.other if other else self.board, self.other_key if other else self.key)
        return ctx

    async def test_cli_listener_and_foreground_exit_with_client(self):
        child, port, leaf = await self.start(fixture=False)
        async with connect(f"wss://127.0.0.1:{port}/control", ssl=self.client(leaf), proxy=None) as control:
            await control.send(json.dumps(dict(type="HELLO", protocol="kvmbridge", version=1, profile=GAUGES)))
            self.assertEqual(json.loads(await control.recv())["type"], "SESSION")
            child.stdin.close()
            await asyncio.wait_for(control.wait_closed(), 5)
        self.assertEqual(await asyncio.to_thread(child.wait, 5), 0)
        self.assertEqual(child.stdout.read(), b"")

    async def test_pinned_listener_active_grant_and_parent_lifetime(self):
        previous = None
        for termination in ("eof", "extra byte"):
            child, port, leaf = await self.start()
            self.assertNotEqual(previous, leaf)
            addr = f"wss://127.0.0.1:{port}"
            ctx = self.client(leaf)
            for invalid in [self.client(leaf, other=True)] + ([self.client(previous)] if previous else []):
                with self.assertRaises((OSError, ConnectionClosed, InvalidHandshake)):
                    async with connect(addr + "/control", ssl=invalid, proxy=None, open_timeout=3) as wrong:
                        await wrong.recv()
            async with connect(addr + "/control", ssl=ctx, proxy=None) as control:
                actual = control.transport.get_extra_info("ssl_object").getpeercert(binary_form=True)
                self.assertEqual(actual, leaf)
                await control.send(json.dumps(dict(type="HELLO", protocol="kvmbridge", version=1, profile=GAUGES)))
                session = json.loads(await control.recv())
                await control.send(json.dumps(dict(type="SUBSCRIBE", seq=1, source="sim.xplane",
                                                   topics=[dict(name="sim.airspeed_kt", rate_hz=5)])))
                self.assertEqual(json.loads(await control.recv())["type"], "SUBSCRIBED")
                async with connect(addr + "/telemetry", ssl=ctx, proxy=None) as data:
                    await data.send(json.dumps(dict(type="BIND", session=session["session"])))
                    async with asyncio.timeout(2):
                        while (sample := json.loads(await data.recv()))["value"] is None:
                            pass
                    self.assertEqual(sample["value"], 123)
                    if termination == "eof":
                        child.stdin.close()
                    else:
                        child.stdin.write(b"x")
                        child.stdin.flush()
                    await asyncio.wait_for(asyncio.gather(data.wait_closed(), control.wait_closed()), 5)
            self.assertEqual(await asyncio.to_thread(child.wait, 5), 0)
            self.assertEqual(child.stdout.read(), b"")
            with self.assertRaises(OSError):
                await asyncio.open_connection("127.0.0.1", port)
            previous = leaf

    async def test_invalid_request_and_failed_bind_publish_nothing(self):
        with socket.socket() as blocker:
            blocker.bind(("127.0.0.1", 0))
            blocker.listen()
            for request in (self.request, {**self.request, "board_der": "!!!!"}):
                child = await self.child(request, blocker.getsockname()[1])
                self.assertNotEqual(await asyncio.to_thread(child.wait, 8), 0)
                self.assertEqual(child.stdout.read(), b"")
                self.assertNotIn(self.request["board_der"].encode(), child.stderr.read())
            self.assertFalse(list(self.folder.glob("kvmpad-host-*.pem")))

    def test_bound_strict_request(self):
        valid = json.dumps(self.request).encode()
        self.assertEqual(read_board(io.BytesIO(valid + b"\n"))[0], self.der)
        self.assertEqual(read_board(io.BytesIO(b"\xef\xbb\xbf" + valid + b"\r\n"))[0], self.der)
        for bad in (valid, valid + b" " * 2048 + b"\n", b"[]\n",
                    valid.replace(b"{", b'{"board_address":"127.0.0.1",', 1) + b"\n",
                    valid.replace(b"127.0.0.1", b"0.0.0.0") + b"\n",
                    valid.replace(b"127.0.0.1", b"127.0.0.1.example") + b"\n"):
            with self.assertRaises(ValueError):
                read_board(io.BytesIO(bad))

    def test_temporary_key_is_encrypted_and_removed_after_ssl_failure(self):
        _, address = read_board(io.BytesIO(json.dumps(self.request).encode() + b"\n"))
        files = []

        def reject_chain(context, name, *, password):
            path = Path(name)
            files.append(path)
            wire = path.read_bytes()
            self.assertIn(b"BEGIN ENCRYPTED PRIVATE KEY", wire)
            self.assertNotIn(b"BEGIN PRIVATE KEY", wire)
            self.assertNotIn(password.encode(), wire)
            raise ssl.SSLError("test load failure")

        with patch.object(ssl.SSLContext, "load_cert_chain", reject_chain), self.assertRaises(ssl.SSLError):
            listener_identity(self.der, address)
        self.assertEqual(len(files), 1)
        self.assertFalse(files[0].exists())


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        args = SimpleNamespace(port=int(sys.argv[2]), msfs=None, obs=False, picker=None, ffmpeg="ffmpeg")
        with patch.object(host, "Foreground", LocalUI), patch.object(host, "XPlane", LocalPlane):
            asyncio.run(host.main(args))
    else:
        unittest.main()
