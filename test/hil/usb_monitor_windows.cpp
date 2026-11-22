// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

// Inspect active desktop topology or draw a local test pattern on the resolved board target.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <usbioctl.h>
#include "usb_board_port.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static bool belongs_to_board(const wchar_t *device_path, const std::wstring &board_id)
{
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    ULONG size = sizeof(id);
    DEVPROPTYPE type;
    if (!*device_path || CM_Get_Device_Interface_PropertyW(device_path, &DEVPKEY_Device_InstanceId,
            &type, reinterpret_cast<PBYTE>(id), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        return false;
    DEVINST node;
    if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    do {
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
        if (_wcsicmp(id, board_id.c_str()) == 0) return true;
        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return false;
        node = parent;
    } while (true);
}

static bool same_source(const DISPLAYCONFIG_PATH_SOURCE_INFO &a, const DISPLAYCONFIG_PATH_SOURCE_INFO &b)
{
    return a.id == b.id && a.adapterId.HighPart == b.adapterId.HighPart &&
           a.adapterId.LowPart == b.adapterId.LowPart;
}

static std::vector<unsigned char> descriptor(HANDLE hub, ULONG port, USHORT value, USHORT language = 0)
{
    alignas(USB_DESCRIPTOR_REQUEST) std::array<unsigned char, sizeof(USB_DESCRIPTOR_REQUEST) + 255> packet{};
    auto request = reinterpret_cast<USB_DESCRIPTOR_REQUEST *>(packet.data());
    request->ConnectionIndex = port;
    request->SetupPacket.bmRequest = 0x80;
    request->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    request->SetupPacket.wValue = value;
    request->SetupPacket.wIndex = language;
    request->SetupPacket.wLength = 255;
    DWORD size = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, request, sizeof(packet),
                         request, sizeof(packet), &size, nullptr) ||
        size < sizeof(*request) || size > sizeof(packet)) return {};
    return {packet.begin() + sizeof(*request), packet.begin() + size};
}

static bool matches_string(const std::vector<unsigned char> &data, const std::wstring &expected)
{
    return data.size() == 2 + expected.size() * 2 && data[0] == data.size() && data[1] == 3 &&
           !std::memcmp(data.data() + 2, expected.data(), expected.size() * 2);
}

static int inspect_usb(std::wstring instance, const wchar_t *serial)
{
    ULONG port = 0;
    HANDLE hub = open_board_hub(instance.data(), port);
    if (hub == INVALID_HANDLE_VALUE) return 1;
    auto config = descriptor(hub, port, 0x0200);
    auto product = descriptor(hub, port, 0x0302, 0x0409);
    auto interface_name = descriptor(hub, port, 0x0304, 0x0409);
    auto serial_number = descriptor(hub, port, 0x0303, 0x0409);
    CloseHandle(hub);
    // One vendor interface with full-speed 64-byte bulk OUT and IN pipes.
    const std::vector<unsigned char> expected{
        9,2,32,0,1,1,0,0x80,250, 9,4,0,0,2,0xff,0,0,4,
        7,5,0x01,2,64,0,0, 7,5,0x81,2,64,0,0};
    const std::wstring caps = L"esp32s3udisp0_R640x480_Ejpg4_Fps10_Bl96";
    bool product_ok = matches_string(product, caps), interface_ok = matches_string(interface_name, caps);
    bool serial_ok = matches_string(serial_number, serial);
    std::printf("USB 303a:2987 port=%lu config=", port);
    for (auto byte : config) std::printf("%02x", byte);
    std::printf(" product=%d interface=%d serial=%d\n", product_ok, interface_ok, serial_ok);
    return config == expected && product_ok && interface_ok && serial_ok ? 0 : 1;
}

using path_key = std::array<long long, 17>;
struct pattern_size { LONG width = 640, height = 480; };

static bool latency_size(const wchar_t *value, pattern_size &size)
{
    if (!wcscmp(value, L"640x480")) { size = {640,480}; return true; }
    if (!wcscmp(value, L"1024x600")) { size = {1024,600}; return true; }
    return false;
}

struct pattern_target {
    RECT area{};
    std::wstring device;
    std::vector<path_key> topology;
    DISPLAYCONFIG_2DREGION signal{};
};

