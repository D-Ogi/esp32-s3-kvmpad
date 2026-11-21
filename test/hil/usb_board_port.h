// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <vector>

// Select the connection by its present device instance, even without a function driver.
static inline HANDLE open_board_hub(wchar_t *name, ULONG &port)
{
    DEVINST board, hub;
    ULONG reg = 0, size = sizeof(port);
    if (CM_Locate_DevNodeW(&board, name, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
        CM_Get_Parent(&hub, board, 0) != CR_SUCCESS ||
        CM_Get_DevNode_Registry_PropertyW(board, CM_DRP_ADDRESS, &reg, &port, &size, 0) != CR_SUCCESS ||
        reg != REG_DWORD || !port)
        return INVALID_HANDLE_VALUE;
    const GUID usb_hub = {0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};
    HDEVINFO set = SetupDiGetClassDevsW(&usb_hub, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA iface = {};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &usb_hub, i, &iface)) break;
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &bytes, nullptr);
        if (!bytes || bytes > 8192) continue;
        std::vector<unsigned char> data(bytes);
        auto path = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(data.data());
        path->cbSize = sizeof(*path);
        SP_DEVINFO_DATA info = {};
        info.cbSize = sizeof(info);
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, path, bytes, nullptr, &info) && info.DevInst == hub) {
            h = CreateFileW(path->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                            nullptr);
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}
