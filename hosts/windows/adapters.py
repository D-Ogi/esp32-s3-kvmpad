# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Built-in simulator and OBS adapters. Device input selects only fixed names."""
import asyncio
import base64
import hashlib
import http.client
import json
import math
import secrets
import struct
import threading
import time
from contextlib import closing
from urllib.parse import urlencode

from websockets.asyncio.client import connect

from media import child, exchange, terminate


class LocalConnection(connect):
    # A loopback provider must never redirect credential or control traffic to another machine.
    def process_redirect(self, exc):
        return exc


TOPICS = {
    "sim.airspeed_kt": ("kt", "airspeed_kts_pilot"),
    "sim.altitude_ft": ("ft", "altitude_ft_pilot"),
    "sim.heading_deg": ("deg", "heading_AHARS_deg_mag_pilot"),
    "sim.vertical_speed_fpm": ("ft/min", "vvi_fpm_pilot"),
    "sim.attitude.pitch_deg": ("deg", "pitch_AHARS_deg_pilot"),
    "sim.attitude.roll_deg": ("deg", "roll_AHARS_deg_pilot"),
}
FLAPS = {"sim.flaps.up": "sim/flight_controls/flaps_up",
         "sim.flaps.down": "sim/flight_controls/flaps_down"}
OBS_ACTIONS = {"obs.record.start", "obs.record.stop", "obs.record.status",
               "obs.scene.select", "obs.input.mute", "obs.input.unmute"}


def subscriptions(entries):
    if type(entries) is not list or not 1 <= len(entries) <= 16:
        raise ValueError("Topic count")
    rates = {}
    for entry in entries:
        if type(entry) is not dict or set(entry) != {"name", "rate_hz"}:
            raise ValueError("Topic fields")
        name, rate = entry["name"], entry["rate_hz"]
        maximum = 20 if name in ("sim.attitude.pitch_deg", "sim.attitude.roll_deg") else 10
        if name not in TOPICS or name in rates or type(rate) is not int or not 1 <= rate <= maximum:
            raise ValueError("Topic or rate")
        rates[name] = rate
    return rates


# Provider events are private sentinels. Only the TELEMETRY quality and a JSON null cross the wire.
MISSING_TOPIC = object()
INVALID_SAMPLE = object()


class ProviderResponseError(ValueError):
    """The provider answered with an explicit error or a malformed successful response."""


class Telemetry:
    def __init__(self, source, rates, quality_v=False):
        self.quality_v = quality_v
        self.source = source
        self.rates = rates
        self.latest = {}
        self.next_due = dict.fromkeys(rates, 0)
        self.sequence = 0

    def update(self, values, now):
        for name, value in values.items():
            if name not in self.rates:
                continue
            if type(value) in (int, float):
                try:
                    value = float(value)
                except OverflowError:
                    value = INVALID_SAMPLE
                if value is not INVALID_SAMPLE:
                    if not math.isfinite(value):
                        value = INVALID_SAMPLE
                    elif name == "sim.heading_deg":
                        value %= 360
            elif value is not MISSING_TOPIC:
                value = INVALID_SAMPLE
            self.latest[name] = (value, now)

    def messages(self, now):
        for name, hz in self.rates.items():
            if now < self.next_due[name]:
                continue
            self.next_due[name] = now + 1 / hz
            value, sample = self.latest.get(name, (None, now))
            age = now - sample
            quality = "VALID"
            if value is None or value is MISSING_TOPIC or age >= 3:
                quality, value = "UNAVAILABLE", None
            elif value is INVALID_SAMPLE:
                quality, value = ("ERROR" if self.quality_v else "UNAVAILABLE"), None
            elif age >= 1:
                quality = "STALE"
            self.sequence += 1
            yield dict(type="TELEMETRY", seq=self.sequence, topic=name, value=value,
                       unit=TOPICS[name][0], ts_mono_ms=int(sample * 1000) & 0xffffffff,
                       age_ms=min(3000, max(0, int(age * 1000))), quality=quality, source=self.source)


