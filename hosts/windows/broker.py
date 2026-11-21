# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""kvmbridge v1. One paired board, current user and active profile per process."""
import asyncio
from collections import deque
import contextlib
import hashlib
import hmac
import json
import math
import re
import secrets
import ssl
import time
from dataclasses import dataclass

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from adapters import FLAPS, INVALID_SAMPLE, OBS_ACTIONS, Telemetry, subscriptions
from media import CaptureError, CURSOR_ENTRY_TIMEOUT, FORMATS, MODES, PICKER_TIMEOUT, FrameRate, capture_review, negotiate, source_crop

MAG = "org.kvmpad.utility.cursor-magnifier"
GAUGES = "org.kvmpad.flight.instruments"
STUDIO = "org.kvmpad.studio.obs"
SIM_CONTROL = "org.kvmpad.studio.sim"
CAPTURE_SCOPE = "host.screen.region"
TELEMETRY_SCOPE = "host.telemetry.read"
DISPLAY_PICKER = "Choose an entire monitor in the Windows picker."
CURSOR_WAIT = "Monitor selected. Move the pointer onto that monitor and keep it there."
DISPLAY_REVIEW = "Display selected. Tap Allow display on KVMpad."
IMAGE_OPEN = "Image channel open. Preparing the first frame."
IMAGE_SENT = "First image sent. Waiting for KVMpad to display it."
IMAGE_SHARING = "Sharing a display"
BIND_TIMEOUT = 45
FRAME_ACK_TIMEOUT = 3
APPROVAL_TIMEOUT = 45
TELEMETRY_START_TIMEOUT = 95
# Pointer entry can begin at the end of the Windows picker interval.
CAPTURE_START_TIMEOUT = APPROVAL_TIMEOUT + PICKER_TIMEOUT + CURSOR_ENTRY_TIMEOUT + 5
LEGACY_SCOPES = {MAG: CAPTURE_SCOPE, GAUGES: TELEMETRY_SCOPE, STUDIO: "host.actions", SIM_CONTROL: "host.actions"}
PROFILE_ID = re.compile(r"[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*){2,}")


def unique(pairs):
    obj = dict(pairs)
    if len(obj) != len(pairs):
        raise ValueError("Duplicate field")
    return obj


def fields(msg, *names):
    if type(msg) is not dict or set(msg) != set(names):
        raise ValueError("Unexpected fields")


async def receive(ws, idle):
    # Allow an idle control connection, but never a slowly growing fragmented request.
    fragments = ws.recv_streaming()
    chunks = [await asyncio.wait_for(anext(fragments), idle)]
    async with asyncio.timeout(2):
        async for chunk in fragments:
            if len(chunks) >= 64:
                raise ValueError("Too many fragments")
            chunks.append(chunk)
    if not all(type(chunk) is str for chunk in chunks):
        raise ValueError("Expected text")
    data = "".join(chunks)
    if len(data.encode()) > 4096:
        raise ValueError("Message too large")
    msg = json.loads(data, object_pairs_hook=unique)
    if type(msg) is not dict:
        raise ValueError("Expected object")
    return msg


async def send(ws, **msg):
    await asyncio.wait_for(ws.send(json.dumps(msg, allow_nan=False, separators=(",", ":"))), 1)


@dataclass
class Services:
    approve: object
    status: object
    capture: object
    providers: dict
    codecs: tuple = tuple(FORMATS)
    system_picker: bool = False


