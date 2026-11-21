// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#define NOMINMAX
#include <windows.h>
#include <wtsapi32.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shobjidl.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
#include "capture_output.hpp"
#include "crop_texture.hpp"
#include "frame_demand.hpp"
#include "share_window.hpp"

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
std::atomic<const char*> stop_reason{};
constexpr DWORD poll_ms = 10;
const char* stage = "arguments"; // Written only by the picker thread. No desktop content goes to stderr.

int failed(const char* where, HRESULT code = S_OK, int exit_code = 3) {
    fprintf(stderr, "KCE1 %s %08X\n", where, static_cast<unsigned>(code));
    fflush(stderr);
    return exit_code;
}

std::wstring required_display() {
    stage = "displayenv";
    wchar_t buffer[32]{};
    SetLastError(ERROR_SUCCESS);  // An empty value is different from an absent variable.
    DWORD length = GetEnvironmentVariableW(L"KVMPAD_HOST_DISPLAY", buffer, ARRAYSIZE(buffer));
    if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return {};
    if (!length || length >= ARRAYSIZE(buffer)) throw hresult_invalid_argument();
    std::wstring name(buffer, length);
    if (name.starts_with(L"\\\\.\\")) name.erase(0, 4);
    if (!name.starts_with(L"DISPLAY") || name.size() < 8 || name.size() > 12 ||
        name.find_first_not_of(L"0123456789", 7) != std::wstring::npos) throw hresult_invalid_argument();
    return L"\\\\.\\" + name;
}

void check_required_display(const std::wstring& required, const wchar_t* picked) {
    if (!required.empty() && required != picked) {
        stage = "wrongdisplay";
        throw hresult_access_denied();
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM key, LPARAM data) {
    if (ShareWindow::appearance_message(window, msg, key, data)) return 0;
    const char* reason{};
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(key) == 1)) reason = "closed";
    else if (msg == WM_DISPLAYCHANGE) reason = "topology";
    else if (msg == WM_ENDSESSION || (msg == WM_WTSSESSION_CHANGE &&
             (key == WTS_SESSION_LOCK || key == WTS_SESSION_LOGOFF ||
              key == WTS_CONSOLE_DISCONNECT || key == WTS_REMOTE_DISCONNECT))) reason = "desktop";
    else if (msg == WM_POWERBROADCAST && key == PBT_APMSUSPEND) reason = "suspended";
    if (reason) {
        stop_reason = reason;
        return 0;
    }
    return DefWindowProcW(window, msg, key, data);
}

bool interactive_desktop() {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    wchar_t name[64]{};
    DWORD used{};
    bool ok = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &used) &&
              _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return ok;
}

void pump(HANDLE parent) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (const char* reason = stop_reason.load()) {
        stage = reason;
        throw hresult_canceled();
    }
    if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) {
        stage = "parent";
        throw hresult_canceled();
    }
    if (!interactive_desktop()) {
        stage = "desktop";
        throw hresult_canceled();
    }
}

void write_bytes(const void* ptr, size_t bytes) {
    if (fwrite(ptr, 1, bytes, stdout) != bytes || fflush(stdout)) throw hresult_canceled();
}

char requested() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD count{};
    check_bool(PeekNamedPipe(input, nullptr, 0, nullptr, &count, nullptr));
    if (!count) return false;
    char command{};
    check_bool(ReadFile(input, &command, 1, &count, nullptr));
    if (count != 1 || (command != 'F' && command != 'P' && command != 'M')) throw hresult_canceled();
    return command;
}

struct Device {
    com_ptr<ID3D11Device> gpu;
    com_ptr<ID3D11DeviceContext> context;
    IDirect3DDevice runtime{nullptr};
    Device() {
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            gpu.put(), nullptr, context.put()));
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(gpu.as<IDXGIDevice>().get(), inspectable.put()));
        runtime = inspectable.as<IDirect3DDevice>();
    }
};

MonitorBounds monitor_bounds(const RECT& rect) {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

struct SelectedMonitor {
    HMONITOR handle;
    MonitorBounds bounds;
};

bool cursor_on_monitor(const SelectedMonitor& selected, POINT& cursor) {
    stage = "cursorpos";
    check_bool(GetPhysicalCursorPos(&cursor));
    stage = "geometry";
    HMONITOR under_cursor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL);
    MONITORINFO current{sizeof(current)};
    check_bool(GetMonitorInfoW(selected.handle, &current));
    switch (check_cursor_mapping(selected.bounds, monitor_bounds(current.rcMonitor), cursor.x, cursor.y,
                                 under_cursor != nullptr, under_cursor == selected.handle)) {
    case CursorMapping::inside: return true;
    case CursorMapping::outside: return false;
    case CursorMapping::topology: stage = "topology"; break;
    case CursorMapping::geometry: break;
    }
    throw hresult_canceled();
}

void require_cursor(const SelectedMonitor& selected, POINT& cursor) {
    if (!cursor_on_monitor(selected, cursor)) {
        stage = "cursor";
        throw hresult_canceled();
    }
}

