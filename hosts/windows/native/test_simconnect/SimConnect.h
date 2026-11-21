// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Offline compile-time API fixture only. The product includes Microsoft's SDK header.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

enum {SIMCONNECT_RECV_ID_QUIT=1,SIMCONNECT_RECV_ID_EXCEPTION,SIMCONNECT_RECV_ID_SIMOBJECT_DATA,
      SIMCONNECT_OBJECT_ID_USER=0,SIMCONNECT_PERIOD_ONCE=1,SIMCONNECT_DATATYPE_FLOAT64=4,
      SIMCONNECT_GROUP_PRIORITY_HIGHEST=1,SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY=1};
struct SIMCONNECT_RECV { DWORD dwID; };
struct SIMCONNECT_RECV_SIMOBJECT_DATA { DWORD dwID,dwRequestID,dwDefineID,dwDefineCount,dwData; };
struct FakePacket { SIMCONNECT_RECV_SIMOBJECT_DATA header; DWORD extra[12]; };
static FakePacket fixture_packet{};
static unsigned fixture_indices[6],fixture_count=0;

static HRESULT SimConnect_Open(HANDLE *handle,const char*,HWND,DWORD,HANDLE,DWORD) {
    if (!std::getenv("KVMPAD_TEST_SIM_PHASE")) return E_FAIL;
    *handle=(HANDLE)&fixture_packet;
    return S_OK;
}
static HRESULT SimConnect_Close(HANDLE) { return S_OK; }
static HRESULT SimConnect_AddToDataDefinition(HANDLE,DWORD def,const char*var,const char*unit,DWORD kind) {
    static const char* vars[]={"AIRSPEED INDICATED","INDICATED ALTITUDE","PLANE HEADING DEGREES MAGNETIC",
                              "VERTICAL SPEED","PLANE PITCH DEGREES","PLANE BANK DEGREES"};
    static const char* units[]={"knots","feet","degrees","feet per minute","degrees","degrees"};
    if (def!=1 || kind!=SIMCONNECT_DATATYPE_FLOAT64 || fixture_count>=6) return E_FAIL;
    for (unsigned i=0;i<6;i++) if (!std::strcmp(var,vars[i]) && !std::strcmp(unit,units[i])) {
        if (fixture_count && i<=fixture_indices[fixture_count-1]) return E_FAIL;
        fixture_indices[fixture_count++]=i;
        return S_OK;
    }
    return E_FAIL;
}
static HRESULT SimConnect_RequestDataOnSimObject(HANDLE,DWORD id,DWORD def,DWORD obj,DWORD period) {
    if (!fixture_count || def!=1 || obj!=SIMCONNECT_OBJECT_ID_USER || period!=SIMCONNECT_PERIOD_ONCE) return E_FAIL;
    const char* path=std::getenv("KVMPAD_TEST_SIM_PHASE");
    FILE *f=std::fopen(path,"rb");
    if (!f) return E_FAIL;
    int phase=std::fgetc(f);
    std::fclose(f);
    if (phase!='A' && phase!='B' && phase!='E' && phase!='F') return E_FAIL;
    fixture_packet.header={SIMCONNECT_RECV_ID_SIMOBJECT_DATA,id,def,fixture_count,0};
    const double first[]={123.5,5320.,-2.,-450.,-8.,11.};
    const double second[]={144.25,6280.,367.,780.,3.,-14.};
    unsigned mask=0;
    unsigned char* bytes=(unsigned char*)&fixture_packet+offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA,dwData);
    for (unsigned i=0;i<fixture_count;i++) {
        mask |= 1u<<fixture_indices[i];
        double value=(phase=='B'?second:first)[fixture_indices[i]];
        if (phase=='E' && fixture_indices[i]==5) value=std::numeric_limits<double>::infinity();
        std::memcpy(bytes+i*sizeof(double),&value,sizeof(value));
    }
    if (phase=='F') fixture_packet.header.dwID=SIMCONNECT_RECV_ID_EXCEPTION;
    if (const char* trace=std::getenv("KVMPAD_TEST_SIM_TRACE")) {
        f=std::fopen(trace,"ab");
        if (!f) return E_FAIL;
        std::fputc(mask,f);
        std::fclose(f);
    }
    return S_OK;
}
static HRESULT SimConnect_GetNextDispatch(HANDLE,SIMCONNECT_RECV** out,DWORD* size) {
    *out=(SIMCONNECT_RECV*)&fixture_packet;
    *size=(DWORD)(offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA,dwData)+fixture_count*sizeof(double));
    return S_OK;
}
static HRESULT SimConnect_MapClientEventToSimEvent(HANDLE,DWORD,const char*) { return E_FAIL; }
static HRESULT SimConnect_TransmitClientEvent(HANDLE,DWORD,DWORD,DWORD,DWORD,DWORD) { return E_FAIL; }
