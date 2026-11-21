// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Exercise the actual read-only and pattern target resolvers with synthetic Windows responses.
#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cwchar>
#include <fcntl.h>
#include <io.h>

static std::array<DISPLAYCONFIG_PATH_INFO, 2> fixture_paths;
static std::array<DISPLAYCONFIG_MODE_INFO, 4> fixture_modes;
static const wchar_t *gdi_name = L"DISPLAY8";
static const wchar_t *board_id = L"USB\\VID_303A&PID_2987\\000000000001";
static unsigned config_calls;

static LONG fixture_sizes(UINT32 flags, UINT32 *paths, UINT32 *modes)
{
    ++config_calls;
    assert(flags == QDC_ONLY_ACTIVE_PATHS);
    *paths = fixture_paths.size();
    *modes = fixture_modes.size();
    return ERROR_SUCCESS;
}

static LONG fixture_query(UINT32, UINT32 *, DISPLAYCONFIG_PATH_INFO *paths, UINT32 *,
                          DISPLAYCONFIG_MODE_INFO *modes, DISPLAYCONFIG_TOPOLOGY_ID *)
{
    std::copy(fixture_paths.begin(), fixture_paths.end(), paths);
    std::copy(fixture_modes.begin(), fixture_modes.end(), modes);
    return ERROR_SUCCESS;
}

static LONG fixture_name(DISPLAYCONFIG_DEVICE_INFO_HEADER *header)
{
    if (header->adapterId.LowPart != 2) return ERROR_NOT_FOUND;
    if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME) {
        std::wcscpy(reinterpret_cast<DISPLAYCONFIG_TARGET_DEVICE_NAME *>(header)->monitorDevicePath, L"fixture");
        return ERROR_SUCCESS;
    }
    if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME) {
        std::wcscpy(reinterpret_cast<DISPLAYCONFIG_SOURCE_DEVICE_NAME *>(header)->viewGdiDeviceName, gdi_name);
        return ERROR_SUCCESS;
    }
    return ERROR_NOT_FOUND;
}

static CONFIGRET fixture_property(const wchar_t *, const DEVPROPKEY *, DEVPROPTYPE *type, PBYTE data, ULONG *, ULONG)
{
    *type = DEVPROP_TYPE_STRING;
    std::wcscpy(reinterpret_cast<wchar_t *>(data), board_id);
    return CR_SUCCESS;
}
static CONFIGRET fixture_locate(DEVINST *node, wchar_t *, ULONG) { *node = 1; return CR_SUCCESS; }
static CONFIGRET fixture_id(DEVINST, wchar_t *id, ULONG, ULONG) { std::wcscpy(id, board_id); return CR_SUCCESS; }
static CONFIGRET fixture_parent(DEVINST *, DEVINST, ULONG) { return CR_NO_SUCH_DEVNODE; }
static HMONITOR fixture_monitor(const RECT *, DWORD) { return reinterpret_cast<HMONITOR>(1); }
static bool wrong_dpi_rect;
static BOOL fixture_monitor_info(HMONITOR, MONITORINFOEXW *result)
{
    const auto &source = fixture_modes[1].sourceMode;
    result->rcMonitor = {source.position.x, source.position.y, source.position.x + static_cast<LONG>(source.width),
                         source.position.y + static_cast<LONG>(source.height)};
    if (wrong_dpi_rect) --result->rcMonitor.right;
    std::wcscpy(result->szDevice, gdi_name);
    return TRUE;
}

static unsigned enum_calls, test_calls;
static bool reassign, reject, no_modes;

static BOOL fixture_enum(const wchar_t *device, DWORD index, DEVMODEW *mode, DWORD flags)
{
    ++enum_calls;
    assert(device && wcscmp(device, L"DISPLAY8") == 0 && flags == 0 && mode->dmSize == sizeof(*mode));
    mode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    if (index != ENUM_CURRENT_SETTINGS && (no_modes || index >= 3)) {
        if (reassign) gdi_name = L"DISPLAY9";
        return FALSE;
    }
    mode->dmPelsWidth = index == 0 ? 640 : 1024;
    mode->dmPelsHeight = index == 0 ? 480 : 600;
    mode->dmBitsPerPel = 32;
    mode->dmDisplayFrequency = index == 2 ? 75 : 60;
    return TRUE;
}

