// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#define NOMINMAX
#include "share_window.hpp"
#include <cassert>
#include <cstdio>
#include <cwchar>

static bool stopped;
static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM key, LPARAM data) {
    if (ShareWindow::appearance_message(hwnd, msg, key, data)) return 0;
    if (msg == WM_COMMAND && LOWORD(key) == 1 && HIWORD(key) == BN_CLICKED) {
        stopped = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, key, data);
}

static RECT bounds(HWND parent, HWND child) {
    RECT rect{};
    assert(GetWindowRect(child, &rect));
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

static void check_controls(ShareWindow& ui, UINT dpi) {
    HWND button = GetDlgItem(ui.window, 1);
    RECT area{}, text = bounds(ui.window, ui.guidance), stop = bounds(ui.window, button);
    assert(GetClientRect(ui.window, &area));
    assert(text.left > 0 && text.right < area.right && text.top > 0 && text.bottom < stop.top);
    assert(stop.left == text.left && stop.right == text.right && stop.bottom < area.bottom);
    assert(stop.bottom - stop.top >= MulDiv(46, dpi, 96));
    assert(IsWindowEnabled(button) && IsWindowVisible(button));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
    assert(font && reinterpret_cast<HFONT>(SendMessageW(ui.guidance, WM_GETFONT, 0, 0)) == font);
    LOGFONTW actual{};
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    assert(SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi));
    assert(GetObjectW(font, sizeof(actual), &actual));
    assert(actual.lfHeight == metrics.lfMessageFont.lfHeight &&
           !wcscmp(actual.lfFaceName, metrics.lfMessageFont.lfFaceName));
    HDC dc = GetDC(ui.guidance);
    HGDIOBJ previous = SelectObject(dc, font);
    for (const auto message : {L"Choose a display in the Windows picker.",
                               L"Move the pointer onto DISPLAY99999\r\nand keep it there.",
                               L"Tap Allow display on KVMpad.\r\nKeep the pointer on this monitor.",
                               L"Capture frozen on KVMpad.\r\nKeep the pointer on this monitor.",
                               L"Sharing this display.\r\nMoving off it stops sharing."}) {
        RECT measured{0, 0, text.right - text.left, 0};
        assert(DrawTextW(dc, message, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX));
        assert(measured.right <= text.right - text.left && measured.bottom <= text.bottom - text.top);
    }
    SelectObject(dc, previous);
    ReleaseDC(ui.guidance, dc);
    wprintf(L"DPI %u: %ls %ld px, Stop %ld x %ld px, all five status messages fit\n", dpi,
            actual.lfFaceName, -actual.lfHeight, stop.right - stop.left, stop.bottom - stop.top);
}

int wmain() {
    assert(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    wchar_t name[64];
    swprintf_s(name, L"KVMpadShareWindowTest%lu", GetCurrentProcessId());
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    assert(desktop && SetThreadDesktop(desktop));
    {
        ShareWindow ui;
        assert(ui.create(GetModuleHandleW(nullptr), procedure));
        check_controls(ui, GetDpiForWindow(ui.window));
        RECT suggested{40, 40, 400, 240};
        for (UINT dpi : {96u, 144u, 192u, 96u}) {
            SendMessageW(ui.window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
            check_controls(ui, dpi);
        }
        SendMessageW(ui.window, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS, 0);
        check_controls(ui, GetDpiForWindow(ui.window));
        // Two synthetic work areas on the private desktop. No desktop capture is involved.
        for (RECT work : {RECT{640, 40, 1280, 480}, RECT{-640, -40, 0, 440}, RECT{80, 80, 420, 400}}) {
            assert(ui.place_on_display(work));
            RECT outer{}, clipped{}, again{}, stop = bounds(nullptr, GetDlgItem(ui.window, 1));
            assert(GetWindowRect(ui.window, &outer));
            assert(IntersectRect(&clipped, &outer, &work) && EqualRect(&clipped, &outer));
            assert(IntersectRect(&clipped, &stop, &work) && EqualRect(&clipped, &stop));
            check_controls(ui, GetDpiForWindow(ui.window));
            assert(ui.place_on_display(work) && GetWindowRect(ui.window, &again) && EqualRect(&outer, &again));
        }
        SendMessageW(GetDlgItem(ui.window, 1), BM_CLICK, 0, 0);
        assert(stopped);
    }
    assert(SetThreadDesktop(original) && CloseDesktop(desktop));
    puts("PASS: selected work area, reachable Stop, preserved placement, per-monitor relayout and Stop command 1.");
}
