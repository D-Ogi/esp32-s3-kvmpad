// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Exercise the production restriction with synthetic GDI identities and no picker or desktop.
#define wmain capture_helper_main
#include "capture.cpp"
#undef wmain
#include <cassert>

int wmain() {
    const wchar_t* key = L"KVMPAD_HOST_DISPLAY";
    const wchar_t* monitor = L"\\\\.\\DISPLAY2";
    SetEnvironmentVariableW(key, nullptr);
    assert(required_display().empty());
    check_required_display(required_display(), L"\\\\.\\DISPLAY1");
    for (auto value : {L"DISPLAY2", monitor, L"DISPLAY99999"}) {
        assert(SetEnvironmentVariableW(key, value));
        auto required = required_display();
        assert(required == (std::wstring(value) == L"DISPLAY99999" ? L"\\\\.\\DISPLAY99999" : monitor));
        check_required_display(required, required.c_str());
        try {
            check_required_display(required, L"\\\\.\\DISPLAY1");
            assert(false);
        } catch (hresult_access_denied const&) {
            assert(std::string(stage) == "wrongdisplay");
        }
    }
    for (auto value : {L"", L"display2", L"DISPLAY", L"DISPLAY123456", L"DISPLAY\xff12", L" DISPLAY2",
                       L"DISPLAY02\n", L"\\\\.\\DISPLAY2\\other", L"DISPLAY000000000000000000000000000000"}) {
        assert(SetEnvironmentVariableW(key, value));
        try {
            required_display();
            assert(false);
        } catch (hresult_invalid_argument const&) {
            assert(std::string(stage) == "displayenv");
        }
    }
    puts("PASS native monitor restriction: unset, short/full identity, mismatched monitor, empty/malformed setting");
}