class XPlane:
    def __init__(self, port=8086):
        self.port = port

    async def radio_snapshot(self):
        from xplane_radio import radio_snapshot
        return await radio_snapshot(self)

    async def radio_command(self, action, com, value):
        from xplane_radio import radio_command
        await radio_command(self, action, com, value)

    def request(self, connection, path):
        connection.request("GET", "/api/v2/" + path, headers={"Accept": "application/json"})
        response = connection.getresponse()
        body = response.read(8193)
        if response.status in (404, 410):
            raise KeyError("Unknown X-Plane resource")
        if response.status in (408, 502, 503, 504):
            raise OSError("X-Plane unavailable")
        if response.status != 200 or len(body) > 8192:
            raise ProviderResponseError("Invalid X-Plane response")
        try:
            return json.loads(body)["data"]
        except (ValueError, TypeError, KeyError, RecursionError) as exc:
            raise ProviderResponseError("Invalid X-Plane response") from exc

    def lookup(self, connection, kind, names):
        query = urlencode([("filter[name]", value) for value in names])
        rows = self.request(connection, kind + "?" + query)
        if type(rows) is not list or len(rows) > len(names):
            raise ProviderResponseError("Invalid X-Plane lookup")
        found = {}
        for row in rows:
            if (type(row) is not dict or type(row.get("name")) is not str or row["name"] not in names
                    or row["name"] in found or type(row.get("id")) is not int or row["id"] < 0):
                raise ProviderResponseError("Invalid X-Plane identifier")
            found[row["name"]] = row["id"]
        return found

    def read(self, names):
        # IDs belong to one simulator instance. Resolve and read on the same TCP connection.
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=.4)
        try:
            connection.connect()
            connection.auto_open = False
            refs = {name: "sim/cockpit2/gauges/indicators/" + TOPICS[name][1] for name in names}
            found = self.lookup(connection, "datarefs", refs.values())
            return {name: self.request(connection, f"datarefs/{found[ref]}/value")
                    for name, ref in refs.items() if ref in found}
        finally:
            connection.close()

    def watch(self, name, rate, stop, publish):
        # A dataref ID is scoped to one simulator instance. Each subscribed gauge keeps
        # its own TCP connection, resolves its ID once and discards it on any error.
        ref = "sim/cockpit2/gauges/indicators/" + TOPICS[name][1]
        period = 1 / rate
        while not stop.is_set():
            try:
                with closing(http.client.HTTPConnection("127.0.0.1",self.port,timeout=.4)) as conn:
                    conn.connect()
                    conn.auto_open = False
                    idx = self.lookup(conn,"datarefs",[ref])[ref]
                    while not stop.is_set():
                        start = time.monotonic()
                        value = self.request(conn,f"datarefs/{idx}/value")
                        if not stop.is_set():
                            publish(name,value,time.monotonic())
                        stop.wait(max(0, start + period - time.monotonic()))
            except (KeyError, ProviderResponseError) as exc:
                if not stop.is_set():
                    publish(name, MISSING_TOPIC if isinstance(exc, KeyError) else INVALID_SAMPLE, time.monotonic())
            except (OSError, http.client.HTTPException):
                pass  # Without a provider response, age the previous observation.
            stop.wait(.5)

    async def samples(self, rates, deliver):
        stop = threading.Event()
        loop = asyncio.get_running_loop()
        def update(name, value, sampled):
            if not stop.is_set():
                deliver({name:value},sampled)
        def publish(name,value,sampled):
            loop.call_soon_threadsafe(update,name,value,sampled)
        workers = [loop.run_in_executor(None,self.watch,name,rate,stop,publish) for name,rate in rates.items()]
        finished = asyncio.gather(*workers,return_exceptions=True)
        try:
            await asyncio.shield(finished)
        finally:
            stop.set()
            await finished

    async def action(self, name):
        command = FLAPS[name]
        canceled = threading.Event()
        def activate():
            c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=.5)
            try:
                c.connect()
                c.auto_open = False
                command_id = self.lookup(c, "commands", [command])[command]
                if canceled.is_set() or c.sock is None:
                    raise PermissionError("Simulator session changed")
                # Keep the same TCP connection. A restarted simulator can reuse a numeric ID.
                c.request("POST", f"/api/v2/command/{command_id}/activate", '{"duration":0}',
                          {"Content-Type":"application/json", "Accept":"application/json"})
                response = c.getresponse()
                if response.status != 200 or len(response.read(4097)) > 4096:
                    raise ValueError("Simulator declined command")
            finally:
                c.close()
        try:
            await asyncio.to_thread(activate)
        finally:
            canceled.set()
        return {"status": "UNVERIFIED"}