static LONG fixture_test_mode(const wchar_t *device, DEVMODEW *mode, HWND window, DWORD flags, void *parameter)
{
    assert(device && wcscmp(device, L"DISPLAY8") == 0 && !window && flags == CDS_TEST && !parameter);
    assert(mode->dmPelsWidth == 1024 && mode->dmPelsHeight == 600);
    ++test_calls;
    return (reject || mode->dmDisplayFrequency == 75) ? DISP_CHANGE_BADMODE : DISP_CHANGE_SUCCESSFUL;
}

#define GetDisplayConfigBufferSizes fixture_sizes
#define EnumDisplaySettingsExW fixture_enum
#define ChangeDisplaySettingsExW fixture_test_mode
#define QueryDisplayConfig fixture_query
#define DisplayConfigGetDeviceInfo fixture_name
#define CM_Get_Device_Interface_PropertyW fixture_property
#define CM_Locate_DevNodeW fixture_locate
#define CM_Get_Device_IDW fixture_id
#define CM_Get_Parent fixture_parent
#define MonitorFromRect fixture_monitor
#define GetMonitorInfoW fixture_monitor_info
#define wmain monitor_cli
#include "usb_monitor_windows.cpp"
#undef wmain

static void test_runner_commands()
{
    HANDLE input, output;
    assert(CreatePipe(&input, &output, nullptr, 0));
    std::string pending;
    auto poll = [&](bool final = false) { return poll_command(input, pending, final); };
    auto send = [&](const char *text) {
        DWORD n = static_cast<DWORD>(std::strlen(text)), written;
        assert(WriteFile(output, text, n, &written, nullptr) && written == n);
    };
    assert(poll() == runner_command::waiting);
    send("FI");
    assert(poll() == runner_command::waiting);
    send("NAL\r");
    assert(poll() == runner_command::waiting);
    send("\n");
    assert(poll() == runner_command::final);
    assert(poll(true) == runner_command::waiting);
    send("FINAL\n");
    assert(poll(true) == runner_command::invalid);
    pending.clear();
    assert(poll() == runner_command::final);
    pending.clear();
    send("WRONG\n");
    assert(poll() == runner_command::invalid);
    CloseHandle(output);
    assert(poll() == runner_command::closed);
    CloseHandle(input);
}