static bool isolated_area(const RECT &area, const std::vector<RECT> &desktops)
{
    unsigned overlaps = 0;
    for (const auto &desktop : desktops) {
        RECT overlap;
        if (IntersectRect(&overlap, &area, &desktop)) ++overlaps;
    }
    return overlaps == 1;
}

static bool source_rect(const DISPLAYCONFIG_SOURCE_MODE &mode, RECT &area)
{
    const long long right = static_cast<long long>(mode.position.x) + mode.width;
    const long long bottom = static_cast<long long>(mode.position.y) + mode.height;
    if (!mode.width || !mode.height || right > std::numeric_limits<LONG>::max() ||
        bottom > std::numeric_limits<LONG>::max()) return false;
    area = {mode.position.x, mode.position.y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
    return true;
}

static int inspect_desktop(const std::wstring &board_id, pattern_target *target = nullptr,
                           bool mode_query = false, const pattern_size *required = nullptr)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 path_count, mode_count;
    LONG rc;
    do {
        rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (rc != ERROR_SUCCESS) break;
        paths.resize(path_count);
        modes.resize(mode_count);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                &mode_count, modes.data(), nullptr);
    } while (rc == ERROR_INSUFFICIENT_BUFFER);
    if (rc != ERROR_SUCCESS) {
        std::fprintf(stderr, "Cannot inspect the active Windows desktop in this session: %ld\n", rc);
        return 2;
    }
    paths.resize(path_count);
    std::vector<RECT> desktops;
    unsigned found = 0;
    bool extended = false, upright_signal = false;
    RECT board_area{};
    for (const auto &path : paths) {
        auto index = path.sourceInfo.modeInfoIdx;
        const auto &s = path.sourceInfo;
        const auto &t = path.targetInfo;
        if (index >= mode_count || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) return 1;
        const auto &mode = modes[index].sourceMode;
        RECT area;
        if (!source_rect(mode, area)) return 1;
        desktops.push_back(area);
        DISPLAYCONFIG_2DREGION signal{};
        if (t.modeInfoIdx < mode_count && modes[t.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
            signal = modes[t.modeInfoIdx].targetMode.targetVideoSignalInfo.activeSize;
        if (target) {
            target->topology.push_back({s.adapterId.HighPart, s.adapterId.LowPart, s.id,
                t.adapterId.HighPart, t.adapterId.LowPart, t.id, area.left, area.top, area.right, area.bottom,
                t.rotation, t.scaling, t.refreshRate.Numerator, t.refreshRate.Denominator, t.outputTechnology,
                signal.cx, signal.cy});
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME monitor{};
        monitor.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(monitor),
                          path.targetInfo.adapterId, path.targetInfo.id};
        DISPLAYCONFIG_ADAPTER_NAME adapter{};
        adapter.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME, sizeof(adapter),
                          path.targetInfo.adapterId, 0};
        bool matched = DisplayConfigGetDeviceInfo(&monitor.header) == ERROR_SUCCESS &&
                       belongs_to_board(monitor.monitorDevicePath, board_id);
        if (!matched && DisplayConfigGetDeviceInfo(&adapter.header) == ERROR_SUCCESS)
            matched = belongs_to_board(adapter.adapterDevicePath, board_id);
        if (!matched) continue;
        ++found;
        board_area = area;
        auto shared = std::count_if(paths.begin(), paths.end(), [&](const auto &other) {
            return same_source(path.sourceInfo, other.sourceInfo);
        });
        pattern_size size = required ? *required : pattern_size{};
        extended = shared == 1 && (mode_query || (paths.size() > 1 && mode.width == static_cast<UINT32>(size.width) &&
                                                   mode.height == static_cast<UINT32>(size.height)));
        upright_signal = t.rotation == DISPLAYCONFIG_ROTATION_IDENTITY &&
                         ((!required && !signal.cx && !signal.cy) || (signal.cx == mode.width && signal.cy == mode.height));
        if (required && upright_signal) {
            // Match the reported timing to this exact source and target on their adapters.
            const auto &source_mode = modes[index], &target_mode = modes[t.modeInfoIdx];
            upright_signal = source_mode.id == s.id && target_mode.id == t.id &&
                source_mode.adapterId.LowPart == s.adapterId.LowPart && source_mode.adapterId.HighPart == s.adapterId.HighPart &&
                target_mode.adapterId.LowPart == t.adapterId.LowPart && target_mode.adapterId.HighPart == t.adapterId.HighPart;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId,
                         path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
            if (target) {
                target->area = area;
                target->device = source.viewGdiDeviceName;
                target->signal = signal;
            } else {
                std::printf("board GDI source=%ls origin=(%ld,%ld)\n", source.viewGdiDeviceName,
                            mode.position.x, mode.position.y);
            }
        }
        if (!target) {
            std::printf("board desktop=%ux%u active_targets=%u targets_on_this_source=%td\n",
                        mode.width, mode.height, path_count, shared);
            const bool quarter_turn = t.rotation == DISPLAYCONFIG_ROTATION_ROTATE90 ||
                                      t.rotation == DISPLAYCONFIG_ROTATION_ROTATE270;
            if (signal.cx && signal.cy) {
                const bool same_size = signal.cx == (quarter_turn ? mode.height : mode.width) &&
                                       signal.cy == (quarter_turn ? mode.width : mode.height);
                std::printf("active_signal=%ux%u source_to_signal=%s ", signal.cx, signal.cy,
                            same_size ? "same-size" : "scaled");
            } else std::fputs("active_signal=unknown source_to_signal=unknown ", stdout);
            if (t.rotation >= DISPLAYCONFIG_ROTATION_IDENTITY && t.rotation <= DISPLAYCONFIG_ROTATION_ROTATE270)
                std::printf("rotation_degrees=%u ", 90u * (static_cast<unsigned>(t.rotation) - 1));
            else std::fputs("rotation_degrees=unknown ", stdout);
            std::printf("scaling_policy=%u\n", static_cast<unsigned>(t.scaling));
        }
    }
    extended = found == 1 && extended && isolated_area(board_area, desktops);
    if (target) {
        if (!extended || (!mode_query && !upright_signal) || target->device.empty()) return 1;
        if (!mode_query) {
            // The pattern places a window, so validate its per-monitor-DPI physical rectangle too.
            // Mode enumeration addresses the GDI source from DisplayConfig directly.
            MONITORINFOEXW monitor{};
            monitor.cbSize = sizeof(monitor);
            HMONITOR handle = MonitorFromRect(&target->area, MONITOR_DEFAULTTONULL);
            if (!handle || !GetMonitorInfoW(handle, &monitor) ||
                !EqualRect(&monitor.rcMonitor, &target->area) || target->device != monitor.szDevice) return 1;
        }
        std::sort(target->topology.begin(), target->topology.end());
        return 0;
    }
    if (extended) {
        std::puts("PASS: the requested board owns an independent 640x480 Windows desktop.");
        return 0;
    }
    std::printf("NOT VERIFIED: %u active targets matched the requested board.\n", found);
    return 1;
}

