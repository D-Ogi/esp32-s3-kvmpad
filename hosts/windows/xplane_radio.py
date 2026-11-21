# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""X-Plane local Web API v2 COM and transponder readback and control."""
import asyncio
from contextlib import closing
import http.client
import json
import threading

from adapters import ProviderResponseError


PREFIX = "sim/cockpit2/radios/actuators/"
ACTIVE = tuple(PREFIX + f"com{n}_frequency_hz_833" for n in (1, 2))
STANDBY = tuple(PREFIX + f"com{n}_standby_frequency_hz_833" for n in (1, 2))
SQUAWK = PREFIX + "transponder_code"
MODE = PREFIX + "transponder_mode"
REFS = (*ACTIVE, *STANDBY, SQUAWK, MODE)
# Laminar's Commands.txt spells both command names "standy".
FLIP = ("sim/radios/com1_standy_flip", "sim/radios/com2_standy_flip")


def valid_channel(khz):
    # Displayed 8.33/25 kHz channel designator, in integer kHz.
    return type(khz) is int and 118000 <= khz <= 136990 and khz % 5 == 0 and khz % 25 != 20


async def _local_operation(provider, operation):
    canceled = threading.Event()

    def run():
        with closing(http.client.HTTPConnection("127.0.0.1", provider.port, timeout=.5)) as conn:
            conn.connect()
            # Resource IDs are valid only for the instance that supplied them.
            conn.auto_open = False
            return operation(conn, canceled)

    try:
        return await asyncio.to_thread(run)
    finally:
        canceled.set()


async def radio_snapshot(provider):
    def read(conn, canceled):
        identifiers = provider.lookup(conn, "datarefs", REFS)
        if len(identifiers) != len(REFS):
            raise KeyError("X-Plane radio dataref unavailable")
        raw = {}
        for name in REFS:
            if canceled.is_set():
                raise InterruptedError("Radio read canceled")
            raw[name] = provider.request(conn, f"datarefs/{identifiers[name]}/value")
        if not all(valid_channel(raw[name]) for name in (*ACTIVE, *STANDBY)):
            raise ProviderResponseError("Invalid X-Plane COM channel")
        code = raw[SQUAWK]
        if type(code) is not int or not 0 <= code <= 7777 or any(c > "7" for c in str(code)):
            raise ProviderResponseError("Invalid X-Plane transponder code")
        mode = raw[MODE]
        if type(mode) is not int or mode not in (1, 2, 3):
            raise ProviderResponseError("Unrepresentable X-Plane transponder mode")
        return dict(active_khz=[raw[name] for name in ACTIVE],
                    standby_khz=[raw[name] for name in STANDBY],
                    squawk=int(str(code), 8), mode=mode - 1, mode_mask=7)

    return await _local_operation(provider, read)


async def radio_command(provider, action, com, value):
    if type(com) is not int or com not in (0, 1) or type(value) is not int:
        raise ValueError("Invalid radio control")
    if action == "tune" and valid_channel(value):
        name, data = STANDBY[com], value
    elif action == "squawk" and 0 <= value <= 0o7777:
        name, data = SQUAWK, int(format(value, "o"))
    elif action == "mode" and 0 <= value <= 2:
        name, data = MODE, value + 1
    elif action == "swap" and value == 0:
        name, data = FLIP[com], 0
    else:
        raise ValueError("Invalid radio control")

    def send(conn, canceled):
        swap = action == "swap"
        kind = "commands" if swap else "datarefs"
        ident = provider.lookup(conn, kind, [name])[name]
        if canceled.is_set() or conn.sock is None:
            raise InterruptedError("Radio control session ended")
        path = f"command/{ident}/activate" if swap else f"datarefs/{ident}/value"
        body = {"duration": 0} if swap else {"data": data}
        conn.request("POST" if swap else "PATCH", "/api/v2/" + path, json.dumps(body),
                     {"Content-Type": "application/json", "Accept": "application/json"})
        reply = conn.getresponse()
        if reply.status in (408, 502, 503, 504):
            raise OSError("X-Plane unavailable")
        if reply.status != 200 or reply.read(4097) not in (b"", b"null"):
            raise ProviderResponseError("X-Plane declined radio control")

    await _local_operation(provider, send)