class MSFS:
    def __init__(self, helper):
        self.helper = helper

    async def open(self):
        if not self.helper:
            raise OSError("Install the SimConnect helper")
        return child(self.helper)

    async def samples(self, rates, deliver):
        # Reopen only the read helper for this granted subscription. A new helper
        # reconnects to SimConnect and defines the six variables in the new instance.
        names = tuple(name for name in TOPICS if name in rates)
        mask = sum(1 << i for i,name in enumerate(TOPICS) if name in rates)
        sample = struct.Struct(f"<{len(names)}d")
        period = 1 / max(rates.values())
        while True:
            process = None
            try:
                process = await self.open()
                if len(names) < len(TOPICS):
                    select = bytes((ord("S"),mask))
                    ack = await asyncio.wait_for(asyncio.to_thread(exchange,process,select,2),1.5)
                    if ack != select:
                        raise ValueError("Simulator helper declined the requested topics")
                while True:
                    start = time.monotonic()
                    data = await asyncio.wait_for(asyncio.to_thread(exchange, process, b"Q", sample.size), 1.5)
                    deliver(dict(zip(names, sample.unpack(data))), time.monotonic())
                    await asyncio.sleep(max(0, start + period - time.monotonic()))
            except (OSError, EOFError, TimeoutError):
                pass
            finally:
                if process is not None:
                    await terminate(process)
            await asyncio.sleep(.5)

    async def action(self, name):
        command = {"sim.flaps.down": b"+", "sim.flaps.up": b"-"}[name]
        process = await self.open()
        try:
            if await asyncio.to_thread(exchange, process, command, 1) != b"A":
                raise ValueError("Simulator declined command")
            return {"status": "UNVERIFIED"}
        finally:
            await terminate(process)


def obs_auth(password, salt, challenge):
    def digest(value):
        return base64.b64encode(hashlib.sha256(value.encode("utf-8")).digest()).decode("ascii")
    return digest(digest(password + salt) + challenge)


class OBS:
    def __init__(self, password, scene=None, input_name=None, port=4455):
        self.password, self.scene, self.input_name, self.port = password, scene, input_name, port

    async def action(self, name):
        if name not in OBS_ACTIONS or not self.password:
            raise PermissionError("OBS unavailable")
        async with LocalConnection(f"ws://127.0.0.1:{self.port}", proxy=None,
                subprotocols=["obswebsocket.json"], max_size=8192, open_timeout=.6, close_timeout=.2) as ws:
            hello = json.loads(await ws.recv())
            if hello.get("op") != 0 or hello["d"]["rpcVersion"] < 1:
                raise ValueError("OBS hello")
            challenge = hello["d"]["authentication"]
            authentication = obs_auth(self.password, challenge["salt"], challenge["challenge"])
            await ws.send(json.dumps({"op":1,"d":{"rpcVersion":1, "eventSubscriptions":0,
                                                  "authentication":authentication}}))
            identified = json.loads(await ws.recv())
            if identified.get("op") != 2 or identified["d"].get("negotiatedRpcVersion") != 1:
                raise PermissionError("OBS authentication")

            async def rpc(method, **params):
                request = secrets.token_hex(8)
                await ws.send(json.dumps({"op":6,"d":{"requestType":method,"requestId":request,
                                                     "requestData":params}}))
                response = json.loads(await ws.recv())
                data = response["d"]
                if (response.get("op") != 7 or data.get("requestId") != request or
                        data.get("requestType") != method or data["requestStatus"].get("result") is not True):
                    raise ValueError("OBS request failed")
                return data.get("responseData", {})

            if name.startswith("obs.record."):
                if name != "obs.record.status":
                    await rpc("StartRecord" if name.endswith(".start") else "StopRecord")
                state = (await rpc("GetRecordStatus"))["outputActive"]
                if type(state) is not bool:
                    raise ValueError("Invalid recording state")
                return {"status":"SUCCESS", "recording":state}
            if name == "obs.scene.select" and self.scene:
                await rpc("SetCurrentProgramScene", sceneName=self.scene)
                state = await rpc("GetCurrentProgramScene")
                return {"status":"SUCCESS", "selected":state["currentProgramSceneName"] == self.scene}
            if name in {"obs.input.mute", "obs.input.unmute"} and self.input_name:
                await rpc("SetInputMute", inputName=self.input_name, inputMuted=name.endswith(".mute"))
                state = (await rpc("GetInputMute", inputName=self.input_name))["inputMuted"]
                if type(state) is not bool:
                    raise ValueError("Invalid mute state")
                return {"status":"SUCCESS", "muted":state}
            raise PermissionError("Configure a local OBS scene or input")