static bool on_input_desktop()
{
    HDESK current = GetThreadDesktop(GetCurrentThreadId());
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wchar_t current_name[256]{}, input_name[256]{};
    DWORD used, receives_input = FALSE;
    bool ok = current && input &&
              GetUserObjectInformationW(current, UOI_IO, &receives_input, sizeof(receives_input), &used) &&
              receives_input &&
              GetUserObjectInformationW(current, UOI_NAME, current_name, sizeof(current_name), &used) &&
              GetUserObjectInformationW(input, UOI_NAME, input_name, sizeof(input_name), &used) &&
              _wcsicmp(current_name, L"Default") == 0 && _wcsicmp(current_name, input_name) == 0;
    if (input) CloseDesktop(input);
    return ok;
}

struct pattern_run {
    std::wstring board;
    pattern_target target;
    bool stopped = false, painted = false, marker = false;
    bool desktop_unavailable = false;
    bool final_requested = false, final_painted = false;
    bool latency = false;
    pattern_size size{};
    unsigned step = 0, painted_step = 0;
    LARGE_INTEGER frequency{};
};

enum class runner_command { waiting, final, next, closed, invalid };

// The runner owns an anonymous stdin pipe. Read only bytes already present so display
// changes and the watchdog are still serviced between fragments of the command.
static runner_command poll_command(HANDLE pipe, std::string &pending, bool final_requested)
{
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? runner_command::closed : runner_command::invalid;
    if (!available) return runner_command::waiting;
    char bytes[7]; // FINAL\r\n
    if (final_requested || available > sizeof(bytes) - pending.size()) return runner_command::invalid;
    DWORD count = 0;
    if (!ReadFile(pipe, bytes, available, &count, nullptr) || !count) return runner_command::invalid;
    pending.append(bytes, count);
    if (pending == "FINAL\n" || pending == "FINAL\r\n") return runner_command::final;
    if (pending.size() <= 6 && std::memcmp("FINAL\r", pending.data(), pending.size()) == 0)
        return runner_command::waiting;
    return runner_command::invalid;
}

