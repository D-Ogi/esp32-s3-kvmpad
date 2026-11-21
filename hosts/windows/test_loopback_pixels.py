# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run production Windows HOST main in a child, with private stdin consent and process-created D3D pixels."""
import argparse
import asyncio
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import secrets
import ssl
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed

import host
from broker import MAG
from framing import FORMATS, Receiver
from media import MODES, source_crop
from test_capture_surface import decode, device_results, verify_crop, verify_picture
from test_host import identity, tls_credentials


def require_fixture(path):
    path=path.resolve(strict=True)
    if path.name.lower()!="kvmpad-test-surface.exe":
        raise ValueError("Use the native synthetic surface fixture")
    return path


class FixtureForeground(host.Foreground):
    def __init__(self, pin):
        self.broker=None
        self.pending=None
        self.root=SimpleNamespace(destroy=lambda:None)

    def status(self, message):
        pass

    async def approve(self, profile, message):
        assert profile==MAG and self.pending is None
        self.pending=asyncio.get_running_loop().create_future()
        print("REVIEW",flush=True)
        try: return await self.pending
        finally: self.pending=None

    async def run(self):
        while True:
            cmd=(await asyncio.to_thread(sys.stdin.readline)).strip()
            if cmd in ("ALLOW","DENY"):
                if self.pending is None: raise ValueError("No pending review")
                self.pending.set_result(cmd=="ALLOW")
            elif cmd=="REVOKE": self.stop()
            elif cmd in ("EXIT",""):
                self.stop()
                return
            else: raise ValueError("Unknown local fixture command")


async def serve_fixture(args):
    picker=require_fixture(args.surface)
    original=host.listen
    @contextlib.asynccontextmanager
    async def bound(broker, address, port, context):
        assert address=="127.0.0.1" and port==0
        async with original(broker,address,port,context) as server:
            print("PORT",server.sockets[0].getsockname()[1],flush=True)
            yield server
    opts=SimpleNamespace(listen="127.0.0.1",port=0,board_cert=args.credentials/"board.pem",
                         host_cert=args.credentials/"host.pem",host_key=args.credentials/"host.key",
                         msfs=None,obs=False,picker=picker,ffmpeg=args.ffmpeg)
    with patch.object(host,"Foreground",FixtureForeground), patch.object(host,"listen",bound):
        pin, context = tls_credentials(opts.board_cert, opts.host_cert, opts.host_key)
        await host.run_listener(opts, opts.listen, pin, context)


async def send(ws, **fields):
    await ws.send(json.dumps(fields,separators=(",",":")))


async def silence(ws, interval):
    try: await asyncio.wait_for(ws.recv(),interval)
    except TimeoutError: return
    raise AssertionError("Unexpected message during quiescence")


async def closed(ws):
    await asyncio.wait_for(ws.wait_closed(),2)
    try: await asyncio.wait_for(ws.recv(),.5)
    except ConnectionClosed: return
    raise AssertionError("Unexpected queued data after closure")


@contextlib.contextmanager
def temporary_credentials():
    # Inherit the workspace temp ACL. Python 3.14 mkdtemp applies an owner-only
    # Windows ACL that excludes the isolated HOST child in a managed workspace.
    path=Path(tempfile.gettempdir())/("kvmpad-loopback-"+secrets.token_hex(16))
    path.mkdir()
    try: yield path
    finally:
        for file in ("host.key","board.key","host.pem","board.pem"):
            (path/file).unlink(missing_ok=True)
        path.rmdir()


@contextlib.asynccontextmanager
async def loopback_host(surface, ffmpeg, layout):
    with temporary_credentials() as tmp:
        hp,_=identity(tmp,"host")
        bp,bk=identity(tmp,"board")
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version=ssl.TLSVersion.TLSv1_2
        ctx.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
        ctx.load_verify_locations(cafile=hp)
        ctx.load_cert_chain(bp,bk)
        pin=hashlib.sha256(ssl.PEM_cert_to_DER_cert(hp.read_text())).digest()
        proc=subprocess.Popen([sys.executable,"-B",str(Path(__file__).resolve()),"--serve",
                "--credentials",str(tmp),"--surface",str(surface),"--ffmpeg",ffmpeg],
                stdin=subprocess.PIPE,stdout=subprocess.PIPE,creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0),
                env={**os.environ,"KVMPAD_TEST_GPU":"warp","KVMPAD_TEST_LAYOUT":layout})
        async def command(s):
            proc.stdin.write((s+"\n").encode())
            proc.stdin.flush()
        try:
            ready=(await asyncio.wait_for(asyncio.to_thread(proc.stdout.readline),10)).decode().strip().split()
            assert len(ready)==2 and ready[0]=="PORT"
            address="wss://127.0.0.1:"+str(int(ready[1]))
            async def socket(path):
                ws=await connect(address+path,ssl=ctx,server_hostname="localhost",proxy=None,
                                 open_timeout=3,close_timeout=.5,compression=None)
                peer=ws.transport.get_extra_info("ssl_object")
                assert hashlib.sha256(peer.getpeercert(binary_form=True)).digest()==pin
                return ws

            async def hello():
                ctrl=await socket("/control")
                await send(ctrl,type="HELLO",protocol="kvmbridge",version=1,profile=MAG)
                opened=json.loads(await asyncio.wait_for(ctrl.recv(),3))
                assert opened["type"]=="SESSION" and opened["crop_v"]==1
                return ctrl,opened["session"]

            async def consent(ctrl,codec,mode,metadata,decision,max_fps=None):
                extra={"crop_v":1,"source_crop":source_crop(codec,mode)} if metadata else {}
                if max_fps is not None:
                    extra.update(present_v=1,max_fps=max_fps)
                await send(ctrl,type="START",seq=1,mode=mode,formats=[codec],**extra)
                assert (await asyncio.wait_for(asyncio.to_thread(proc.stdout.readline),2)).strip()==b"REVIEW"
                await silence(ctrl,.12)
                await command(decision)
                return json.loads(await asyncio.wait_for(ctrl.recv(),5))

            yield socket, hello, consent, command
        finally:
            if proc.poll() is None:
                with contextlib.suppress(ConnectionError): await command("EXIT")
                try: await asyncio.wait_for(asyncio.to_thread(proc.wait),4)
                except TimeoutError:
                    proc.kill()
                    await asyncio.to_thread(proc.wait)
            proc.stdin.close()
            proc.stdout.close()
            if proc.returncode: raise RuntimeError(f"HOST fixture process exited {proc.returncode}")

