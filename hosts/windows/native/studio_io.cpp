// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <array>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Collection { std::wstring path, serial; };

bool identity(HANDLE file, std::wstring& serial) {
    HIDD_ATTRIBUTES a{sizeof(a)};
    if (!HidD_GetAttributes(file,&a) || a.VendorID!=0x303a || a.ProductID!=0x400c ||
        a.VersionNumber!=0x0101) return false;
    PHIDP_PREPARSED_DATA pp=nullptr;
    if (!HidD_GetPreparsedData(file,&pp)) return false;
    HIDP_CAPS caps{};
    bool ok=HidP_GetCaps(pp,&caps)==HIDP_STATUS_SUCCESS && caps.UsagePage==0xff61 && caps.Usage==1 &&
        caps.InputReportByteLength==64 && caps.OutputReportByteLength==64 && caps.FeatureReportByteLength==16;
    HidD_FreePreparsedData(pp);
    wchar_t buffer[64]{};
    if (!ok || !HidD_GetSerialNumberString(file,buffer,sizeof(buffer)) || buffer[63]) return false;
    serial=buffer;
    if (serial.size()!=12) return false;
    for (auto c : serial) if (!((c>=L'0' && c<=L'9') || (c>=L'A' && c<=L'F'))) return false;
    return true;
}

std::vector<Collection> enumerate() {
    std::vector<Collection> result;
    GUID guid{}; HidD_GetHidGuid(&guid);
    HDEVINFO set=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (set==INVALID_HANDLE_VALUE) return result;
    for (DWORD n=0;;n++) {
        SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
        if (!SetupDiEnumDeviceInterfaces(set,nullptr,&guid,n,&data)) break;
        DWORD bytes=0;
        SetupDiGetDeviceInterfaceDetailW(set,&data,nullptr,0,&bytes,nullptr);
        if (bytes<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || bytes>8192) continue;
        std::vector<BYTE> storage(bytes);
        auto info=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        info->cbSize=sizeof(*info);
        if (!SetupDiGetDeviceInterfaceDetailW(set,&data,info,bytes,nullptr,nullptr)) continue;
        std::wstring path=info->DevicePath, lower=path;
        for (auto& ch : lower) ch=static_cast<wchar_t>(towlower(ch));
        if (lower.find(L"vid_303a&pid_400c&mi_03")==std::wstring::npos) continue;
        HANDLE probe=CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if (probe==INVALID_HANDLE_VALUE) continue;
        std::wstring serial;
        if (identity(probe,serial)) result.push_back({path,serial});
        CloseHandle(probe);
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

using Report=std::array<unsigned char,64>;
bool transfer(HANDLE file, Report& report, bool write, std::atomic_bool& done) {
    OVERLAPPED op{};
    op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!op.hEvent) return false;
    DWORD count=0;
    BOOL ok=write ? WriteFile(file,report.data(),64,&count,&op) : ReadFile(file,report.data(),64,&count,&op);
    if (!ok && GetLastError()==ERROR_IO_PENDING) {
        auto start=GetTickCount64();
        while (!done && (!write || GetTickCount64()-start<800)) {
            DWORD wait=WaitForSingleObject(op.hEvent,100);
            if (wait!=WAIT_TIMEOUT) break;
        }
        CancelIoEx(file,&op);
        ok=GetOverlappedResult(file,&op,&count,TRUE);
    }
    CloseHandle(op.hEvent);
    return ok && count==64;
}

int serve(HANDLE device) {
    std::array<unsigned char,16> feature{};
    feature[0]=0x24;
    if (!HidD_GetFeature(device,feature.data(),static_cast<ULONG>(feature.size())) ||
        feature[0]!=0x24 || feature[1]!=1 || feature[5]!=8 || feature[6]!=51) return 3;
    if (fwrite(feature.data(),1,16,stdout)!=16 || fflush(stdout)) return 3;
    std::atomic_bool done=false;
    std::thread writer([&] {
        HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
        Report packet{};
        while (!done) {
            DWORD available=0;
            if (!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr)) break;
            if (available<64) { Sleep(4); continue; }
            DWORD got=0;
            if (!ReadFile(input,packet.data(),64,&got,nullptr) || got!=64 || packet[0]!=0x23 ||
                !transfer(device,packet,true,done)) break;
        }
        done=true;
    });
    Report packet{};
    while (!done && transfer(device,packet,false,done)) {
        if (packet[0]!=0x22 || fwrite(packet.data(),1,64,stdout)!=64 || fflush(stdout)) break;
    }
    done=true;
    CancelIoEx(device,nullptr);
    writer.join();
    return 0;
}
}

int wmain(int argc,wchar_t** argv) {
    bool list=argc==2 && std::wstring(argv[1])==L"--list";
    if (!list && (argc!=3 || std::wstring(argv[1])!=L"--serial")) return 2;
    auto entries=enumerate();
    if (list) { for (auto& e : entries) wprintf(L"%ls\n",e.serial.c_str()); return 0; }
    std::wstring path;
    for (auto& e : entries) if (e.serial==argv[2]) {
        if (!path.empty()) return 3;
        path=e.path;
    }
    if (path.empty()) return 3;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                            nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (file==INVALID_HANDLE_VALUE) return 3;
    _setmode(_fileno(stdin),_O_BINARY); _setmode(_fileno(stdout),_O_BINARY);
    std::wstring serial;
    int rc=identity(file,serial) && serial==argv[2] ? serve(file) : 3;
    CloseHandle(file);
    return rc;
}