// Consume at most one NEXT line so that even buffered commands paint in sequence.
static runner_command poll_latency_command(HANDLE pipe, std::string &pending, unsigned steps)
{
    constexpr char line[] = "NEXT\n";
    char bytes[sizeof(line)-1];
    DWORD available = 0, count = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE && pending.empty() && steps
               ? runner_command::closed : runner_command::invalid;
    if (!available) return runner_command::waiting;
    if (steps >= 16) return runner_command::invalid;
    DWORD take = std::min(available, static_cast<DWORD>(sizeof(bytes) - pending.size()));
    if (!ReadFile(pipe, bytes, take, &count, nullptr) || !count) return runner_command::invalid;
    pending.append(bytes, count);
    if (std::memcmp(line, pending.data(), pending.size()) != 0) return runner_command::invalid;
    if (pending.size() < sizeof(bytes)) return runner_command::waiting;
    pending.clear();
    return runner_command::next;
}

static bool same_target(const pattern_target &current, const pattern_target &previous)
{
    return current.device == previous.device && current.topology == previous.topology &&
           EqualRect(&current.area, &previous.area);
}

static bool dimension(const wchar_t *arg, DWORD &value)
{
    const size_t length = wcslen(arg);
    if (!length || length > 5 || wcsspn(arg, L"0123456789") != length) return false;
    value = wcstoul(arg, nullptr, 10);
    return value > 0;
}

static int display_modes(const std::wstring &board_id, DWORD width, DWORD height)
{
    pattern_target board;
    if (inspect_desktop(board_id, &board, true) != 0) {
        std::fputs("Cannot identify one independent active source for this board serial.\n", stderr);
        return 1;
    }
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsExW(board.device.c_str(), ENUM_CURRENT_SETTINGS, &current, 0) ||
        current.dmPelsWidth != static_cast<DWORD>(board.area.right - board.area.left) ||
        current.dmPelsHeight != static_cast<DWORD>(board.area.bottom - board.area.top)) return 1;

    struct entry { DEVMODEW mode; LONG test; };
    std::vector<entry> rows;
    bool accepted = false;
    for (DWORD i = 0; ; ++i) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        // Flags zero exposes the modes Windows offers for this monitor. RAWMODE includes unsupported modes.
        if (!EnumDisplaySettingsExW(board.device.c_str(), i, &mode, 0)) break;
        if (i >= 4096 || !mode.dmPelsWidth || !mode.dmPelsHeight) return 1;
        LONG test = DISP_CHANGE_FAILED;
        if (width && mode.dmPelsWidth == width && mode.dmPelsHeight == height) {
            DEVMODEW trial = mode;
            test = ChangeDisplaySettingsExW(board.device.c_str(), &trial, nullptr, CDS_TEST, nullptr);
            accepted |= test == DISP_CHANGE_SUCCESSFUL;
        }
        rows.push_back({mode, test});
    }
    pattern_target after;
    if (rows.empty() || inspect_desktop(board_id, &after, true) != 0 || !same_target(board, after)) {
        std::fputs("No mode list or display mapping changed during enumeration.\n", stderr);
        return 1;
    }
    std::printf("board=%ls source=%ls current=%lux%lu hz=%lu bpp=%lu ", board_id.c_str(), board.device.c_str(),
                current.dmPelsWidth, current.dmPelsHeight, current.dmDisplayFrequency, current.dmBitsPerPel);
    if (board.signal.cx && board.signal.cy) std::printf("active_signal=%ux%u\n", board.signal.cx, board.signal.cy);
    else std::puts("active_signal=unknown");
    for (const auto &row : rows) {
        const auto &m = row.mode;
        std::printf("offered=%lux%lu hz=%lu bpp=%lu flags=%lu", m.dmPelsWidth, m.dmPelsHeight,
                    m.dmDisplayFrequency, m.dmBitsPerPel, m.dmDisplayFlags);
        if (width && m.dmPelsWidth == width && m.dmPelsHeight == height)
            std::printf(" CDS_TEST=%ld", row.test);
        std::putchar('\n');
    }
    if (width) std::printf("test=%lux%lu accepted=%d applied=0\n", width, height, accepted);
    return width && !accepted ? 1 : 0;
}

