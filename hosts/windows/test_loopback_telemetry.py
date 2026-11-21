# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Exercise production Windows HOST TLS, broker, six datarefs and SimConnect read IPC on loopback."""
import argparse
import asyncio
from contextlib import asynccontextmanager
import http.server
import json
import os
from pathlib import Path
import ssl
import threading
import time
from types import SimpleNamespace
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.client import connect

import host
from adapters import TOPICS, Telemetry, XPlane
from broker import GAUGES
from test_host import identity, tls_credentials
from test_loopback_pixels import send, temporary_credentials

FIRST=[123.5,5320.,-2.,-450.,8.,-11.]
SECOND=[144.25,6280.,367.,780.,-3.,14.]
NAMES=list(TOPICS)
REFS=["sim/cockpit2/gauges/indicators/"+TOPICS[n][1] for n in NAMES]
RATES={name:20 if ".attitude." in name else 10 for name in NAMES}


class Simulator(http.server.BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"

    def log_message(self,*args):
        pass

    def do_GET(self):
        phase=self.server.fixture.phase
        self.server.fixture.gets+=1
        path=urlsplit(self.path)
        code=200
        generation = "B" if phase in ("B", "L") else "A"
        if phase in ("X", "F", "M"):
            self.close_connection=True
            code={"X":503,"F":500,"M":404}[phase]
            data=None
        elif path.path=="/api/v2/datarefs":
            base=100 if generation=="A" else 1000
            names=parse_qs(path.query)["filter[name]"]
            assert len(names)==1 and names[0] in REFS
            self.server.fixture.lookups[generation].add(names[0])
            data=[{"id":base+REFS.index(names[0]),"name":names[0]}]
        else:
            base=100 if generation=="A" else 1000
            parts=path.path.strip("/").split("/")
            assert parts[:3]==["api","v2","datarefs"] and parts[-1]=="value"
            index=int(parts[3])-base
            if not 0<=index<6:
                self.server.fixture.stale_id+=1
                code=404
                data=None
            else:
                if index==0: time.sleep(.18)  # A slow IAS request must not block attitude samples.
                data=(FIRST if generation=="A" else SECOND)[index]
                if phase in ("E","L") and index==5: data={"unexpected":"measurement"}
        body=json.dumps({"data":data}).encode()
        self.send_response(code)
        self.send_header("Content-Length",str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except OSError: pass


async def run(args):
    helper=args.helper.resolve(strict=True)
    if helper.name!="kvmpad-test-simconnect.exe":
        raise ValueError("Use the compiled synthetic IPC fixture")
    state=SimpleNamespace(phase="A",gets=0,lookups={"A":set(),"B":set()},stale_id=0)
    endpoint=http.server.ThreadingHTTPServer(("127.0.0.1",0),Simulator)
    endpoint.daemon_threads=True
    endpoint.fixture=state
    thread=threading.Thread(target=endpoint.serve_forever,daemon=True)
    thread.start()
    approve=asyncio.Queue()
    port_ready=asyncio.get_running_loop().create_future()
    exit_ui=asyncio.Event()
    class UI:
        def __init__(self,pin): self.root=SimpleNamespace(destroy=lambda:None)
        def status(self,text): pass
        async def approve(self,profile,text):
            assert profile==GAUGES
            permit=asyncio.get_running_loop().create_future()
            await approve.put(permit)
            return await permit
        async def run(self): await exit_ui.wait()
    listen=host.listen
    @asynccontextmanager
    async def capture_port(broker,addr,port,ctx):
        assert addr=="127.0.0.1" and port==0
        state.broker=broker
        async with listen(broker,addr,port,ctx) as server:
            port_ready.set_result(server.sockets[0].getsockname()[1])
            yield server
    try:
        with temporary_credentials() as tmp:
            hp,hk=identity(tmp,"host")
            bp,bk=identity(tmp,"board")
            trace=tmp/"requests"
            trace.write_bytes(b"")
            phase_file=tmp/"phase"
            phase_file.write_text("A")
            opts=SimpleNamespace(listen="127.0.0.1",port=0,board_cert=bp,host_cert=hp,host_key=hk,
                                 msfs=helper,obs=False,picker=None,ffmpeg="ffmpeg")
            ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
            ctx.load_verify_locations(cafile=hp)
            ctx.load_cert_chain(bp,bk)
            async def socket(path):
                return await connect("wss://127.0.0.1:"+str(await port_ready)+path,ssl=ctx,
                                     server_hostname="localhost",proxy=None,compression=None,open_timeout=3)
            out={}
            patches=(patch.object(host,"Foreground",UI),patch.object(host,"listen",capture_port),
                     patch.object(host,"XPlane",lambda:XPlane(endpoint.server_port)),
                     patch.dict(os.environ,KVMPAD_TEST_SIM_PHASE=str(phase_file),KVMPAD_TEST_SIM_TRACE=str(trace)))
            with patches[0],patches[1],patches[2],patches[3]:
                pin, context = tls_credentials(opts.board_cert, opts.host_cert, opts.host_key)
                service=asyncio.create_task(host.run_listener(opts, opts.listen, pin, context))
                try:
                    for source in ("sim.msfs2024","sim.xplane"):
                        def phase(p):
                            state.phase=p
                            phase_file.write_text(p)
                        phase("A")
                        ctrl=await socket("/control")
                        await send(ctrl,type="HELLO",protocol="kvmbridge",version=1,profile=GAUGES)
                        advertised=json.loads(await ctrl.recv())
                        assert advertised["quality_v"]==1
                        token=advertised["session"]
                        await send(ctrl,type="SUBSCRIBE",seq=1,source=source,quality_v=1,
                                   topics=[{"name":k,"rate_hz":v} for k,v in RATES.items()])
                        (await asyncio.wait_for(approve.get(),2)).set_result(True)
                        assert json.loads(await ctrl.recv())=={"type":"SUBSCRIBED","source":source,"quality_v":1}
                        data=await socket("/telemetry")
                        await send(data,type="BIND",session=token)
                        seq=0
                        async def message():
                            nonlocal seq
                            m=json.loads(await asyncio.wait_for(data.recv(),1.5))
                            assert m["type"]=="TELEMETRY" and m["source"]==source and m["topic"] in TOPICS
                            assert m["unit"]==TOPICS[m["topic"]][0] and m["seq"]==seq+1
                            assert type(m["ts_mono_ms"]) is int and 0<=m["ts_mono_ms"]<=0xffffffff
                            seq=m["seq"]
                            return m
                        async def valid(values):
                            seen=set()
                            async with asyncio.timeout(4):
                                while len(seen)<6:
                                    m=await message()
                                    if m["quality"]=="VALID":
                                        i=NAMES.index(m["topic"])
                                        expected=values[i]%360 if i==2 else values[i]
                                        assert m["value"]==expected
                                        seen.add(m["topic"])
                        await valid(FIRST)
                        times={name:set() for name in NAMES}
                        start=time.monotonic()
                        while time.monotonic()-start<2:
                            m=await message()
                            assert m["quality"]=="VALID"
                            times[m["topic"]].add(m["ts_mono_ms"])
                        elapsed=time.monotonic()-start
                        rates={k:round((len(ts)-1)*1000/(max(ts)-min(ts)),2) for k,ts in times.items()}
                        assert all(rates[n]>=10 for n in NAMES[4:]),rates
                        assert all(rates[n]>=2 for n in NAMES[:4]),rates
                        phase("E")  # One bad response. Other instruments remain valid.
                        async def expect_error_channels(targets, quality):
                            seen=set()
                            async with asyncio.timeout(4):
                                while seen!=targets:
                                    m=await message()
                                    if m["quality"]==quality:
                                        assert m["topic"] in targets and m["value"] is None
                                        seen.add(m["topic"])
                                    elif m["topic"] not in targets:
                                        assert m["quality"]=="VALID"
                        await expect_error_channels({NAMES[5]}, "ERROR")
                        phase("F")  # Explicit provider error affects every requested measurement.
                        await expect_error_channels(set(NAMES), "ERROR")
                        if source=="sim.xplane":
                            phase("M")  # The known DataRef no longer exists.
                            await expect_error_channels(set(NAMES), "UNAVAILABLE")
                        phase("A")
                        await valid(FIRST)
                        print(source, "one invalid roll, explicit all-channel provider failure and recovery verified")
                        phase("X")
                        missing,stale={},{}
                        async with asyncio.timeout(5):
                            while len(missing)<6:
                                m=await message()
                                n=m["topic"]
                                age=(int(time.monotonic()*1000)-m["ts_mono_ms"]) & 0xffffffff
                                if m["quality"]=="STALE" and n not in stale:
                                    assert 990<=age<=1500 and m["value"] is not None
                                    stale[n]=age
                                elif m["quality"]=="UNAVAILABLE" and n not in missing:
                                    assert n in stale and 2990<=age<=3600 and m["value"] is None
                                    missing[n]=age
                        phase("B")
                        await valid(SECOND)
                        await send(ctrl,type="STOP",seq=2)
                        await asyncio.wait_for(ctrl.wait_closed(),3)
                        await asyncio.wait_for(data.wait_closed(),3)
                        async with asyncio.timeout(3):
                            while state.broker.session is not None: await asyncio.sleep(.01)
                        out[source]={"wire_messages":seq,"live_window_seconds":round(elapsed,3),"fresh_sample_hz":rates,
                                     "error_checks":["single invalid roll","explicit provider error","recovery"],
                                     "first_stale_age_ms":stale,"first_unavailable_age_ms":missing}
                        print(source,rates,"first stale",min(stale.values()),max(stale.values()),
                              "ms; unavailable",min(missing.values()),max(missing.values()),"ms; recovered all six")
                    assert all(v==set(REFS) for v in state.lookups.values()) and not state.stale_id
                    assert set(trace.read_bytes())=={63}
                    state.lookups["B"].clear()
                    for source in ("sim.msfs2024","sim.xplane"):
                        phase("B")
                        topics=(NAMES[1],NAMES[5])
                        at=trace.stat().st_size
                        gets=state.gets
                        ctrl=await socket("/control")
                        await send(ctrl,type="HELLO",protocol="kvmbridge",version=1,profile=GAUGES)
                        token=json.loads(await ctrl.recv())["session"]
                        await send(ctrl,type="SUBSCRIBE",seq=1,source=source,topics=[{"name":n,"rate_hz":5} for n in topics])
                        decision=await asyncio.wait_for(approve.get(),2)
                        await asyncio.sleep(.15)
                        assert trace.stat().st_size==at and state.gets==gets
                        decision.set_result(True)
                        assert json.loads(await ctrl.recv())["type"]=="SUBSCRIBED"
                        data=await socket("/telemetry")
                        await send(data,type="BIND",session=token)
                        seen=set()
                        async with asyncio.timeout(3):
                            while seen!=set(topics):
                                m=json.loads(await data.recv())
                                assert m["topic"] in topics and m["unit"]==TOPICS[m["topic"]][0]
                                if m["quality"]=="VALID":
                                    assert m["value"]==SECOND[NAMES.index(m["topic"])]
                                    seen.add(m["topic"])
                        if source=="sim.xplane":
                            phase("L")
                            async with asyncio.timeout(2):
                                while True:
                                    m=json.loads(await data.recv())
                                    assert m["quality"]!="ERROR"
                                    if m["topic"]==NAMES[5] and m["quality"]=="UNAVAILABLE":
                                        assert m["value"] is None
                                        break
                        phase("X")
                        await asyncio.sleep(.8)
                        state.lookups["A"].clear()
                        phase("A")
                        fresh=set()
                        async with asyncio.timeout(3):
                            while fresh!=set(topics):
                                m=json.loads(await data.recv())
                                assert m["topic"] in topics
                                if m["quality"]=="VALID" and m["value"]==FIRST[NAMES.index(m["topic"])]:
                                    fresh.add(m["topic"])
                        await send(ctrl,type="STOP",seq=2)
                        await asyncio.wait_for(ctrl.wait_closed(),3)
                        await asyncio.wait_for(data.wait_closed(),3)
                        async with asyncio.timeout(3):
                            while state.broker.session is not None: await asyncio.sleep(.01)
                        if source=="sim.msfs2024": assert set(trace.read_bytes()[at:])=={34}
                        else: assert state.lookups["A"]==state.lookups["B"]=={REFS[1],REFS[5]}
                        print(source,"only altitude and roll were requested before and after a second simulator restart")
                    # Once the worker has observed cancellation, there are no further requests.
                    await asyncio.sleep(.55)
                    count=state.gets
                    size=trace.stat().st_size
                    await asyncio.sleep(.6)
                    assert state.gets==count and trace.stat().st_size==size
                    if args.report: args.report.write_text(json.dumps(out,indent=2)+"\n",encoding="utf-8")
                finally:
                    exit_ui.set()
                    await asyncio.wait_for(service,3)
                    phase_file.unlink(missing_ok=True)
                    trace.unlink(missing_ok=True)
    finally:
        await asyncio.to_thread(endpoint.shutdown)
        endpoint.server_close()
        thread.join()
    t=Telemetry("sim.xplane",{NAMES[0]:10})
    t.update({NAMES[0]:1.5},1)
    t.update({NAMES[0]:10**1000},1.5)
    t.update({NAMES[0]:float("nan")},1.6)
    t.update({NAMES[0]:True},1.7)
    msg, = t.messages(1.7)
    assert msg["quality"]=="UNAVAILABLE" and msg["value"] is None
    print("Source restart re-resolved all six X-Plane IDs; no additional reads after Stop. Malformed samples could not refresh the last good sample.")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper",type=Path,required=True)
    parser.add_argument("--report",type=Path)
    asyncio.run(run(parser.parse_args()))