class Session:
    def __init__(self, ws, profile, services, requested_scope=None, *, composite=False, status_changed=None):
        self.ws = ws
        self.profile = profile
        self.scope = LEGACY_SCOPES[profile] if requested_scope is None else requested_scope
        self.scoped_hello = requested_scope is not None
        self.composite = composite
        self.status_changed = status_changed
        self.status_text = ""
        self.services = services
        self.token = secrets.token_hex(16)
        self.sequence = 0
        self.active = True
        self.channel = None
        self.kind = None
        self.capture = None
        self.awaiting_pointer = False
        self.progress_v = False
        self.pointer_label_v = False
        self.frozen = False
        self.resuming = False
        self.crop_v = False
        self.last_image_seq = -1
        self.present_v = False
        self.rate = FrameRate()
        self.outstanding = self.retired = None
        self.credit = asyncio.Event()
        self.frame_started = 0
        self.presentations = deque(maxlen=32)
        self.unshown_frames = 0
        self.last_rate_update = 0
        self.image_failure = "Windows could not prepare an image from the selected display."
        self.failure_note = None
        self.closing = None
        self.displayed = False
        self.frame_lock = asyncio.Lock()
        self.operation = None
        self.producer = None
        self.picker_watch = None
        self.deadline = asyncio.get_running_loop().call_later(3600, self.stop, "The one-hour HOST grant expired.")
        self.start_timeout = None
        self.bind_timeout = None
        self.last_command = 0

    def set_status(self, text):
        self.status_text = text
        if self.status_changed:
            self.status_changed(self)
        else:
            self.services.status(text or "Stopped")

    def stop(self, reason="Stopped"):
        if not self.active:
            return
        self.active = False
        self.outstanding = self.retired = None
        self.credit.set()
        self.token = None
        self.deadline.cancel()
        if self.start_timeout:
            self.start_timeout.cancel()
        if self.bind_timeout:
            self.bind_timeout.cancel()
        if self.capture:
            self.capture.revoke()
        for task in (self.operation, self.producer, self.picker_watch):
            if task and task is not asyncio.current_task():
                task.cancel()
        sockets = [ws for ws in (self.channel, self.ws) if ws]
        close_code = None
        if self.kind == "/image" and isinstance(reason, CaptureError):
            close_code = {"cursor": 4001, "frame_timeout": 4002, "geometry": 4003,
                          "topology": 4003, "cursorpos": 4004}.get(reason.stage)
        if close_code:
            # Either authenticated socket may be the next one the board reads.
            self.closing = asyncio.gather(*(ws.close(code=close_code) for ws in sockets), return_exceptions=True)
        else:
            for ws in sockets:
                ws.transport.abort()
        self.set_status("" if reason == "Stopped" else str(reason))

    def board_end_reason(self):
        if self.failure_note:
            return self.failure_note
        if self.scope == CAPTURE_SCOPE and self.kind == "/image" and not self.channel:
            return "KVMpad ended the request before opening the image channel."
        if self.scope == CAPTURE_SCOPE and self.capture and not self.kind:
            if self.awaiting_pointer:
                return "KVMpad ended the request while waiting for the pointer to enter the selected monitor."
            return "KVMpad ended the request during display selection."
        if self.scope == CAPTURE_SCOPE and self.channel and not self.displayed:
            return "KVMpad ended the image session before the first display confirmation."
        return "KVMpad ended the session."

    async def finish(self):
        self.stop(self.board_end_reason())
        tasks = [t for t in (self.operation, self.producer, self.picker_watch) if t and t is not asyncio.current_task()]
        await asyncio.gather(*tasks, return_exceptions=True)
        if self.closing:
            await self.closing
        if self.capture:
            await self.capture.close()

    async def control(self):
        features = {"crop_v": 1, "present_v": 1, "progress_v": 1, "pointer_label_v": 1} if self.scope == CAPTURE_SCOPE else {}
        if self.scope == TELEMETRY_SCOPE:
            features["quality_v"] = 1
        if self.scoped_hello:
            features["scope"] = self.scope
        if self.composite:
            features["composite_v"] = 1
        await send(self.ws, type="SESSION", session=self.token, version=1, **features)
        idle = 100
        while self.active:
            msg = await receive(self.ws, idle)
            if type(msg.get("seq")) is not int or msg["seq"] != self.sequence + 1:
                raise ValueError("Control sequence")
            self.sequence = msg["seq"]
            if msg.get("type") == "STOP":
                fields(msg, "type", "seq")
                self.stop(self.board_end_reason())
                return
            if self.operation and not self.operation.done():
                raise ValueError("Operation in progress")
            now = time.monotonic()
            if now - self.last_command < .1:
                raise ValueError("Command rate limit")
            self.last_command = now
            if not self.kind and (self.scope, msg.get("type")) in (
                    (CAPTURE_SCOPE, "START"), (TELEMETRY_SCOPE, "SUBSCRIBE")):
                # Frames and their credits travel on a different socket. Retire the initial control
                # wait before yielding to approval or the picker, and bound startup separately.
                idle = 3600
                timeout = CAPTURE_START_TIMEOUT if self.scope == CAPTURE_SCOPE else TELEMETRY_START_TIMEOUT
                self.start_timeout = asyncio.get_running_loop().call_later(
                    timeout, self.stop, "The HOST request timed out before the data channel was ready.")
            self.operation = asyncio.create_task(self.operate(msg))

    async def operate(self, msg):
        failure = "The HOST could not complete the request."
        try:
            op = msg.get("type")
            if op == "START" and self.scope == CAPTURE_SCOPE and not self.kind:
                failure = "The requested capture settings are unavailable on this HOST."
                optional = ()
                if "progress_v" in msg:
                    if type(msg["progress_v"]) is not int or msg["progress_v"] != 1:
                        raise ValueError("Unsupported picker progress version")
                    self.progress_v = True
                    optional = ("progress_v",)
                if "pointer_label_v" in msg:
                    if not self.progress_v or type(msg["pointer_label_v"]) is not int or msg["pointer_label_v"] != 1:
                        raise ValueError("Unsupported pointer display label")
                    self.pointer_label_v = True
                    optional += ("pointer_label_v",)
                if "crop_v" in msg:
                    extra = ()
                    if "present_v" in msg:
                        extra = ("present_v", "max_fps")
                        fps = msg.get("max_fps")
                        if (type(msg["present_v"]) is not int or msg["present_v"] != 1 or
                                type(fps) is not int or not 1 <= fps <= 10):
                            raise ValueError("Unsupported presentation feedback")
                        self.present_v = True
                        self.rate = FrameRate(fps)
                    fields(msg, "type", "seq", "formats", "mode", "crop_v", "source_crop", *extra, *optional)
                    if type(msg["crop_v"]) is not int or msg["crop_v"] != 1:
                        raise ValueError("Unsupported crop metadata version")
                    self.crop_v = True
                else:
                    fields(msg, "type", "seq", "formats", "mode", *optional)
                candidates = msg["formats"]
                if type(candidates) is not list or not 1 <= len(candidates) <= 3 or any(
                        type(c) is not str for c in candidates) or msg["mode"] not in MODES:
                    raise ValueError("Unsupported format or mode")
                codec = negotiate(candidates, self.services.codecs)
                if not codec:
                    raise ValueError("No supported format")
                params = {}
                if self.crop_v:
                    requested = msg["source_crop"]
                    if (len(candidates)!=1 or type(requested) is not list or
                            any(type(v) is not int for v in requested) or requested!=source_crop(codec,msg["mode"])):
                        raise ValueError("Invalid source crop")
                    params = {"crop_v":1, "source_crop":requested}
                    if self.present_v:
                        params.update(present_v=1, max_fps=self.rate.maximum, start_fps=self.rate.fps)
                if self.services.system_picker and self.profile == MAG:
                    self.set_status(capture_review(codec, msg["mode"], system_picker=True))
                else:
                    self.set_status("Waiting for Allow once in this HOST window.")
                    failure = "Display sharing was declined on this PC."
                    try:
                        await self.consent(capture_review(codec, msg["mode"]))
                    except TimeoutError as exc:
                        raise CaptureError("approval_timeout") from exc
                    self.set_status(DISPLAY_PICKER)
                if self.progress_v:
                    await send(self.ws, type="START_PROGRESS", seq=msg["seq"], stage="picker")
                failure = "Windows could not use the selected display. Choose an entire display in the picker."
                self.capture = self.services.capture()
                try:
                    resource = await self.capture.open(codec, msg["mode"], self.wait_for_pointer)
                except TimeoutError as exc:
                    raise CaptureError("selection_timeout") from exc
                async def monitor_picker():
                    reason = "Windows closed the capture source."
                    try:
                        await self.capture.wait_closed()
                    except CaptureError as issue:
                        reason = issue
                    finally:
                        if self.active:
                            self.stop(reason)
                self.picker_watch = asyncio.create_task(monitor_picker())
                self.stream = secrets.randbelow(0xffffffff) + 1
                self.ready("/image")
                failure = "The board did not receive the display selection."
                await send(self.ws, type="STARTED", stream=self.stream, codec=codec,
                           mode=msg["mode"], resource=resource, **params)
            elif op in ("FREEZE", "RESUME") and self.kind == "/image" and self.channel:
                fields(msg, "type", "seq")
                if (op == "FREEZE") == self.frozen:
                    raise ValueError("Invalid freeze transition")
                if op == "FREEZE":
                    self.frozen = True  # Discard any frame still being produced.
                async with self.frame_lock:
                    if op == "FREEZE":
                        await self.capture.pause()
                        if self.outstanding is not None:
                            self.retired, self.outstanding = self.outstanding, None
                            self.credit.set()
                    else:
                        await self.capture.require_selected()
                        self.frozen = False
                    self.rate.reset()
                    self.presentations.clear()
                    self.unshown_frames = 0
                    self.last_rate_update = 0
                    self.resuming = not self.frozen
                    self.set_status("Capture frozen. KVMpad has paused new frames." if self.frozen else
                                    "Resuming capture. Waiting for a fresh image.")
                fence = {"last_frame": self.last_image_seq} if self.crop_v and self.frozen else {}
                await send(self.ws, type="FROZEN" if self.frozen else "RESUMED", seq=msg["seq"], **fence)
            elif op == "SUBSCRIBE" and self.scope == TELEMETRY_SCOPE and not self.kind:
                quality_v = "quality_v" in msg
                extra = {"quality_v": 1} if quality_v else {}
                fields(msg, "type", "seq", "source", "topics", *extra)
                if quality_v and (type(msg["quality_v"]) is not int or msg["quality_v"] != 1):
                    raise ValueError("Unsupported telemetry quality version")
                self.source = msg["source"]
                if self.source not in ("sim.xplane", "sim.msfs2024") or self.source not in self.services.providers:
                    raise ValueError("Provider unavailable")
                rates = subscriptions(msg["topics"])
                failure = "Instrument sharing was declined or the approval request timed out."
                await self.consent(f"Read {', '.join(rates)} from {self.source}")
                self.telemetry = Telemetry(self.source, rates, quality_v)
                self.ready("/telemetry")
                await send(self.ws, type="SUBSCRIBED", source=self.source, **extra)
            elif op == "ACTION" and self.scope == "host.actions" and self.profile in (STUDIO, SIM_CONTROL):
                fields(msg, "type", "seq", "action", "source")
                source, name = msg["source"], msg["action"]
                allowed = ((self.profile == STUDIO and source == "obs" and name in OBS_ACTIONS) or
                           (self.profile == SIM_CONTROL and source in ("sim.xplane", "sim.msfs2024") and name in FLAPS))
                if not allowed or source not in self.services.providers:
                    raise PermissionError("Action unavailable")
                await self.consent(f"Allow {name} on {source}")
                await send(self.ws, type="ACCEPTED", seq=msg["seq"])
                try:
                    result = await asyncio.wait_for(self.services.providers[source].action(name), 2)
                except (ConnectionClosed, OSError, TimeoutError, ValueError, PermissionError,
                        TypeError, KeyError, asyncio.IncompleteReadError):
                    result = {"status":"UNAVAILABLE"}
                await send(self.ws, type="RESULT", seq=msg["seq"], **result)
            else:
                raise PermissionError("Unavailable for this profile")
        except asyncio.CancelledError:
            raise
        except Exception as issue:
            if isinstance(issue, CaptureError):
                if self.kind == "/image":
                    # The control request can discover a picker exit before its watcher.
                    self.stop(issue)
                    return
                failure = str(issue)
            self.failure_note = failure
            details = ({"reason": issue.stage} if op == "START" and self.scope == CAPTURE_SCOPE
                       and isinstance(issue, CaptureError) and issue.stage in
                       ("approval_timeout", "selection_timeout", "cancel", "cursorwait", "cursor", "pickedwindow",
                        "geometry", "topology", "cursorpos") else {})
            with contextlib.suppress(Exception):
                await send(self.ws, type="DENIED", seq=msg["seq"], **details)
            self.stop(failure)

    async def wait_for_pointer(self, resource=None):
        if self.active:
            self.awaiting_pointer = True
            display = resource.removeprefix("\\\\.\\") if resource else None
            self.set_status(f"Move the pointer onto {display} and keep it there." if display else CURSOR_WAIT)
            if self.progress_v:
                label = {"resource": resource} if self.pointer_label_v and resource else {}
                await send(self.ws, type="START_PROGRESS", seq=self.sequence, stage="pointer", **label)

    def ready(self, kind):
        if self.start_timeout:
            self.start_timeout.cancel()
        self.kind = kind
        reason = ("KVMpad did not open the image channel after display selection." if kind == "/image"
                  else "KVMpad did not open the instrument channel.")
        self.bind_timeout = asyncio.get_running_loop().call_later(BIND_TIMEOUT, self.stop, reason)
        self.set_status(DISPLAY_REVIEW if kind == "/image" else "Sharing instruments")

    async def consent(self, purpose):
        if not await asyncio.wait_for(self.services.approve(self.profile, purpose), APPROVAL_TIMEOUT):
            raise PermissionError("Denied")
        if not self.active:
            raise PermissionError("Revoked")

    def observe_frame(self, started, shown):
        old = self.rate.fps
        self.rate.observe(time.monotonic() - started, shown)
        if not self.present_v and self.displayed and not self.resuming and old != self.rate.fps:
            self.set_status(f"Sharing a display - target {self.rate.fps} fps")

    def record_presentation(self, now):
        self.presentations.append((now, now - self.frame_started))
        if len(self.presentations) < 2 or now - self.last_rate_update < 2:
            return
        elapsed = now - self.presentations[0][0]
        if elapsed <= 0:
            return
        self.last_rate_update = now
        fps = (len(self.presentations) - 1) / elapsed
        text = f"Sharing a display · {fps:.1f} fps confirmed by KVMpad (target {self.rate.fps})"
        if len(self.presentations) >= 20:
            durations = sorted(duration for _, duration in self.presentations)
            p95 = durations[math.ceil(.95 * len(durations)) - 1]
            text += f" · request to LCD acknowledgement p95 {p95 * 1000:.0f} ms"
        self.set_status(text)

    async def credits(self, ws):
        while self.active:
            msg = await receive(ws, 3600)
            fields(msg, "type", "stream", "frame", "shown")
            seq = msg["frame"]
            if (msg["type"] != "FRAME_DONE" or type(msg["stream"]) is not int or msg["stream"] != self.stream or
                    type(seq) is not int or not 0 <= seq <= 0xffffffff or type(msg["shown"]) is not bool):
                raise ValueError("Invalid presentation feedback")
            if seq == self.retired:
                self.retired = None
                continue
            if seq != self.outstanding:
                raise ValueError("Unexpected presentation sequence")
            self.outstanding = None
            if not self.frozen:
                self.observe_frame(self.frame_started, msg["shown"])
                if msg["shown"]:
                    if not self.presentations:
                        self.displayed = True
                        self.resuming = False
                        self.set_status(IMAGE_SHARING)
                    self.unshown_frames = 0
                    self.record_presentation(time.monotonic())
                else:
                    self.unshown_frames += 1
                    if self.unshown_frames == 1 and not self.presentations:
                        self.set_status("KVMpad received an image but could not display it.")
                    elif self.unshown_frames == 2 and self.presentations:
                        # An isolated dropped frame is normal. Restart the measured rate after a run of drops.
                        self.presentations.clear()
                        self.last_rate_update = 0
                        self.set_status("Waiting for KVMpad to display a new image.")
            self.credit.set()

    async def produce(self):
        if self.kind == "/image":
            sequence = 0
            drops = 0
            while self.active:
                start = time.monotonic()
                fresh_until = start + 1
                if self.frozen:
                    await asyncio.sleep(.05)
                    continue
                async with self.frame_lock:
                    self.image_failure = "Windows could not prepare an image from the selected display."
                    if self.crop_v:
                        frame, record = await self.capture.frame_with_crop(self.stream, sequence)
                    else:
                        frame, record = await self.capture.frame(self.stream, sequence), None
                    if self.frozen or not self.active:
                        continue
                    # A stale capture should never be queued after a delayed encoder.
                    if frame is not None and time.monotonic() <= fresh_until:
                        self.image_failure = "The HOST could not send the image to KVMpad."
                        if self.crop_v:
                            if not isinstance(record,str) or len(record)!=96:
                                raise ValueError("Missing cursor metadata")
                            await send(self.channel,type="CROP1",data=record)
                            if not self.active:
                                return
                            if time.monotonic() > fresh_until:
                                # The board is waiting for this exact image after CROP1.
                                self.image_failure = "The image expired while sending cursor metadata to KVMpad."
                                raise TimeoutError(self.image_failure)
                        if self.present_v:
                            self.credit.clear()
                            self.outstanding, self.frame_started = sequence, start
                        await asyncio.wait_for(self.channel.send(frame), .8)
                        if self.present_v:
                            if self.last_image_seq == -1 and self.outstanding is not None and not self.resuming:
                                self.set_status(IMAGE_SENT)
                        else:
                            if not self.displayed or self.resuming:
                                self.displayed = True
                                self.resuming = False
                                self.set_status(IMAGE_SHARING)
                            self.observe_frame(start, True)
                        self.last_image_seq = sequence
                        sequence = (sequence + 1) & 0xffffffff
                        drops = 0
                    else:
                        self.observe_frame(start, False)
                        drops += 1
                        if drops >= 6:
                            self.image_failure = "Windows could not provide a fresh image in six attempts."
                            raise ValueError("No usable fresh frame")
                if self.outstanding is not None:
                    self.image_failure = "KVMpad did not acknowledge the last image in time."
                    await asyncio.wait_for(self.credit.wait(), FRAME_ACK_TIMEOUT)
                await asyncio.sleep(max(0, 1 / self.rate.fps - (time.monotonic() - start)))
        else:
            async def collect():
                try:
                    await self.services.providers[self.source].samples(self.telemetry.rates, self.telemetry.update)
                except ValueError:
                    self.telemetry.update(dict.fromkeys(self.telemetry.rates, INVALID_SAMPLE), time.monotonic())
                except (OSError, EOFError):
                    pass  # Age the previous observation when the source disappears.
            collector = asyncio.create_task(collect())
            try:
                while self.active:
                    for msg in self.telemetry.messages(time.monotonic()):
                        await send(self.channel, **msg)
                    await asyncio.sleep(.01)
            finally:
                collector.cancel()
                await asyncio.gather(collector, return_exceptions=True)

    async def bind(self, ws):
        self.channel = ws
        self.bind_timeout.cancel()
        if self.kind == "/image":
            self.set_status(IMAGE_OPEN)
        self.producer = asyncio.create_task(self.produce())
        # Original sessions are one-way. A negotiated receiver may return one completion per image.
        monitor = asyncio.create_task(self.credits(ws) if self.present_v else ws.recv())
        reason = "The board closed the data connection."
        try:
            done, _ = await asyncio.wait((self.producer, monitor), return_when=asyncio.FIRST_COMPLETED)
            if self.kind == "/image" and self.producer in done and not self.producer.cancelled():
                issue = self.producer.exception()
                reason = issue if isinstance(issue, CaptureError) else self.image_failure
            elif self.present_v and monitor in done and not monitor.cancelled():
                issue = monitor.exception()
                if issue and not isinstance(issue, (ConnectionClosed, OSError)):
                    reason = "KVMpad sent invalid image feedback."
        finally:
            self.stop(reason)
            monitor.cancel()
            await asyncio.gather(monitor, self.producer, return_exceptions=True)
            if self.closing:
                await self.closing


