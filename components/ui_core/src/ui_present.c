// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Status wording (01 §6.1 / §6.2 "View" columns).
#include "ui_present.h"

#include "ui_contracts.h"

const char *ui_present_host(int32_t v)
{
    switch (v) {
    case KVM_HOST_USB_UNINITIALIZED: return "Initializing";
    case KVM_HOST_USB_DETACHED: return "Disconnected";
    case KVM_HOST_USB_ENUMERATING: return "Connecting";
    case KVM_HOST_USB_MOUNTED: return "Active";
    case KVM_HOST_USB_SUSPENDED: return "Suspended";
    case KVM_HOST_USB_ERROR: return "Error";
    default: return "Unknown";
    }
}

const char *ui_present_client(int32_t v)
{
    switch (v) {
    case UI_CLIENT_DISABLED: return "Off";
    case UI_CLIENT_NETWORK_DOWN: return "Offline";
    case UI_CLIENT_LISTENING: return "Available";
    case UI_CLIENT_AUTHENTICATING: return "Connecting";
    case UI_CLIENT_CONNECTED: return "Connected";
    case UI_CLIENT_CONTROL_GRANTED: return "Controlling";
    case UI_CLIENT_STALE: return "Reconnecting";
    default: return "Unknown";
    }
}

const char *ui_present_owner(int32_t v)
{
    switch (v) {
    case OWNER_NONE: return "None";
    case OWNER_LOCAL: return "Local";
    case OWNER_REMOTE: return "Remote";
    default: return "Unknown";
    }
}