static void test_final_pixels()
{
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), 640, -480, 1, 32, BI_RGB};
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    assert(dc && bitmap && bits);
    HGDIOBJ old = SelectObject(dc, bitmap);
    assert(old && old != HGDI_ERROR);
    const DWORD wrgb[] = {0xffffff, 0xff0000, 0x00ff00, 0x0000ff};
    for (bool marker : {false, true}) {
        assert(paint_pattern(dc, marker, true) && GdiFlush());
        const auto *pixels = static_cast<const DWORD *>(bits);
        for (int y = 0; y < 480; ++y)
            for (int x = 0; x < 640; ++x)
                assert((pixels[y * 640 + x] & 0xffffff) == wrgb[2 * (y >= 240) + (x >= 320)]);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static void test_latency_commands()
{
    HANDLE input,output;
    std::string pending;
    unsigned steps=0;
    auto open=[&]() { assert(CreatePipe(&input,&output,nullptr,0)); pending.clear(); steps=0; };
    auto send=[&](const char *text) {
        DWORD size=static_cast<DWORD>(std::strlen(text)),written=0;
        assert(WriteFile(output,text,size,&written,nullptr) && written==size);
    };
    auto poll=[&]() { return poll_latency_command(input,pending,steps); };
    open();
    assert(poll()==runner_command::waiting);
    send("NE");assert(poll()==runner_command::waiting);
    send("XT\nNEXT\n");assert(poll()==runner_command::next);steps++;
    assert(poll()==runner_command::next);steps++;
    for (;steps<16;steps++) {send("NEXT\n");assert(poll()==runner_command::next);}
    assert(poll()==runner_command::waiting);
    send("NEXT\n");assert(poll()==runner_command::invalid);
    CloseHandle(output);CloseHandle(input);
    for (const char *wrong : {"NEXT\r\n","NEXT ","FINAL\n","next\n"}) {
        open();send(wrong);
        assert(poll()==runner_command::invalid);
        CloseHandle(output);CloseHandle(input);
    }
    open();CloseHandle(output);assert(poll()==runner_command::invalid);CloseHandle(input);
    open();send("NEXT\nNE");CloseHandle(output);
    assert(poll()==runner_command::next);steps++;
    assert(poll()==runner_command::waiting && poll()==runner_command::invalid);
    CloseHandle(input);
    for (unsigned count : {1u,16u}) {
        open();
        for (;steps<count;steps++) {send("NEXT\n");assert(poll()==runner_command::next);}
        CloseHandle(output);assert(poll()==runner_command::closed);CloseHandle(input);
    }
}

static void test_latency_paints(pattern_size size)
{
    HDC dc=CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=size.width;
    info.bmiHeader.biHeight=-size.height;
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    void *bits=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    assert(dc && bitmap && bits);
    HGDIOBJ old=SelectObject(dc,bitmap);
    assert(old && old!=HGDI_ERROR);
    HANDLE input,output;
    assert(CreatePipe(&input,&output,nullptr,0));
    FILE *stream=_fdopen(_open_osfhandle(reinterpret_cast<intptr_t>(output),_O_WRONLY|_O_BINARY),"wb");
    assert(stream);
    pattern_run run{};
    run.latency=true;
    run.size=size;
    assert(QueryPerformanceFrequency(&run.frequency) && run.frequency.QuadPart>0);
    long long last=0;
    for (run.step=0;run.step<=16;run.step++) {
        LARGE_INTEGER before{},stamp{},after{};
        assert(QueryPerformanceCounter(&before) && paint_validated_pattern(dc,run,stamp) && QueryPerformanceCounter(&after));
        if (run.step) assert(stamp.QuadPart>=before.QuadPart && stamp.QuadPart<=after.QuadPart && stamp.QuadPart>last);
        else assert(!stamp.QuadPart);
        last=stamp.QuadPart;
        const DWORD rgbw[]={0xff0000,0x00ff00,0x0000ff,0xffffff};
        for (int y=0;y<size.height;y++) for (int x=0;x<size.width;x++) {
            unsigned cell=2*(y>=size.height/2)+(x>=size.width/2);
            assert((static_cast<const DWORD*>(bits)[y*size.width+x]&0xffffff)==rgbw[(cell+(run.step%2 ? 3 : 0))%4]);
        }
        assert(report_step(run,stamp,stream));
        DWORD available=0;
        assert(PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr));
        if (run.step) {
            char expected[120],actual[120];
            int len=std::snprintf(expected,sizeof(expected),"PATTERN_STEP %u %s %lld %lld\n",
                                  run.step,run.step%2 ? "WRGB" : "RGBW",stamp.QuadPart,run.frequency.QuadPart);
            DWORD read=0;
            assert(static_cast<int>(available)==len && ReadFile(input,actual,available,&read,nullptr) &&
                   read==available && !std::memcmp(actual,expected,read));
        } else assert(!available);
        // Windows may repaint the current window without another runner command.
        LARGE_INTEGER unchanged{};
        assert(paint_validated_pattern(dc,run,unchanged) && !unchanged.QuadPart && report_step(run,unchanged,stream));
        assert(PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr) && !available);
    }
    std::fclose(stream);CloseHandle(input);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}

static void check_latency_target_stdout(int argc,wchar_t **argv,const char *expected_origin,const char *mode)
{
    int pipe_fds[2];
    assert(_pipe(pipe_fds,4096,_O_TEXT)==0);
    const int output=_fileno(stdout);
    assert(std::fflush(stdout)==0);
    const int saved=_dup(output);
    assert(saved!=-1 && _dup2(pipe_fds[1],output)==0);
    _close(pipe_fds[1]);
    int status=monitor_cli(argc,argv);
    assert(std::fflush(stdout)==0 && _dup2(saved,output)==0);
    _close(saved);
    std::string report;
    char chunk[256];
    int n;
    while ((n=_read(pipe_fds[0],chunk,sizeof(chunk)))>0) report.append(chunk,n);
    _close(pipe_fds[0]);
    assert(status==0 && n==0);

    const std::string source=std::string(R"(board GDI source=\\.\DISPLAY27 origin=)")+expected_origin+"\n";
    const std::string last=std::string("LATENCY_TARGET ")+mode+"\n";
    const auto offset=report.find(source);
    if (offset==std::string::npos) std::fprintf(stderr,"Missing '%s' in CLI stdout:\n%s",source.c_str(),report.c_str());
    assert(offset!=std::string::npos && (offset==0 || report[offset-1]=='\n'));
    assert(report.size()>=offset+source.size()+last.size());
    const auto end_line=report.size()-last.size();
    assert(report[end_line-1]=='\n' && report.compare(end_line,last.size(),last)==0);
}