class Broker:
    def __init__(self, pin, services):
        self.pin = pin
        self.services = services
        self.sessions = {}
        self.last_end = {}

    @property
    def session(self):
        """The first reserved session, for existing single-session observers."""
        return next(iter(self.sessions.values()), None)

    def publish_status(self, session):
        if self.sessions.get(session.scope) is not session:
            return
        if not session.active:
            if session.status_text:
                self.last_end[session.scope] = session.status_text
            # stop() has revoked the token, source and sockets. Unblocking a new grant must not
            # depend on how long Windows takes to reap the old picker process.
            del self.sessions[session.scope]
        active = [s.status_text for s in self.sessions.values() if s.status_text]
        self.services.status("\n".join([*active, *self.last_end.values()]) or "Stopped")

    def stop(self):
        had_session = any(s.active for s in self.sessions.values())
        for session in tuple(self.sessions.values()):
            session.stop()
        if had_session:
            self.last_end.clear()
            self.services.status("Stopped")

    async def handler(self, ws):
        try:
            tls = ws.transport.get_extra_info("ssl_object")
            cert = tls.getpeercert(binary_form=True) if tls else None
            if not cert or not hmac.compare_digest(hashlib.sha256(cert).digest(), self.pin):
                raise PermissionError("Unknown board")
            path = ws.request.path
            msg = await receive(ws, 5)
            if path == "/control":
                scoped = "scope" in msg
                composite = "composite_v" in msg
                expected = ("type", "protocol", "version", "profile")
                if scoped:
                    expected += ("scope",)
                if composite:
                    expected += ("composite_v",)
                    if not scoped or type(msg["composite_v"]) is not int or msg["composite_v"] != 1:
                        raise ValueError("Invalid composite version or missing scope")
                fields(msg, *expected)
                if (msg["type"] != "HELLO" or msg["protocol"] != "kvmbridge" or
                        type(msg["version"]) is not int or msg["version"] != 1):
                    raise ValueError("Session unavailable")
                profile = msg["profile"]
                scope = msg.get("scope")
                if scoped:
                    if (type(profile) is not str or len(profile) > 100 or not PROFILE_ID.fullmatch(profile) or
                            profile in (STUDIO, SIM_CONTROL) or scope not in (CAPTURE_SCOPE, TELEMETRY_SCOPE)):
                        raise ValueError("Invalid profile or scope")
                elif profile not in LEGACY_SCOPES:
                    raise ValueError("Unknown profile")
                if any(not composite or not other.composite or profile != other.profile or scope == other.scope
                       for other in self.sessions.values()):
                    raise ValueError("Session unavailable")
                session = Session(ws, profile, self.services, scope, composite=composite,
                                  status_changed=self.publish_status)
                if not self.sessions:
                    self.last_end.clear()
                self.last_end.pop(session.scope, None)
                self.sessions[session.scope] = session
                try:
                    await session.control()
                except TimeoutError:
                    session.stop("The board's control request timed out.")
                except (ValueError, TypeError, KeyError):
                    session.stop("KVMpad sent an invalid control request.")
                finally:
                    await session.finish()
            else:
                fields(msg, "type", "session")
                scope = {"/image": CAPTURE_SCOPE, "/telemetry": TELEMETRY_SCOPE}.get(path)
                session = self.sessions.get(scope)
                if (not session or not session.active or session.channel or path != session.kind or
                        msg["type"] != "BIND" or type(msg["session"]) is not str or
                        not secrets.compare_digest(session.token, msg["session"])):
                    raise PermissionError("Channel unavailable")
                await session.bind(ws)
        except (ConnectionClosed, TimeoutError, OSError, ValueError, KeyError, TypeError, RecursionError):
            pass
        finally:
            await ws.close(code=1008, reason="Session ended")


def listen(broker, address, port, context):
    if context.verify_mode != ssl.CERT_REQUIRED:
        raise ValueError("Peer certificate required")
    return serve(broker.handler, address, port, ssl=context, origins=[None], compression=None,
                 max_size=4096, max_queue=4, write_limit=16384, open_timeout=3, close_timeout=.3,
                 ping_interval=2, ping_timeout=4, server_header=None)
