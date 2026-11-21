// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <windows.h>
#include <algorithm>

// The foreground indicator owns only its HWNDs and the Windows message font.
class ShareWindow final {
    HWND stop_button{};
    HFONT font{};
    static constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    static constexpr DWORD extended = WS_EX_TOPMOST;
public:
    HWND window{}, guidance{};
    ~ShareWindow() {
        if (window) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            DestroyWindow(window);
        }
        if (font) DeleteObject(font);
    }
    ShareWindow() = default;
    ShareWindow(const ShareWindow&) = delete;
    ShareWindow& operator=(const ShareWindow&) = delete;

    bool place_on_display(const RECT& work) {
        RECT current{}, visible{};
        if (!GetWindowRect(window, &current) || IsRectEmpty(&work)) return false;
        if (IntersectRect(&visible, &current, &work) && EqualRect(&visible, &current)) return true;
        // Move first so GetDpiForWindow reports the selected display's scale.
        if (!SetWindowPos(window, nullptr, work.left, work.top,
                          std::min(current.right - current.left, work.right - work.left),
                          std::min(current.bottom - current.top, work.bottom - work.top),
                          SWP_NOZORDER | SWP_NOACTIVATE)) return false;
        return layout(GetDpiForWindow(window), &work, &work);
    }

    bool create(HINSTANCE instance, WNDPROC procedure) {
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = procedure;
        wc.lpszClassName = L"KvmpadHostShare";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        if (!RegisterClassW(&wc)) return false;
        window = CreateWindowExW(extended, wc.lpszClassName, L"KVMpad HOST | Select a display", style,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 nullptr, nullptr, instance, nullptr);
        if (!window) return false;
        guidance = CreateWindowW(L"STATIC", L"Choose a display in the Windows picker.", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                 0, 0, 0, 0, window, nullptr, instance, nullptr);
        stop_button = CreateWindowW(L"BUTTON", L"Stop sharing", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(1), instance, nullptr);
        if (!guidance || !stop_button || !layout(GetDpiForWindow(window), nullptr)) return false;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ShowWindow(window, SW_SHOW);
        return true;
    }

private:
    bool layout(UINT dpi, const RECT* suggested, const RECT* work = nullptr) {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) return false;
        HFONT next = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!next) return false;
        SendMessageW(guidance, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        SendMessageW(stop_button, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font) DeleteObject(font);
        font = next;

        HDC dc = GetDC(guidance);
        if (!dc) return false;
        HGDIOBJ previous = SelectObject(dc, font);
        TEXTMETRICW text{};
        bool measured = GetTextMetricsW(dc, &text) != FALSE;
        SelectObject(dc, previous);
        ReleaseDC(guidance, dc);
        if (!measured) return false;
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        const int margin = scale(12), gap = scale(8);
        int width = std::max(scale(330), 44 * static_cast<int>(text.tmAveCharWidth));
        RECT border{};
        if (!AdjustWindowRectExForDpi(&border, style, FALSE, extended, dpi)) return false;
        if (work) {
            int available = work->right - work->left - (border.right - border.left) - 2 * margin;
            if (available <= 0) return false;
            width = std::min(width, available);
        }
        const int guidance_height = std::max(scale(42), 3 * static_cast<int>(text.tmHeight));
        const int button_height = std::max(scale(46), static_cast<int>(text.tmHeight) + 2 * gap);
        RECT frame{0, 0, width + 2 * margin, 2 * margin + guidance_height + gap + button_height};
        if (!AdjustWindowRectExForDpi(&frame, style, FALSE, extended, dpi)) return false;
        LONG x = suggested ? suggested->left : 0, y = suggested ? suggested->top : 0;
        if (work && suggested) {
            if (frame.bottom - frame.top > work->bottom - work->top) return false;
            x = std::clamp(x, work->left, work->right - (frame.right - frame.left));
            y = std::clamp(y, work->top, work->bottom - (frame.bottom - frame.top));
        }
        if (!SetWindowPos(window, nullptr, x, y,
                          frame.right - frame.left, frame.bottom - frame.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE))) return false;
        return MoveWindow(guidance, margin, margin, width, guidance_height, TRUE) &&
               MoveWindow(stop_button, margin, margin + guidance_height + gap, width, button_height, TRUE);
    }

public:
    static bool appearance_message(HWND hwnd, UINT msg, WPARAM key, LPARAM data) {
        if (msg != WM_DPICHANGED && msg != WM_SETTINGCHANGE) return false;
        auto self = reinterpret_cast<ShareWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return false;
        RECT area{};
        if (msg == WM_DPICHANGED) area = *reinterpret_cast<const RECT*>(data);
        else if (!GetWindowRect(hwnd, &area)) return false;
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST), &info)) return false;
        return self->layout(msg == WM_DPICHANGED ? HIWORD(key) : GetDpiForWindow(hwnd), &area, &info.rcWork);
    }
};