static void test_latency_target()
{
    wchar_t cmd[]=L"probe",serial[]=L"000000000001",combined[]=L"--composite";
    wchar_t verify[]=L"--latency-target",native[]=L"1024x600",vga[]=L"640x480",bad[]=L"1024X600";
    wchar_t *args[]={cmd,serial,verify,native};
    wchar_t *controls[]={cmd,combined,serial,verify,native};
    auto &source=fixture_modes[1].sourceMode;
    auto &signal=fixture_modes[3].targetMode.targetVideoSignalInfo.activeSize;
    auto &target=fixture_paths[1].targetInfo;
    unsigned original_calls=config_calls;
    args[3]=bad;
    assert(monitor_cli(4,args)==2 && monitor_cli(3,args)==2 && original_calls==config_calls);
    args[3]=vga;
    const wchar_t *original_name=gdi_name;
    gdi_name=LR"(\\.\DISPLAY27)";
    check_latency_target_stdout(4,args,"(1920,0)","640x480");
    signal={1024,600};assert(monitor_cli(4,args)==1);
    args[3]=native;assert(monitor_cli(4,args)==1);
    source.width=1024;source.height=600;assert(monitor_cli(4,args)==0);
    const wchar_t *standalone=board_id;
    board_id=L"USB\\VID_303A&PID_2986\\000000000001";
    source.position={-1024,-100};
    check_latency_target_stdout(5,controls,"(-1024,-100)","1024x600");
    source.position={1920,0};
    gdi_name=original_name;
    assert(monitor_cli(4,args)==1);
    board_id=standalone;
    signal={640,480};assert(monitor_cli(4,args)==1);
    signal={0,0};assert(monitor_cli(4,args)==1);
    signal={1024,600};
    target.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;assert(monitor_cli(4,args)==1);
    target.modeInfoIdx=3;
    ++fixture_modes[3].id;assert(monitor_cli(4,args)==1);--fixture_modes[3].id;
    ++fixture_modes[1].adapterId.LowPart;assert(monitor_cli(4,args)==1);--fixture_modes[1].adapterId.LowPart;
    target.rotation=DISPLAYCONFIG_ROTATION_ROTATE180;assert(monitor_cli(4,args)==1);
    target.rotation=DISPLAYCONFIG_ROTATION_IDENTITY;
    wrong_dpi_rect=true;assert(monitor_cli(4,args)==1);wrong_dpi_rect=false;
    source.position.x=1919;assert(monitor_cli(4,args)==1);source.position.x=1920;
    const auto original=fixture_paths[0].sourceInfo;
    fixture_paths[0].sourceInfo=fixture_paths[1].sourceInfo;assert(monitor_cli(4,args)==1);
    fixture_paths[0].sourceInfo=original;
    assert(monitor_cli(4,args)==0 && test_calls==0 && enum_calls==0);
    source.width=640;source.height=480;signal={640,480};
    std::puts("PASS latency-target stdout: validated GDI source and origin, final VGA/native marker");
    std::puts("PASS exact VGA/native read-only latency qualification, both USB identities, missing/mismatched timing, DPI, rotation, overlap and clone");
}