// The staging texture contains only the selected crop. Desktop coordinates are physical pixels.
void emit(Device& device, Direct3D11CaptureFrame const& frame, const SelectedMonitor& selected,
          int width, int height, int crop_w, int crop_h, bool jpeg_output, bool metadata, HANDLE parent,
          HWND window, const std::wstring& indicator, bool& edge_shown) {
    POINT cursor{};
    require_cursor(selected, cursor);
    stage = "geometry";
    const auto& rect = selected.bounds;
    auto size = frame.ContentSize();
    Region region{};
    if (!cursor_region(rect.left,rect.top,rect.right,rect.bottom,cursor.x,cursor.y,
                       size.Width,size.Height,crop_w,crop_h,region)) throw hresult_canceled();
    if (region.clamped != edge_shown) {
        edge_shown = region.clamped;
        SetWindowTextW(window, (indicator + (edge_shown ? L" | Edge clamped" : L"")).c_str());
    }
    stage = "frame";
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
    auto pixels = crop_texture(device.gpu.get(), device.context.get(), texture.get(), size.Width, size.Height, region,
                               crop_w, crop_h, width, height);
    pump(parent);
    publish_frame(pixels, width, height, jpeg_output, [&] {
        pump(parent);
        POINT latest{};
        require_cursor(selected, latest);
        stage = "frame";
        if (metadata) {
            auto prefix=crop_prefix(rect.left,rect.top,cursor.x,cursor.y,region);
            write_bytes(&prefix,sizeof(prefix));
        }
    }, write_bytes);
}

