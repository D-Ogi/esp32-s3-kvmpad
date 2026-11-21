# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify the real broker/receiver contract entirely in process with deterministic fixture images."""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import struct
from unittest.mock import AsyncMock, Mock, patch
import zlib

from broker import MAG, Services, Session
from media import CROP_PREFIX, crop_record, encode
from test_capture_surface import device_results


def make(session, seq, payload=b"fixture-image"):
    image=encode("jpeg",session,seq,1234,payload)
    prefix=CROP_PREFIX.pack(b"KCM1",-44,30,-300,-226,512,512,0,0,0,bytes(3))
    return image,crop_record(prefix,image,"jpeg","PIXEL_1X")


def text(raw):
    return json.dumps({"type":"CROP1","data":raw.hex()},separators=(",",":")).encode()


def check_device(exe):
    stream=0xf001cafe
    image,record=make(stream,0xffffffff)
    raw=bytes.fromhex(record)
    message=text(raw)
    expected="-44 30 -300 -226 512 512 0 0 0"
    def test(pairs):
        return device_results(exe,"jpeg",stream,(512,512),pairs)
    assert test([(message,image)])==[expected]
    next_image,next_record=make(stream,0)
    assert test([(message,image),(message,image),(text(bytes.fromhex(next_record)),next_image)])==[expected,"REJECT",expected]
    # Exercise hard semantic checks even with a fresh outer CRC.
    invalid=[]
    def altered(offset, data):
        b=bytearray(raw); b[offset:offset+len(data)]=data
        struct.pack_into("<I",b,44,zlib.crc32(b[:44]))
        return text(b)
    for offset,data in [(0,b"KCM2"),(20,b"\0\0"),(22,struct.pack("<H",513)),
                        (24,b"\x01"),(26,b"\x01"),(28,b"\x01"),(28,b"\x02"),(29,b"\x01"),
                        (4,struct.pack("<i",212)),(8,struct.pack("<i",-227)),
                        (32,struct.pack("<I",7)),(36,struct.pack("<I",2)),(40,b"\x00"*4)]:
        invalid.append((altered(offset,data),image))
    b=bytearray(raw); b[12]^=1
    invalid.extend([(text(b),image),(text(raw[:-1]),image),(text(raw)+b"X",image),
                    (message.replace(b"CROP1",b"CAPS1"),image),(message.replace(b'"type":',b'"extra":1,"type":'),image),
                    (message.replace(b"4b43",b"zz43"),image),(message,image[:32]),
                    (message,image[:-1]+bytes([image[-1]^1])),(message,next_image)])
    assert test(invalid)==["REJECT"]*len(invalid)
    # Correlation also detects another valid and CRC-correct image with the same ID and sequence.
    other,_=make(stream,0xffffffff,b"replacement-image")
    assert test([(message,other)])==["REJECT"]
    fingerprint=hashlib.sha256(message+image).hexdigest()
    print(f"C receiver: accepted signed positions, wrap and exact image; refused {len(invalid)+2} malformed, replayed or mismatched pairs.")
    assert fingerprint=="7e220acb090539871135d2e281bfda979f9319bd16e34dcd2e46f5c87fe61d4e"
    print("Golden text + KVS SHA-256",fingerprint)
    return fingerprint


async def broker_checks(check):
    def launch(consent=True):
        ws=Mock(); ws.send=AsyncMock()
        picker=Mock(); picker.open=AsyncMock(return_value=r"\\.\DISPLAY42")
        async def wait_closed(): await asyncio.Event().wait()
        picker.wait_closed=wait_closed
        picker.frame_with_crop=AsyncMock(side_effect=make)
        async def legacy(s,q): return make(s,q)[0]
        picker.frame=legacy; picker.close=AsyncMock()
        factory=Mock(return_value=picker)
        s=Session(ws,MAG,Services(AsyncMock(return_value=consent),Mock(),factory,{}))
        return s,picker
    start={"type":"START","seq":1,"formats":["jpeg"],"mode":"PIXEL_1X"}
    extension={"crop_v":1,"source_crop":[512,512]}
    for metadata in (False, True):
        s,capture=launch()
        try:
            await s.operate({**start,**(extension if metadata else {})})
            ack=json.loads(s.ws.send.await_args.args[0])
            assert ack["type"]=="STARTED" and ack["resource"]==r"\\.\DISPLAY42"
            assert ("source_crop" in ack)==metadata and ("crop_v" in ack)==metadata
            capture.frame_with_crop.assert_not_awaited()
            observed=[]; received=asyncio.Event()
            async def write(item):
                observed.append(item)
                if isinstance(item,bytes): received.set()
            s.channel=Mock(); s.channel.send=write
            s.producer=asyncio.create_task(s.produce())
            await asyncio.wait_for(received.wait(),2)
            await s.operate({"type":"FREEZE","seq":2})
            frozen=json.loads(s.ws.send.await_args.args[0])
            assert frozen==({"type":"FROZEN","seq":2,"last_frame":0} if metadata else {"type":"FROZEN","seq":2})
            assert len(observed)==(2 if metadata else 1)
            if metadata:
                assert json.loads(observed[0])["type"]=="CROP1"
                assert device_results(check,"jpeg",s.stream,(512,512),[(observed[0].encode(),observed[1])])==[
                    "-44 30 -300 -226 512 512 0 0 0"]
            assert observed[-1]==make(s.stream,0)[0]
            capture.pause.assert_called_once()
        finally:
            await s.finish()
    for change,allowed in [({"source_crop":[256,256]},True),({"crop_v":True},True),
                           ({"source_crop":[True,512]},True),({},False)]:
        s,capture=launch(allowed)
        try:
            await s.operate({**start,**extension,**change})
            assert not s.active and json.loads(s.ws.send.await_args.args[0])["type"]=="DENIED"
            capture.open.assert_not_awaited()
        finally: await s.finish()
    s,_=launch()
    with patch("broker.receive",AsyncMock(return_value={"type":"STOP","seq":1})):
        await s.control()
    assert json.loads(s.ws.send.await_args_list[0].args[0])["crop_v"]==1
    print("Broker: legacy binary unchanged, negotiated ACK, ordered pair passed C receiver; Freeze fence, denial and invalid requests passed.")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--crop-check",type=Path,required=True)
    args=parser.parse_args()
    check_device(args.crop_check)
    asyncio.run(broker_checks(args.crop_check))