int main()
{
    wchar_t command[] = L"usb-monitor", version[] = L"--pattern-version";
    wchar_t *argv[] = {command, version};
    assert(monitor_cli(2, argv) == 0 && config_calls == 0);
    wchar_t final_version[] = L"--final-pattern-version";
    argv[1] = final_version;
    assert(monitor_cli(2, argv) == 0 && config_calls == 0);
    test_runner_commands();
    test_final_pixels();
    wchar_t latency_version[]=L"--latency-pattern-version";
    argv[1]=latency_version;
    assert(monitor_cli(2,argv)==0 && config_calls==0);
    wchar_t native_version[]=L"--native-latency-version";
    argv[1]=native_version;
    assert(monitor_cli(2,argv)==0 && config_calls==0);
    test_latency_commands();
    test_latency_paints({640,480});
    test_latency_paints({1024,600});
    for (unsigned i = 0; i < 2; ++i) {
        auto &path = fixture_paths[i];
        path.sourceInfo.adapterId.LowPart = i + 1;
        path.targetInfo.adapterId.LowPart = i + 1;
        path.sourceInfo.modeInfoIdx = i;
        path.targetInfo.modeInfoIdx = i + 2;
        path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
        fixture_modes[i].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        fixture_modes[i].adapterId = path.sourceInfo.adapterId;
        fixture_modes[i].sourceMode.width = i ? 640 : 1920;
        fixture_modes[i].sourceMode.height = i ? 480 : 1080;
        fixture_modes[i + 2].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
        fixture_modes[i + 2].adapterId = path.targetInfo.adapterId;
        fixture_modes[i + 2].targetMode.targetVideoSignalInfo.activeSize = {i ? 640u : 1920u, i ? 480u : 1080u};
    }
    auto &x = fixture_modes[1].sourceMode.position.x;
    auto &target = fixture_paths[1].targetInfo;
    x = 1920;
    test_latency_target();
    pattern_target original;
    assert(inspect_desktop(board_id) == 0 && inspect_desktop(board_id, &original) == 0);
    auto &source = fixture_modes[1].sourceMode;
    source.width = 1024;
    source.height = 600;
    wchar_t serial[] = L"000000000001", list[] = L"--modes", test[] = L"--test-mode";
    wchar_t wide[] = L"1024", high[] = L"600", invalid[] = L"640x";
    wchar_t *mode_argv[] = {command, serial, list, wide, high};
    assert(monitor_cli(3, mode_argv) == 0 && test_calls == 0);
    wchar_t combined[] = L"--composite";
    wchar_t *combined_argv[] = {command, combined, serial, list};
    assert(monitor_cli(4, combined_argv) == 1);
    const wchar_t *single = board_id;
    board_id = L"USB\\VID_303A&PID_2986\\000000000001";
    assert(monitor_cli(4, combined_argv) == 0 && monitor_cli(3, mode_argv) == 1);
    board_id = single;
    mode_argv[2] = test;
    assert(monitor_cli(5, mode_argv) == 0 && test_calls == 2);
    reject = true;
    assert(monitor_cli(5, mode_argv) == 1 && test_calls == 4);
    reject = false;
    assert(display_modes(board_id, 800, 600) == 1 && test_calls == 4);
    unsigned calls = enum_calls;
    mode_argv[3] = invalid;
    assert(monitor_cli(5, mode_argv) == 2 && enum_calls == calls);
    assert(display_modes(L"USB\\VID_303A&PID_2987\\000000000009", 0, 0) == 1 && enum_calls == calls);
    x = 100;
    assert(display_modes(board_id, 0, 0) == 1 && enum_calls == calls);
    x = 1920;
    reassign = true;
    assert(display_modes(board_id, 0, 0) == 1);
    gdi_name = L"DISPLAY8";
    reassign = false;
    no_modes = true;
    assert(display_modes(board_id, 0, 0) == 1);
    no_modes = false;
    std::puts("PASS board-only offered modes, non-applying CDS_TEST, missing, declined and reassigned sources");
    source.width = 640;
    source.height = 480;
    board_id=L"USB\\VID_303A&PID_2986\\000000000001";
    pattern_target controls_target,wrong_parent;
    assert(inspect_desktop(board_id,&controls_target)==0 && same_target(controls_target,original));
    assert(inspect_desktop(single,&wrong_parent)==1);
    board_id=single;
    for (LONG overlap : {0, 1919}) {
        x = overlap;
        pattern_target clone;
        assert(inspect_desktop(board_id) == 1 && inspect_desktop(board_id, &clone) == 1);
    }
    x = -640;
    pattern_target left;
    assert(inspect_desktop(board_id) == 0 && inspect_desktop(board_id, &left) == 0 && !same_target(left, original));
    x = 1920;
    target.rotation = DISPLAYCONFIG_ROTATION_ROTATE180;
    pattern_target rotated;
    assert(inspect_desktop(board_id) == 0 && inspect_desktop(board_id, &rotated) == 1);
    target.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    fixture_modes[3].targetMode.targetVideoSignalInfo.activeSize = {1024, 600};
    pattern_target scaled;
    assert(inspect_desktop(board_id) == 0 && inspect_desktop(board_id, &scaled) == 1);
    fixture_modes[3].targetMode.targetVideoSignalInfo.activeSize = {640, 480};
    gdi_name = L"DISPLAY19";
    pattern_target reassigned;
    assert(inspect_desktop(board_id, &reassigned) == 0 && !same_target(reassigned, original));
    x = 0;
    fixture_paths[1].sourceInfo = fixture_paths[0].sourceInfo;
    pattern_target same_source_clone;
    assert(inspect_desktop(board_id) == 1 && inspect_desktop(board_id, &same_source_clone) == 1);
    std::puts("PASS extended, overlapping, cloned, rotated, scaled and reassigned synthetic board desktops");
    std::puts("PASS runner command fragments, invalid input, EOF and 640x480 static WRGB pixels");
    std::puts("PASS latency protocol, strict fragmented/buffered NEXT, 16 transitions, static quadrant pixels, one flushed QPC record per step");
}
