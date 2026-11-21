# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Foreground Studio HID/1 endpoint for a single explicitly selected USB serial."""
import argparse
import asyncio
import binascii
import getpass
import re
import secrets
import struct
import time
from pathlib import Path

from adapters import OBS
from host import Foreground
from media import child, terminate

HEADER = struct.Struct("<4B2H3B")
ACTIONS = {0x0101:(1, "obs.record.start", "Start recording"),
           0x0102:(2, "obs.record.stop", "Stop recording"),
           0x0103:(3, "obs.input.mute", "Mute input")}


def frames(report, op, seq, data, flags=0, ack=0):
    if len(data) > 408:
        raise ValueError("Studio message exceeds eight fragments")
    count = max(1, (len(data)+50)//51)
    for index in range(count):
        block = data[51*index:51*(index+1)]
        body = HEADER.pack(report,1,op,flags,seq,ack,len(block),index,count) + block.ljust(51,b"\0")
        yield body + struct.pack("<H",binascii.crc_hqx(body,0xffff))


class Assembly:
    def __init__(self):
        self.partial = None

    def read(self, wire, now):
        if len(wire)!=64 or binascii.crc_hqx(wire[:62],0xffff)!=struct.unpack_from("<H",wire,62)[0]:
            raise ValueError("Studio CRC or size")
        rid,major,op,flags,seq,ack,length,index,count=HEADER.unpack_from(wire)
        if (rid!=0x22 or major!=1 or op not in (1,3,4,0x10,0x20,0x21,0x22) or flags>3 or
                length>51 or not 0<=index<count<=8 or any(wire[11+length:62])):
            raise ValueError("Studio report fields")
        key=op,flags,seq,ack,count
        if index==0:
            if self.partial:
                raise ValueError("Overlapping Studio messages")
            self.partial=(key,[],now+.3)
        if not self.partial:
            raise ValueError("Missing first fragment")
        prior,parts,deadline=self.partial
        if key!=prior or len(parts)!=index or now>deadline:
            raise ValueError("Studio fragment order or timeout")
        parts.append(wire[11:11+length])
        if len(parts)==count:
            self.partial=None
            return op,flags,seq,ack,b"".join(parts)
        return None


class Studio:
    def __init__(self, send_report, adapter, mute):
        self.send_report, self.adapter = send_report, adapter
        self.actions={k:v for k,v in ACTIONS.items() if mute or k!=0x0103}
        self.assembly=Assembly()
        self.tx=0
        self.last_rx=None
        self.peer_nonce=0
        self.observed_version=0
        self.pending=None
        self.request=None
        self.result=None
        self.lock=asyncio.Lock()
        self.closed=False
        self.last_ping=time.monotonic()

    async def send(self, op, data, ack=0):
        async with self.lock:
            if self.closed:
                return
            self.tx=(self.tx+1)&0xffff
            for packet in frames(0x23,op,self.tx,data,ack=ack):
                await self.send_report(packet)

    async def ready(self, data):
        if self.peer_nonce or len(data)!=8:
            raise ValueError("USB session changed")
        major,minor,caps,nonce=struct.unpack("<BBHI",data)
        if major!=1 or not nonce or caps&0x21!=0x21:
            raise ValueError("Unsupported Studio feature set")
        self.peer_nonce=secrets.randbelow(0xffffffff)+1
        await self.send(2,struct.pack("<BBHII",1,0,0x21,nonce,self.peer_nonce))
        await self.send(0x14,bytes([len(self.actions)])+b"".join(struct.pack("<H",k) for k in self.actions))
        bank=bytearray(struct.pack("<HB",1,len(self.actions)))
        for number,(tile,_,label) in self.actions.items():
            name=label.encode("ascii")
            bank.extend(struct.pack("<HHB",tile,number,len(name))+name)
        await self.send(0x15,bank)

    async def outcome(self, action, tile, request):
        try:
            response=await asyncio.wait_for(self.adapter.action(self.actions[action][1]),1.7)
            key="muted" if action==0x0103 else "recording"
            actual=response[key]
            if type(actual) is not bool or response["status"]!="SUCCESS":
                raise ValueError("Missing OBS state")
            code=0 if actual==(action!=0x0102) else 6
            self.observed_version=(self.observed_version+1)&0xffffffff
            # Both record tiles show the observed recording state.
            for item in ((3,) if key=="muted" else (1,2)):
                await self.send(0x13,struct.pack("<HHBI",1,item,2 if actual else 1,self.observed_version))
        except TimeoutError:
            code=3
        except PermissionError:
            code=2
        except (OSError,ConnectionError):
            code=1
        except Exception:
            code=5
        self.result=struct.pack("<IHI",request,code,self.observed_version)
        await self.send(0x12,self.result)

    async def receive(self, wire):
        message=self.assembly.read(wire,time.monotonic())
        if not message:
            return
        op,flags,seq,ack,data=message
        if flags&2 and op!=0x20:
            raise ValueError("Unexpected Studio error flag")
        if op==0x10 and self.request and data==self.request[0]:
            if flags&1:
                await self.send(0x21,struct.pack("<H",seq),ack=seq)
            await self.send(0x11,struct.pack("<IB",self.request[1],int(self.request[2])))
            if self.result:
                await self.send(0x12,self.result)
            return
        if self.last_rx is not None and not 0<(seq-self.last_rx)&0xffff<0x8000:
            raise ValueError("Stale Studio sequence")
        self.last_rx=seq
        if op==1:
            await self.ready(data)
            return
        if not self.peer_nonce:
            raise ValueError("Expected HELLO")
        if flags&1:
            await self.send(0x21,struct.pack("<H",seq),ack=seq)
        if op==3 and len(data)==4:
            self.last_ping=time.monotonic()
            await self.send(4,data)
        elif op==0x10 and len(data)==8:
            action,tile,request=struct.unpack("<HHI",data)
            if (not request or (self.request and not 0<(request-self.request[1])&0xffffffff<0x80000000)
                or (self.pending and not self.pending.done())):
                raise ValueError("Duplicate or outstanding action")
            allowed=action in self.actions and self.actions[action][0]==tile
            self.request,self.result=(data,request,allowed),None
            await self.send(0x11,struct.pack("<IB",request,int(allowed)))
            if allowed:
                self.pending=asyncio.create_task(self.outcome(action,tile,request))
        elif op==0x21 and len(data)==2 and ack==int.from_bytes(data,"little"):
            pass
        else:
            raise ValueError("Studio stopped or invalid message")

    async def close(self,bye=False):
        if self.pending:
            self.pending.cancel()
            await asyncio.gather(self.pending,return_exceptions=True)
        if bye and self.peer_nonce:
            try:
                await asyncio.wait_for(self.send(0x22,b"\0\0"),.15)
            except (TimeoutError,OSError):
                pass
        self.closed=True
        self.request=self.result=None
        self.peer_nonce=0


class UsbSession:
    def __init__(self, ui, args, obs):
        self.ui,self.args,self.obs=ui,args,obs
        self.task=None

    def stop(self):
        if self.task and not self.task.done():
            self.task.cancel()

    async def run(self):
        process=None
        protocol=None
        try:
            actions="start recording, stop recording" + (", mute "+self.args.obs_input if self.args.obs_input else "")
            if not await self.ui.approve("USB Studio",f"Allow board buttons for {actions} until Stop or disconnect?"):
                return
            if (await asyncio.wait_for(self.obs.action("obs.record.status"),2)).get("status")!="SUCCESS":
                raise ValueError("OBS unavailable")
            process=child(self.args.io,"--serial",self.args.serial)
            feature=await asyncio.wait_for(asyncio.to_thread(process.stdout.read,16),3)
            if len(feature)!=16 or feature[:3]!=bytes((0x24,1,0)) or feature[5:7]!=bytes((8,51)):
                raise ValueError("Studio feature unavailable")
            async def write(packet):
                def flush():
                    process.stdin.write(packet)
                    process.stdin.flush()
                await asyncio.wait_for(asyncio.to_thread(flush),.8)
            protocol=Studio(write,self.obs,self.args.obs_input is not None)
            self.ui.status("USB Studio armed. Waiting for physical board requests.")
            async with asyncio.timeout(3600):
                while time.monotonic()-protocol.last_ping<=6:
                    timeout=6
                    if protocol.assembly.partial:
                        timeout=max(.001,protocol.assembly.partial[2]-time.monotonic())
                    wire=await asyncio.wait_for(asyncio.to_thread(process.stdout.read,64),timeout)
                    await protocol.receive(wire)
        except (OSError,ValueError,TimeoutError):
            pass
        finally:
            if protocol:
                await protocol.close(bye=True)
            if process:
                await terminate(process)
            self.ui.status("USB Studio stopped")


async def main(args):
    if not re.fullmatch("[A-F0-9]{12}",args.serial):
        raise ValueError("The exact 12-character uppercase board serial is required")
    obs=OBS(getpass.getpass("Local OBS WebSocket password: "),input_name=args.obs_input)
    if not obs.password:
        raise ValueError("OBS authentication is required")
    ui=Foreground(args.serial,"USB Studio serial")
    runner=ui.broker=UsbSession(ui,args,obs)
    try:
        runner.task=asyncio.create_task(runner.run())
        await ui.run()
    finally:
        runner.stop()
        await asyncio.gather(runner.task,return_exceptions=True)
        ui.root.destroy()


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial",required=True)
    parser.add_argument("--io",type=Path,required=True)
    parser.add_argument("--obs-input")
    asyncio.run(main(parser.parse_args()))