int capture(int argc, wchar_t** argv) {
    if (argc != 5) return failed("arguments", E_INVALIDARG, 2);
    const auto required = required_display();
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        DWORD error = GetLastError();
        if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return failed("geometry", HRESULT_FROM_WIN32(error));
    }
    std::wstring codec(argv[1]);
    if (codec != L"jpeg" && codec != L"cinepak-intra" && codec != L"h264-intra") return failed("arguments", E_INVALIDARG, 2);
    int width = codec == L"cinepak-intra" ? 960 : 512;
    int height = width == 512 ? 512 : 540;
    int factor = _wtoi(argv[2]); // 1 = zoom, 2 = native, 4 = overview.
    if ((width != 512 && width != 960) || (factor != 1 && factor != 2 && factor != 4))
        return failed("arguments", E_INVALIDARG, 2);
    if (std::wstring(argv[3]) != L"--parent") return failed("arguments", E_INVALIDARG, 2);
    handle parent{OpenProcess(SYNCHRONIZE, FALSE, wcstoul(argv[4], nullptr, 10))};
    if (!parent) return failed("parent", HRESULT_FROM_WIN32(GetLastError()), 2);
    stage = "window";
    init_apartment(apartment_type::single_threaded);
    if (!GraphicsCaptureSession::IsSupported()) return failed("start", E_NOTIMPL);
    _setmode(_fileno(stdout), _O_BINARY);
    ShareWindow indicator_window;
    check_bool(indicator_window.create(GetModuleHandleW(nullptr), window_proc));
    HWND window = indicator_window.window, guidance = indicator_window.guidance;
    check_bool(WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION));
    stage = "select";
    GraphicsCapturePicker picker;
    check_hresult(picker.as<IInitializeWithWindow>()->Initialize(window));
    auto selection = picker.PickSingleItemAsync();
    while (selection.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
        pump(parent.get()); Sleep(poll_ms);
    }
    pump(parent.get());
    auto item = selection.GetResults();
    if (!item) return failed("cancel");
    // This interface identifies the *picked* display, including duplicate models and rotated displays.
    // A picked window or OS without monitor interop has no authorized coordinate mapping.
    HMONITOR handle{};
    stage = "monitor";
    auto monitor_item = item.try_as<IMonitorGraphicsCaptureItemInterop>();
    HRESULT result = monitor_item ? monitor_item->GetMonitor(&handle) : E_NOINTERFACE;
    if (FAILED(result) || !handle) {
        HWND picked_window{};
        auto window_item = item.try_as<IWindowGraphicsCaptureItemInterop>();
        if (window_item && SUCCEEDED(window_item->GetWindow(&picked_window)) && picked_window)
            return failed("pickedwindow");
        return failed("monitor", FAILED(result) ? result : E_UNEXPECTED);
    }
    stage = "geometry";
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    check_bool(GetMonitorInfoW(handle, &info));
    check_required_display(required, info.szDevice);
    const SelectedMonitor selected{handle, monitor_bounds(info.rcMonitor)};
    const auto selected_size = item.Size();
    if (!selected.bounds.matches_size(selected_size.Width, selected_size.Height)) throw hresult_canceled();
    stage = "window";
    check_bool(indicator_window.place_on_display(info.rcWork));
    stage = "name";
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info.szDevice, -1,
                                    nullptr, 0, nullptr, nullptr);
    if (length < 2 || length > 96) return failed("name");
    char name[96]{};
    check_bool(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info.szDevice, -1,
                                  name, length, nullptr, nullptr));
    item.Closed([](auto&&, auto&&) { stop_reason = "itemclosed"; });
    std::wstring display_label = info.szDevice;
    if (display_label.starts_with(L"\\\\.\\")) display_label.erase(0, 4);
    const std::wstring indicator = std::wstring(L"KVMpad HOST | ") + display_label;
    bool edge_shown = false;
    SetWindowTextW(window, indicator.c_str());
    stage = "gpu";
    Device device;
    POINT cursor{};
    const auto entry_deadline = GetTickCount64() + 20000;
    if (!wait_for_cursor_entry([&] { return cursor_on_monitor(selected, cursor); }, [&] {
            stage = "window";
            const auto prompt = std::wstring(L"Move the pointer onto ") + display_label + L"\r\nand keep it there.";
            check_bool(SetWindowTextW(guidance, prompt.c_str()));
            wchar_t progress[2]{};
            if (GetEnvironmentVariableW(L"KVMPAD_PICKER_PROGRESS", progress, 2) == 1 && progress[0] == L'1') {
                stage = "closed";
                wchar_t label[2]{};
                if (GetEnvironmentVariableW(L"KVMPAD_PICKER_LABEL", label, 2) == 1 && label[0] == L'1') {
                    const std::string waiting = std::string("KCP2 CURSOR ") + name + "\n";
                    write_bytes(waiting.data(), waiting.size());
                } else {
                    constexpr char waiting[] = "KCP1 CURSOR\n";
                    write_bytes(waiting, sizeof(waiting) - 1);
                }
            }
        }, [&] {
            pump(parent.get());
            Sleep(poll_ms);
            return GetTickCount64() < entry_deadline;
        })) return failed("cursorwait", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    pump(parent.get());
    require_cursor(selected, cursor);
    stage = "window";
    check_bool(SetWindowTextW(guidance, L"Tap Allow display on KVMpad.\r\nKeep the pointer on this monitor."));
    stage = "closed";
    write_bytes(name, length - 1);
    write_bytes("\n", 1);
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    auto close_session = [&] {
        if (session) { session.Close(); session = nullptr; }
        if (pool) { pool.Close(); pool = nullptr; }
    };
    auto start_session = [&] {
        pump(parent.get());
        require_cursor(selected, cursor);
        stage = "pool";
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(device.runtime,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        stage = "start";
        session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(true);
        session.StartCapture();
    };
    FrameDemand demand;
    for (;;) {
        stage = "closed";
        pump(parent.get());
        // Covers panel approval, slow frame demand and Freeze. Never switch resources on leave.
        require_cursor(selected, cursor);
        stage = "closed";
        char cmd = requested();
        if (cmd == 'P') {
            close_session();
            demand = {};
            stage = "window";
            check_bool(SetWindowTextW(guidance, L"Capture frozen on KVMpad.\r\nKeep the pointer on this monitor."));
        } else if (cmd == 'F' || cmd == 'M') {
            demand.begin(cmd, GetTickCount64(), static_cast<bool>(session));
            if (!session) {
                start_session();
                stage = "window";
                check_bool(SetWindowTextW(guidance, L"Sharing this display.\r\nMoving off it stops sharing."));
            }
        }
        stage = "frame";
        if (pool) {
            demand.poll(GetTickCount64(), [&] { return pool.TryGetNextFrame(); }, [&] {
                close_session();
                start_session();
            }, [&](const Direct3D11CaptureFrame& frame, bool metadata) {
                emit(device, frame, selected, width, height, width * factor / 2, height * factor / 2,
                     codec == L"jpeg", metadata, parent.get(), window, indicator, edge_shown);
            });
        }
        Sleep(poll_ms);
    }
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && (std::wstring(argv[1]) == L"--encode-cinepak" ||
                      std::wstring(argv[1]) == L"--encode-jpeg")) {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        const bool is_jpeg = std::wstring(argv[1]) == L"--encode-jpeg";
        init_apartment(apartment_type::multi_threaded);
        std::vector<uint32_t> pixels(is_jpeg ? 512 * 512 : cinepak::width * cinepak::height);
        while (fread(pixels.data(), sizeof(uint32_t), pixels.size(), stdin) == pixels.size()) {
            auto packet = is_jpeg ? jpeg::encode(pixels.data(), 512, 512) : cinepak::encode(pixels.data());
            if (is_jpeg) {
                uint32_t count = static_cast<uint32_t>(packet.size());
                if (fwrite(&count, sizeof(count), 1, stdout) != 1) return 2;
            }
            if (fwrite(packet.data(), 1, packet.size(), stdout) != packet.size() || fflush(stdout)) return 2;
        }
        return feof(stdin) ? 0 : 2;
    }
    try { return capture(argc, argv); }
    catch (const TextureExtentMismatch& error) { return failed("geometry", error.code()); }
    catch (const hresult_error& error) { return failed(stage, error.code()); }
    catch (...) { return failed(stage, E_FAIL); }
}