static bool target_still_valid(pattern_run &run)
{
    if (!on_input_desktop()) {
        if (!run.desktop_unavailable) {
            std::puts("PATTERN_UNAVAILABLE input_desktop");
            std::fflush(stdout);
        }
        run.desktop_unavailable = true;
        return false;
    }
    pattern_target current;
    return inspect_desktop(run.board, &current, false, run.latency ? &run.size : nullptr) == 0 &&
           same_target(current, run.target);
}

static bool window_fits(HWND window, const RECT &area)
{
    RECT actual, client;
    return GetWindowRect(window, &actual) && EqualRect(&actual, &area) &&
           GetClientRect(window, &client) && client.right == area.right-area.left && client.bottom == area.bottom-area.top;
}

static bool paint_pattern(HDC dc, bool marker, bool final_pattern = false, bool show_marker = true,
                          pattern_size size = {})
{
    const COLORREF colors[] = {RGB(255,0,0), RGB(0,255,0), RGB(0,0,255), RGB(255,255,255)};
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    for (int i = 0; i < 4; ++i) {
        const LONG half_w = size.width/2, half_h = size.height/2;
        const LONG x = (i % 2) * half_w, y = (i / 2) * half_h;
        RECT cell{x, y, x + half_w, y + half_h};
        int color = final_pattern ? (i + 3) % 4 : i;
        if (SetDCBrushColor(dc, colors[color]) == CLR_INVALID || !FillRect(dc, &cell, brush)) return false;
    }
    if (final_pattern || !show_marker) return true;
    RECT corner{0, 0, 32, 32};
    return SetDCBrushColor(dc, marker ? RGB(255,255,255) : RGB(0,0,0)) != CLR_INVALID &&
           FillRect(dc, &corner, brush);
}

static bool paint_validated_pattern(HDC dc, const pattern_run &run, LARGE_INTEGER &started)
{
    bool wrgb = run.latency ? (run.step % 2 != 0) : run.final_requested;
    if (run.latency && run.step != run.painted_step && !QueryPerformanceCounter(&started)) return false;
    return paint_pattern(dc, run.marker, wrgb, !run.latency, run.size) && GdiFlush();
}

static bool report_step(pattern_run &run, LARGE_INTEGER started, FILE *output)
{
    if (run.painted_step == run.step) return true;
    if (std::fprintf(output, "PATTERN_STEP %u %s %lld %lld\n", run.step, run.step % 2 ? "WRGB" : "RGBW",
                     started.QuadPart, run.frequency.QuadPart) < 0 || std::fflush(output) != 0) return false;
    run.painted_step = run.step;
    return true;
}

static LRESULT CALLBACK pattern_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto run = reinterpret_cast<pattern_run *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!run) return DefWindowProcW(window, msg, wparam, lparam);
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_WINDOWPOSCHANGING) {
        auto pos = reinterpret_cast<WINDOWPOS *>(lparam);
        if ((!(pos->flags & SWP_NOMOVE) && (pos->x != run->target.area.left || pos->y != run->target.area.top)) ||
            (!(pos->flags & SWP_NOSIZE) && (pos->cx != run->size.width || pos->cy != run->size.height))) {
            run->stopped = true;
            pos->flags = (pos->flags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
        }
        return 0;
    }
    if (msg == WM_DISPLAYCHANGE || msg == WM_DPICHANGED || msg == WM_CLOSE || msg == WM_ENDSESSION) {
        run->stopped = true;
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGED && IsWindowVisible(window) && !window_fits(window, run->target.area)) {
        run->stopped = true;
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        LARGE_INTEGER started{};
        bool ok = dc && !run->stopped && window_fits(window, run->target.area) &&
                  target_still_valid(*run) && paint_validated_pattern(dc, *run, started);
        EndPaint(window, &paint);
        if (!ok) {
            run->stopped = true;
            ShowWindow(window, SW_HIDE);
        } else if (!run->painted && IsWindowVisible(window)) {
            run->painted = true;
            std::printf("PATTERN_READY %ldx%ld RGBW\n", run->size.width, run->size.height);
            std::fflush(stdout);
        } else if (run->latency && IsWindowVisible(window)) {
            if (!report_step(*run, started, stdout)) run->stopped = true;
        } else if (run->final_requested && !run->final_painted && IsWindowVisible(window)) {
            run->final_painted = true;
            std::puts("PATTERN_FINAL 640x480 WRGB");
            std::fflush(stdout);
        }
        return 0;
    }
    return DefWindowProcW(window, msg, wparam, lparam);
}