async def run(args):
    surface=require_fixture(args.surface)
    ffmpeg=shutil.which(args.ffmpeg)
    if not ffmpeg or not args.crop_check or not args.crop_check.is_file(): raise ValueError("Provide FFmpeg and the C receiver fixture")
    async with loopback_host(surface,ffmpeg,"landscape") as (socket,hello,consent,command):
        rows=[]
        for codec,mode in (("jpeg","PIXEL_1X"),("h264-intra","ZOOM_2X"),("cinepak-intra","OVERVIEW")):
            same=[]
            for metadata in (False,True):
                ctrl,token=await hello()
                ack=await consent(ctrl,codec,mode,metadata,"ALLOW")
                assert ack["type"]=="STARTED" and ack["resource"]==r"\\.\DISPLAY42"
                assert ack["mode"]==mode and ack["codec"]==codec
                assert ack.get("source_crop")== (source_crop(codec,mode) if metadata else None)
                assert ack.get("crop_v")== (1 if metadata else None)
                stream=ack["stream"]
                data=await socket("/image")
                await send(data,type="BIND",session=token)
                receiver=Receiver(codec,stream)
                hashes=[]
                pairs=[]
                for index in (0,1):
                    if index:
                        resume_at=asyncio.get_running_loop().time()
                        await send(ctrl,type="RESUME",seq=3)
                        assert json.loads(await ctrl.recv())=={"type":"RESUMED","seq":3}
                    item=await asyncio.wait_for(data.recv(),3)
                    if metadata:
                        assert type(item) is str and json.loads(item)["type"]=="CROP1"
                        frame=await asyncio.wait_for(data.recv(),2)
                        pairs.append((item.encode("utf-8"),frame))
                    else: frame=item
                    assert type(frame) is bytes
                    payload=receiver.receive(frame)
                    assert receiver.sequence==index
                    # The HOST rate-limits control commands to 10/s. Freeze after the image,
                    # before expensive fixture decoding; at most the first 5 Hz interval elapsed.
                    if index: await asyncio.sleep(max(0,resume_at+.11-asyncio.get_running_loop().time()))
                    await send(ctrl,type="FREEZE",seq=4 if index else 2)
                    frozen=json.loads(await asyncio.wait_for(ctrl.recv(),2))
                    assert frozen==({"type":"FROZEN","seq":4 if index else 2,"last_frame":index} if metadata
                                    else {"type":"FROZEN","seq":4 if index else 2})
                    await silence(data,.28)
                    if metadata:
                        verify_crop(args.crop_check,codec,MODES[mode],"landscape",(1440,1080,144),index,frame,
                                    json.loads(item)["data"],stream)
                    pixels=decode(ffmpeg,codec,payload)
                    landmarks=verify_picture(pixels,codec,MODES[mode],(1440,1080,144),index)
                    hashes.append({"encoded_sha256":hashlib.sha256(payload).hexdigest(),
                                   "decoded_gray_sha256":hashlib.sha256(pixels).hexdigest(),
                                   "validated_luma_samples":landmarks})
                if metadata:
                    coords=device_results(args.crop_check,codec,stream,source_crop(codec,mode),pairs)
                    assert len(coords)==2 and all(x!="REJECT" for x in coords)
                    assert coords[0].split()[:2]==["-696","348"] and coords[1].split()[:2]==["-1","899"],coords
                else: coords=[]
                if codec=="h264-intra":
                    await command("REVOKE")
                    reason="local HOST revoke"
                elif codec=="cinepak-intra":
                    await data.close()
                    reason="image disconnect"
                else:
                    await send(ctrl,type="STOP",seq=5)
                    reason="board STOP"
                await asyncio.gather(closed(ctrl),closed(data))
                denied=await socket("/image")
                await send(denied,type="BIND",session=token)
                await closed(denied)
                same.append(hashes)
                rows.append({"codec":codec,"mode":mode,"metadata":metadata,"close":reason,
                             "frames":hashes,"geometry":coords})
                print(f"{codec} {mode} metadata={metadata}: two decoded frames, Freeze/Resume, {reason}, stale bind rejected")
            assert same[0]==same[1],(codec,same)
        ctrl,_=await hello()
        ack=await consent(ctrl,"jpeg","PIXEL_1X",True,"DENY")
        assert ack["type"]=="DENIED"
        await closed(ctrl)
        print("Fresh connection required another explicit fixture decision; denial emitted no image or metadata.")
        if args.report: args.report.write_text(json.dumps(rows,indent=2)+"\n",encoding="utf-8")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--surface",type=Path,required=True)
    parser.add_argument("--ffmpeg",default="ffmpeg")
    parser.add_argument("--crop-check",type=Path)
    parser.add_argument("--report",type=Path)
    parser.add_argument("--serve",action="store_true",help=argparse.SUPPRESS)
    parser.add_argument("--credentials",type=Path,help=argparse.SUPPRESS)
    args=parser.parse_args()
    asyncio.run(serve_fixture(args) if args.serve else run(args))
