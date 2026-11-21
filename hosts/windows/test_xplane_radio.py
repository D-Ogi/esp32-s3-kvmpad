# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise the production XPlane radio adapter against a local Web API server."""
import asyncio
import http.client
import json
import unittest
from urllib.parse import parse_qs, urlsplit

from adapters import ProviderResponseError, XPlane


class RadioHTTP(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        prefix = "sim/cockpit2/radios/actuators/"
        self.refs = {prefix + name: n for n, name in enumerate((
            "com1_frequency_hz_833", "com2_frequency_hz_833", "com1_standby_frequency_hz_833",
            "com2_standby_frequency_hz_833", "transponder_code", "transponder_mode"), 11)}
        self.commands = {"sim/radios/com1_standy_flip": 95, "sim/radios/com2_standy_flip": 96}
        self.values = dict(zip(range(11, 17), (118000, 136990, 132010, 121500, 7000, 2)))
        self.trace = []
        self.writes = []
        self.fail = None
        self.connections = 0
        self.lookup_seen = asyncio.Event()
        self.release = asyncio.Event()
        self.closed = asyncio.Event()
        self.server = await asyncio.start_server(self.serve, "127.0.0.1", 0)
        self.provider = XPlane(port=self.server.sockets[0].getsockname()[1])

    async def asyncTearDown(self):
        self.release.set()
        self.server.close()
        await self.server.wait_closed()

    async def serve(self, reader, writer):
        self.connections += 1
        session = self.connections
        try:
            while line := await reader.readline():
                verb, target, _ = line.decode().split()
                headers = {}
                while (line := await reader.readline()).strip():
                    name, value = line.decode().split(":", 1)
                    headers[name.lower()] = value.strip()
                body = await reader.readexactly(int(headers.get("content-length", 0)))
                url = urlsplit(target)
                self.trace.append((session, verb, url.path))
                status, close, reply = 200, False, b""
                if url.query:
                    names = parse_qs(url.query)["filter[name]"]
                    resources = {"/api/v2/datarefs": self.refs, "/api/v2/commands": self.commands}[url.path]
                    rows = [{"name": n, "id": resources[n]} for n in reversed(names) if n in resources]
                    reply = json.dumps({"data": rows}).encode()
                    self.lookup_seen.set()
                    if self.fail == "hold-lookup":
                        await self.release.wait()
                    close = self.fail == "close-lookup"
                elif verb == "GET":
                    ident = int(url.path.split("/")[4])
                    if self.fail == "drop-read" and ident == 13:
                        return
                    reply = json.dumps({"data": self.values[ident]}).encode()
                else:
                    self.writes.append((verb, url.path, json.loads(body)))
                    if self.fail == "drop-reply":
                        return
                    if self.fail == "reject":
                        status = 403
                header = (f"HTTP/1.1 {status} Response\r\nContent-Length: {len(reply)}\r\n"
                          + ("Connection: close\r\n" if close else "") + "\r\n")
                writer.write(header.encode() + reply)
                await writer.drain()
                if close:
                    return
        finally:
            writer.close()
            await writer.wait_closed()
            self.closed.set()

    async def test_snapshot(self):
        for xp_mode, code, expected_code in ((1, 73, 0o0073), (2, 7000, 0o7000), (3, 7777, 4095)):
            self.values.update({15: code, 16: xp_mode})
            self.assertEqual(await self.provider.radio_snapshot(),
                             dict(active_khz=[118000, 136990], standby_khz=[132010, 121500],
                                  squawk=expected_code, mode=xp_mode - 1, mode_mask=7))
            self.assertEqual(len({session for session, _, _ in self.trace[-7:]}), 1)

    async def test_commands_submit_once_without_synthesizing_readback(self):
        for action, com, value in (("swap", 0, 0), ("swap", 1, 0), ("tune", 0, 118005),
                                   ("tune", 1, 136990), ("squawk", 0, 0o0073),
                                   ("squawk", 0, 0o7777), ("mode", 0, 0), ("mode", 1, 2)):
            self.assertIsNone(await self.provider.radio_command(action, com, value))
            self.assertEqual(self.trace[-2][0], self.trace[-1][0])
        self.assertEqual(self.writes, [
            ("POST", "/api/v2/command/95/activate", {"duration": 0}),
            ("POST", "/api/v2/command/96/activate", {"duration": 0}),
            ("PATCH", "/api/v2/datarefs/13/value", {"data": 118005}),
            ("PATCH", "/api/v2/datarefs/14/value", {"data": 136990}),
            ("PATCH", "/api/v2/datarefs/15/value", {"data": 73}),
            ("PATCH", "/api/v2/datarefs/15/value", {"data": 7777}),
            ("PATCH", "/api/v2/datarefs/16/value", {"data": 1}),
            ("PATCH", "/api/v2/datarefs/16/value", {"data": 3})])
        self.assertEqual((await self.provider.radio_snapshot())["squawk"], 0o7000)

    async def test_invalid_request_never_contacts_simulator(self):
        for args in (("tune", 0, 118020), ("tune", 0, 136995), ("tune", 1, 117995),
                     ("tune", 1, 118001), ("tune", 2, 118000), ("tune", False, 118000),
                     ("squawk", 0, 4096), ("squawk", 0, -1), ("mode", 1, 3),
                     ("mode", 1, True), ("swap", -1, 0), ("swap", 1, 1), ("other", 0, 1)):
            with self.subTest(args=args), self.assertRaises(ValueError):
                await self.provider.radio_command(*args)
        self.assertEqual(self.connections, 0)

    async def test_unrepresentable_state_and_missing_dataref(self):
        for ident, invalid in ((11, 118020), (13, 132010000), (15, 7281), (15, -1),
                               (15, 77770), (15, True), (16, 0), (16, 4), (16, 5), (16, 6),
                               (16, 7), (16, True)):
            previous, self.values[ident] = self.values[ident], invalid
            with self.subTest(ident=ident, value=invalid), self.assertRaises(ProviderResponseError):
                await self.provider.radio_snapshot()
            self.values[ident] = previous
        self.refs.pop("sim/cockpit2/radios/actuators/transponder_mode")
        with self.assertRaises(KeyError):
            await self.provider.radio_snapshot()

    async def test_disconnect_does_not_reuse_id_or_repeat_swap(self):
        self.fail = "close-lookup"
        with self.assertRaises(OSError):
            await self.provider.radio_command("swap", 1, 0)
        self.assertEqual(self.connections, 1)
        self.assertEqual(self.writes, [])
        self.fail = "drop-reply"
        self.commands["sim/radios/com2_standy_flip"] = 202
        with self.assertRaises((OSError, http.client.HTTPException)):
            await self.provider.radio_command("swap", 1, 0)
        self.assertEqual(self.connections, 2)
        self.assertEqual(self.writes, [("POST", "/api/v2/command/202/activate", {"duration": 0})])
        self.fail = "drop-read"
        with self.assertRaises((OSError, http.client.HTTPException)):
            await self.provider.radio_snapshot()
        self.assertEqual(self.connections, 3)

    async def test_cancel_during_lookup_and_provider_rejection(self):
        self.fail = "hold-lookup"
        send = asyncio.create_task(self.provider.radio_command("swap", 0, 0))
        await asyncio.wait_for(self.lookup_seen.wait(), 1)
        send.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await send
        self.release.set()
        await asyncio.wait_for(self.closed.wait(), 1)
        self.assertEqual(self.writes, [])
        self.fail = "reject"
        with self.assertRaises(ProviderResponseError):
            await self.provider.radio_command("tune", 0, 118025)
        self.assertEqual(len(self.writes), 1)


if __name__ == "__main__":
    unittest.main()