static bool use_physical_coordinates()
{
    using set_dpi_context = DPI_AWARENESS_CONTEXT (WINAPI *)(DPI_AWARENESS_CONTEXT);
    auto set_dpi = reinterpret_cast<set_dpi_context>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    return set_dpi && set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

static int show_pattern(const std::wstring &board, bool final_mode, bool latency, pattern_size size)
{
    pattern_run run{board, {}};
    run.latency = latency;
    run.size = size;
    bool controlled = final_mode || latency;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (controlled && GetFileType(input) != FILE_TYPE_PIPE) {
        std::fprintf(stderr, "%s requires a runner stdin pipe.\n", latency ? "--pattern-latency" : "--pattern-final");
        return 1;
    }
    if (latency && (!QueryPerformanceFrequency(&run.frequency) || run.frequency.QuadPart <= 0)) return 1;
    if (!on_input_desktop()) {
        std::puts("PATTERN_UNAVAILABLE input_desktop");
        return 3;
    }
    if (!use_physical_coordinates() || inspect_desktop(board, &run.target, false, latency ? &size : nullptr) != 0) return 1;
    const ULONGLONG start = GetTickCount64();
    do {
        Sleep(200);
        if (!target_still_valid(run)) return run.desktop_unavailable ? 3 : 1;
    } while (GetTickCount64() - start < 2000);

    WNDCLASSW cls{};
    cls.lpfnWndProc = pattern_proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"KVMpadUsbMonitorPattern";
    if (!RegisterClassW(&cls)) return 1;
    const RECT area = run.target.area;
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        cls.lpszClassName, L"KVMpad USB monitor test", WS_POPUP, area.left, area.top, size.width, size.height,
        nullptr, nullptr, cls.hInstance, &run);
    bool shown = window && !run.stopped && window_fits(window, area) && target_still_valid(run) &&
                 SetWindowPos(window, HWND_TOPMOST, area.left, area.top, size.width, size.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const ULONGLONG visible_since = GetTickCount64();
    ULONGLONG last_validation = visible_since;
    std::string pending_command;
    bool runner_finished = false;
    while (shown && !run.stopped && GetTickCount64() - visible_since < (controlled ? 120000u : 30000u)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        ULONGLONG now = GetTickCount64();
        bool validate = !latency || now - last_validation >= 200;
        if (run.stopped || !window_fits(window, area) || (validate && !target_still_valid(run))) {
            run.stopped = true;
            break;
        }
        if (validate) last_validation = now;
        if (latency && !run.painted &&
            (!InvalidateRect(window, nullptr, FALSE) || !UpdateWindow(window) || !run.painted)) break;
        if (latency && run.painted) {
            auto command = poll_latency_command(input, pending_command, run.painted_step);
            if (command == runner_command::closed || command == runner_command::invalid) {
                runner_finished = command == runner_command::closed;
                break;
            }
            if (command == runner_command::next) {
                ++run.step;
                if (!InvalidateRect(window, nullptr, FALSE) || !UpdateWindow(window) ||
                    run.painted_step != run.step) break;
            }
        }
        if (final_mode && run.painted) {
            auto command = poll_command(input, pending_command, run.final_requested);
            if (command == runner_command::closed || command == runner_command::invalid) {
                runner_finished = command == runner_command::closed && run.final_painted;
                break;
            }
            if (command == runner_command::final) {
                run.final_requested = true;
                if (!InvalidateRect(window, nullptr, FALSE) || !UpdateWindow(window) || !run.final_painted)
                    break;
            }
        }
        if (!latency && !run.final_requested) {
            bool marker = ((GetTickCount64() - visible_since) / 500) % 2 != 0;
            if (run.marker != marker || !run.painted) {
                run.marker = marker;
                RECT corner{0, 0, 32, 32};
                InvalidateRect(window, run.painted ? &corner : nullptr, FALSE);
                UpdateWindow(window);
            }
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, latency ? 10 : 200, QS_ALLINPUT);
    }
    bool complete = shown && run.painted && !run.stopped && (!controlled || runner_finished) && target_still_valid(run);
    if (window) DestroyWindow(window);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    if (run.desktop_unavailable) return 3;
    return complete ? 0 : 1;
}

int wmain(int argc, wchar_t **argv)
{
    // The composite profile has a different parent device ID, with the same unit serial.
    const bool composite = argc >= 3 && wcscmp(argv[1], L"--composite") == 0;
    if (composite) { ++argv; --argc; }
    if (argc == 2 && wcscmp(argv[1], L"--pattern-version") == 0) {
        std::puts("KVM_USB_PATTERN 1");
        return 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--final-pattern-version") == 0) {
        std::puts("KVM_USB_FINAL_PATTERN 2");
        return 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--latency-pattern-version") == 0) {
        std::puts("KVM_USB_LATENCY_PATTERN 1");
        return 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--native-latency-version") == 0) {
        std::puts("KVM_USB_NATIVE_LATENCY 1");
        return 0;
    }
    bool inspect = argc == 3 && wcscmp(argv[2], L"--usb") == 0;
    bool pattern = argc == 3 && wcscmp(argv[2], L"--pattern") == 0;
    bool final_mode = argc == 3 && wcscmp(argv[2], L"--pattern-final") == 0;
    pattern_size size{};
    bool latency = (argc == 3 || argc == 4) && wcscmp(argv[2], L"--pattern-latency") == 0 &&
                   (argc == 3 || latency_size(argv[3], size));
    bool verify = argc == 4 && wcscmp(argv[2], L"--latency-target") == 0 && latency_size(argv[3], size);
    DWORD width = 0, height = 0;
    bool modes = argc == 3 && wcscmp(argv[2], L"--modes") == 0;
    bool test_mode = argc == 5 && wcscmp(argv[2], L"--test-mode") == 0 &&
                     dimension(argv[3], width) && dimension(argv[4], height);
    if ((composite && inspect) || (argc != 2 && !inspect && !pattern && !final_mode && !latency && !verify && !modes && !test_mode) || wcslen(argv[1]) != 12 ||
        wcsspn(argv[1], L"0123456789ABCDEFabcdef") != 12) {
        std::fputs("Usage: usb-monitor-windows [--composite] BOARD_USB_SERIAL [--usb (display-only) | --modes | --test-mode WIDTH HEIGHT | "
                   "--pattern | --pattern-final | --pattern-latency [640x480|1024x600] | --latency-target 640x480|1024x600]\n", stderr);
        return 2;
    }
    const std::wstring board_id = std::wstring(composite ? L"USB\\VID_303A&PID_2986\\" : L"USB\\VID_303A&PID_2987\\") + argv[1];
    if (modes || test_mode) return display_modes(board_id, width, height);
    if (verify) {
        pattern_target target;
        if (!use_physical_coordinates() || inspect_desktop(board_id, &target, false, &size) != 0) return 1;
        std::printf("board GDI source=%ls origin=(%ld,%ld)\n", target.device.c_str(), target.area.left, target.area.top);
        std::printf("LATENCY_TARGET %ldx%ld\n", size.width, size.height);
        return 0;
    }
    if (inspect) {
        std::wstring serial(argv[1]);
        for (auto &c : serial) if (c >= L'a' && c <= L'f') c -= L'a' - L'A';
        return inspect_usb(board_id, serial.c_str());
    }
    if (!pattern && !final_mode && !latency) return inspect_desktop(board_id);
    int result = show_pattern(board_id, final_mode, latency, size);
    if (result) std::fputs("Pattern stopped: target validation, painting, runner protocol or timeout.\n", stderr);
    return result;
}
