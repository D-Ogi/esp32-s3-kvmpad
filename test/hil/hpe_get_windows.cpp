// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include "usb_board_port.h"
#include <usbiodef.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <string>
#include <algorithm>
#include <array>
#include <vector>
#include "../../components/usb_hid_device/include/hpe_reports.h"
#include "../../components/usb_hid_device/src/hpe_cert.h"

static bool board_ancestor(DEVINST node, const std::wstring &board_id)
{
    for (unsigned depth = 0; depth < 12; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
        if (_wcsicmp(id, board_id.c_str()) == 0) return true;
        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return false;
}

// HidP consumes the Windows preparsed descriptor; every packet below is process-local.
// The zero-access device handle is never used to read or submit an input/output report.
struct Layout {
    PHIDP_PREPARSED_DATA pp;
    const HIDP_CAPS &caps;
    std::vector<HIDP_BUTTON_CAPS> button[3];
    std::vector<HIDP_VALUE_CAPS> value[3];
    std::vector<HIDP_LINK_COLLECTION_NODE> nodes;

    bool load()
    {
        ULONG count = caps.NumberLinkCollectionNodes;
        nodes.resize(count);
        if (!count || HidP_GetLinkCollectionNodes(nodes.data(), &count, pp) != HIDP_STATUS_SUCCESS ||
            count != nodes.size())
            return false;
        const USHORT bc[] = {caps.NumberInputButtonCaps, caps.NumberOutputButtonCaps, caps.NumberFeatureButtonCaps};
        const USHORT vc[] = {caps.NumberInputValueCaps, caps.NumberOutputValueCaps, caps.NumberFeatureValueCaps};
        for (int t = 0; t < 3; ++t) {
            auto type = static_cast<HIDP_REPORT_TYPE>(t);
            USHORT nb = bc[t], nv = vc[t];
            button[t].resize(nb);
            value[t].resize(nv);
            if ((nb && HidP_GetButtonCaps(type, button[t].data(), &nb, pp) != HIDP_STATUS_SUCCESS) ||
                (nv && HidP_GetValueCaps(type, value[t].data(), &nv, pp) != HIDP_STATUS_SUCCESS) || nb != bc[t] ||
                nv != vc[t])
                return false;
            for (const auto &b : button[t])
                printf("  button type=%d id=%02x page=%04x usages=%04x..%04x link=%u flags=%x\n", t, b.ReportID,
                       b.UsagePage, b.IsRange ? b.Range.UsageMin : b.NotRange.Usage,
                       b.IsRange ? b.Range.UsageMax : b.NotRange.Usage, b.LinkCollection, b.BitField);
            for (const auto &v : value[t])
                printf("  value type=%d id=%02x page=%04x usages=%04x..%04x link=%u bits=%u count=%u "
                       "logical=%ld..%ld physical=%ld..%ld unit=%lx exp=%lx flags=%x null=%u\n",
                       t, v.ReportID, v.UsagePage, v.IsRange ? v.Range.UsageMin : v.NotRange.Usage,
                       v.IsRange ? v.Range.UsageMax : v.NotRange.Usage, v.LinkCollection, v.BitSize, v.ReportCount,
                       v.LogicalMin, v.LogicalMax, v.PhysicalMin, v.PhysicalMax, v.Units, v.UnitsExp, v.BitField,
                       v.HasNull);
        }
        return true;
    }
    template <class T> static bool has_usage(const T &cap, unsigned usage)
    {
        return cap.IsRange ? cap.Range.UsageMin <= usage && usage <= cap.Range.UsageMax : cap.NotRange.Usage == usage;
    }
    const HIDP_VALUE_CAPS *field(int type, int id, unsigned page, unsigned usage, unsigned bits, long lo, long hi,
                                 int link = -1) const
    {
        for (const auto &v : value[type])
            if (v.ReportID == id && v.UsagePage == page && has_usage(v, usage) && v.BitSize == bits &&
                v.LogicalMin == lo && v.LogicalMax == hi && (link < 0 || v.LinkCollection == link))
                return &v;
        return nullptr;
    }
    bool key(int type, int id, unsigned page, unsigned usage, int link = -1) const
    {
        for (const auto &b : button[type])
            if (b.ReportID == id && b.UsagePage == page && has_usage(b, usage) &&
                (link < 0 || b.LinkCollection == link))
                return true;
        return false;
    }
    bool only_id(int id) const
    {
        for (int t = 0; t < 3; ++t) {
            for (const auto &b : button[t])
                if (b.ReportID != id) return false;
            for (const auto &v : value[t])
                if (v.ReportID != id) return false;
        }
        return true;
    }
    bool bytes(int input, int output, int feature) const
    {
        return caps.InputReportByteLength == input && caps.OutputReportByteLength == output &&
               caps.FeatureReportByteLength == feature;
    }
    bool byte_field(int type, int id, unsigned page, unsigned usage, unsigned count, int link = -1) const
    {
        for (const auto &v : value[type])
            if (v.ReportID == id && v.UsagePage == page && has_usage(v, usage) && v.BitSize == 8 &&
                v.ReportCount == count && v.IsAbsolute && (link < 0 || v.LinkCollection == link)) return true;
        return false;
    }
    bool byte_extents() const
    {
        bool ok = true;
        for (const auto &values : value) {
            for (const auto &v : values) {
                bool contact = v.ReportID == HPE_PTP && v.UsagePage == 13 && has_usage(v, 0x51);
                bool cert = v.ReportID == HPE_CERT && v.UsagePage == 0xff00 && has_usage(v, 0xc5);
                bool studio = v.UsagePage == 0xff61 && v.ReportID >= HPE_STUDIO_IN && v.ReportID <= HPE_STUDIO_FEATURE;
                if (!(contact || cert || studio) || (v.LogicalMin == 0 && v.LogicalMax == 255)) continue;
                printf("BYTE_EXTENT id=%02x link=%u logical=%ld..%ld expected=0..255 FAIL\n",
                       v.ReportID, v.LinkCollection, v.LogicalMin, v.LogicalMax);
                ok = false;
            }
        }
        return ok;
    }
    using Packet = std::vector<char>;
    bool read_value(const Packet &p, int type, unsigned page, unsigned usage, unsigned link, ULONG &raw) const
    {
        return HidP_GetUsageValue(static_cast<HIDP_REPORT_TYPE>(type), page, link, usage, &raw, pp,
                                  const_cast<char *>(p.data()), p.size()) == HIDP_STATUS_SUCCESS;
    }
    bool equals(const Packet &p, int type, unsigned page, unsigned usage, ULONG expected, unsigned link = 0) const
    {
        ULONG raw = 0;
        return read_value(p, type, page, usage, link, raw) && raw == expected;
    }
    bool buttons(Packet &p, int type, unsigned page, unsigned link, std::vector<USAGE> expected) const
    {
        std::array<USAGE, 32> usages = {};
        ULONG count = usages.size();
        if (HidP_GetUsages(static_cast<HIDP_REPORT_TYPE>(type), page, link, usages.data(), &count, pp, p.data(),
                           p.size()) != HIDP_STATUS_SUCCESS ||
            count != expected.size())
            return false;
        std::sort(usages.begin(), usages.begin() + count);
        std::sort(expected.begin(), expected.end());
        return std::equal(expected.begin(), expected.end(), usages.begin());
    }
    bool array_bytes(int type, int id, unsigned page, unsigned usage, unsigned count) const
    {
        if (!byte_field(type, id, page, usage, count)) return false;
        const unsigned length[] = {caps.InputReportByteLength, caps.OutputReportByteLength,
                                   caps.FeatureReportByteLength};
        Packet packet(length[type], 0), read(count);
        if (packet.size() < count + 1) return false;
        packet[0] = static_cast<char>(id);
        for (unsigned i = 0; i < count; ++i)
            packet[1 + i] = static_cast<char>(37 * i + 19);
        return HidP_GetUsageValueArray(static_cast<HIDP_REPORT_TYPE>(type), page, 0, usage, read.data(), count, pp,
                                       packet.data(), packet.size()) == HIDP_STATUS_SUCCESS &&
               std::equal(read.begin(), read.end(), packet.begin() + 1);
    }
    bool consumer(int id) const
    {
        if (!bytes(3, 0, 0) || !only_id(id)) return false;
        char local[3] = {static_cast<char>(id), (char)0xe9, 0}; // volume increment in host memory
        USAGE u[2] = {};
        ULONG n = 2;
        return HidP_GetUsages(HidP_Input, 0x0c, 0, u, &n, pp, local, sizeof(local)) == HIDP_STATUS_SUCCESS && n == 1 &&
               u[0] == 0xe9 && key(0, id, 12, 0x3ff);
    }
    bool ptp() const
    {
        if (!bytes(34, 0, 257)) return false;
        Packet report(34, 0);
        report[0] = HPE_PTP;
        for (unsigned i = 0; i < 5; ++i) {
            unsigned at = 1 + i * 6;
            report[at] = static_cast<char>(1 + (i % 3));
            report[at + 1] = static_cast<char>(251 + i);
            report[at + 2] = static_cast<char>(i + 7);
            report[at + 3] = 0x21;
            report[at + 4] = static_cast<char>(i + 19);
            report[at + 5] = 0x43;
        }
        report[31] = 0x23;
        report[32] = 0x45;
        report[33] = 5;
        unsigned fingers = 0;
        for (unsigned i = 1; i < nodes.size(); ++i) {
            const auto &n = nodes[i];
            if (n.LinkUsagePage != 13 || n.LinkUsage != 0x22) continue;
            const auto *x = field(0, HPE_PTP, 1, 0x30, 16, 0, 32767, i),
                       *y = field(0, HPE_PTP, 1, 0x31, 16, 0, 32767, i);
            if (n.Parent != 0 || n.CollectionType != 2 || !x || !y || !x->IsAbsolute || !y->IsAbsolute ||
                x->Units != 0x11 || y->Units != 0x11 || (x->UnitsExp & 15) != 14 || (y->UnitsExp & 15) != 14 ||
                !key(0, HPE_PTP, 13, 0x42, i) || !key(0, HPE_PTP, 13, 0x47, i) ||
                !byte_field(0, HPE_PTP, 13, 0x51, 1, i))
                return false;
            ULONG id = 0;
            if (!read_value(report, 0, 13, 0x51, i, id) || id < 251 || id > 255) return false;
            unsigned slot = id - 251;
            if (fingers & (1u << slot)) return false;
            fingers |= 1u << slot;
            unsigned flags = 1 + slot % 3;
            std::vector<USAGE> pressed;
            if (flags & 1) pressed.push_back(0x42);
            if (flags & 2) pressed.push_back(0x47);
            if (!buttons(report, 0, 13, i, pressed) || !equals(report, 0, 1, 0x30, 0x2107 + slot, i) ||
                !equals(report, 0, 1, 0x31, 0x4313 + slot, i))
                return false;
        }
        const auto *scan = field(0, HPE_PTP, 13, 0x56, 16, 0, 65535);
        Packet feature(caps.FeatureReportByteLength, 0);
        feature[0] = HPE_CAPS;
        feature[1] = 0x25;
        return fingers == 31 && scan && scan->Units == 0x1001 && (scan->UnitsExp & 15) == 12 &&
               equals(report, 0, 13, 0x56, 0x4523) && equals(report, 0, 13, 0x54, 5) &&
               equals(feature, 2, 13, 0x55, 5) && equals(feature, 2, 13, 0x59, 2) &&
               field(0, HPE_PTP, 13, 0x54, 8, 0, 5) && field(2, HPE_CAPS, 13, 0x55, 4, 0, 5) &&
               field(2, HPE_CAPS, 13, 0x59, 4, 0, 2) && field(2, HPE_LATENCY, 13, 0x60, 8, 0, 1) &&
               array_bytes(2, HPE_CERT, 0xff00, 0xc5, 256);
    }
    bool joystick() const
    {
        if (!bytes(12, 0, 0) || !only_id(HPE_JOYSTICK)) return false;
        for (unsigned axis : {0x30u, 0x31u, 0x35u}) {
            const auto *v = field(0, HPE_JOYSTICK, 1, axis, 16, -32767, 32767);
            if (!v || !v->IsAbsolute) return false;
        }
        for (unsigned b = 1; b <= 16; ++b)
            if (!key(0, HPE_JOYSTICK, 9, b)) return false;
        const auto *hat = field(0, HPE_JOYSTICK, 1, 0x39, 4, 0, 7);
        Packet report = {HPE_JOYSTICK,
                         0x11,
                         0x22,
                         0x33,
                         0x44,
                         0x55,
                         0x66,
                         0x78,
                         static_cast<char>(0x87),
                         0x01,
                         static_cast<char>(0x80),
                         0x03};
        return equals(report, 0, 1, 0x30, 0x2211) && equals(report, 0, 1, 0x31, 0x4433) &&
               equals(report, 0, 1, 0x35, 0x6655) && equals(report, 0, 1, 0x32, 0x8778) &&
               equals(report, 0, 1, 0x39, 3) && buttons(report, 0, 9, 0, {1, 16}) &&
               field(0, HPE_JOYSTICK, 1, 0x32, 16, 0, 65535) && hat && hat->HasNull && hat->PhysicalMax == 315 &&
               hat->Units == 0x14;
    }
    bool check(int index, bool hpe) const
    {
        if (index == 0) {
            Packet input = {0, 1, 0, 4, 5, 6, 7, 8, 9}, led = {0, 0x15};
            return bytes(9, 2, 0) && only_id(0) && key(0, 0, 7, 0xe7) &&
                   buttons(input, 0, 7, 0, {0xe0, 4, 5, 6, 7, 8, 9}) && buttons(led, 1, 8, 0, {1, 3, 5});
        }
        if (index == 1) {
            Packet report = {0, 0x11, 21, 42, 53, 64};
            return bytes(6, 0, 0) && only_id(0) && buttons(report, 0, 9, 0, {1, 5}) &&
                   field(0, 0, 1, 0x30, 8, -127, 127) && field(0, 0, 1, 0x31, 8, -127, 127) &&
                   equals(report, 0, 1, 0x30, 21) && equals(report, 0, 1, 0x31, 42) && equals(report, 0, 1, 0x38, 53) &&
                   equals(report, 0, 12, 0x238, 64);
        }
        if (!hpe) return index == 2 && consumer(0);
        if (index == 2) return ptp();
        if (index == 3) {
            Packet mode = {HPE_INPUT_MODE, 3}, selective = {HPE_SELECT, 1};
            return bytes(0, 0, 2) && field(2, HPE_INPUT_MODE, 13, 0x52, 8, 0, 3) && equals(mode, 2, 13, 0x52, 3) &&
                   buttons(selective, 2, 13, 0, {0x57}) && key(2, HPE_SELECT, 13, 0x58);
        }
        if (index == 4) {
            const auto *dial = field(0, HPE_RADIAL, 1, 0x37, 15, -3600, 3600);
            Packet report = {HPE_RADIAL, 0x47, 0x02};
            return bytes(3, 0, 0) && only_id(HPE_RADIAL) && buttons(report, 0, 9, 0, {1}) &&
                   equals(report, 0, 1, 0x37, 0x123) && dial && !dial->IsAbsolute && dial->PhysicalMin == -3600 &&
                   dial->PhysicalMax == 3600 && dial->Units == 0x14 && (dial->UnitsExp & 15) == 15;
        }
        if (index == 5) return consumer(HPE_CONSUMER);
        if (index == 6)
            return bytes(64, 64, 16) && array_bytes(0, HPE_STUDIO_IN, 0xff61, 2, 63) &&
                   array_bytes(1, HPE_STUDIO_OUT, 0xff61, 3, 63) && array_bytes(2, HPE_STUDIO_FEATURE, 0xff61, 4, 15);
        return index == 7 && joystick();
    }
};

static bool get(HANDLE dev, unsigned id, unsigned max_size)
{
    if (max_size < 2 || max_size > 257) return false;
    std::vector<unsigned char> b(max_size, 0xaa);
    b[0] = (unsigned char)id;
    if (!HidD_GetFeature(dev, b.data(), max_size)) {
        printf("GET_FEATURE id=%02x winerror=%lu\n", id, GetLastError());
        return false;
    }
    bool ok = b[0] == id;
    switch (id) {
    case HPE_CERT:
        ok = ok && b.size() == sizeof(s_ptp_cert_report) && !memcmp(b.data(), s_ptp_cert_report, b.size());
        break;
    case HPE_CAPS:
        ok = ok && b[1] == 0x25;
        break;
    case HPE_LATENCY:
        ok = ok && b[1] <= 1;
        break;
    case HPE_INPUT_MODE:
        ok = ok && (b[1] == 0 || b[1] == 3);
        break;
    case HPE_SELECT:
        ok = ok && b[1] <= 3;
        break;
    case HPE_STUDIO_FEATURE:
        ok = ok && b.size() == 16 && b[1] == 1 && b[2] == 0 && b[5] == 8 && b[6] == 51 && b[7] <= 5 && !b[12] &&
             !b[13] && !b[14] && !b[15];
        break;
    default:
        return false;
    }
    unsigned useful = id == HPE_CERT || id == HPE_STUDIO_FEATURE ? max_size : 2;
    printf("GET_FEATURE id=%02x checked_bytes=%u data=", id, useful);
    for (unsigned i = 0; i < useful; i++)
        printf("%02x", b[i]);
    printf(" %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// Resolve the board's current physical parent and port before opening the hub.
// GET_DESCRIPTOR reads configuration zero; the class filter does not enumerate other ports.
static bool configuration(unsigned revision, std::wstring &board_id)
{
    ULONG port = 0;
    HANDLE h = open_board_hub(board_id.data(), port);
    if (h == INVALID_HANDLE_VALUE) return false;
    USB_NODE_CONNECTION_INFORMATION_EX connection = {};
    connection.ConnectionIndex = port;
    DWORD got = 0;
    USB_DEVICE_DESCRIPTOR &d = connection.DeviceDescriptor;
    bool ok = DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, &connection, sizeof(connection),
                              &connection, sizeof(connection), &got, nullptr) &&
              got >= sizeof(connection) && connection.ConnectionStatus == DeviceConnected && d.idVendor == 0x303a &&
              d.idProduct == 0x400c && d.bcdDevice == revision && d.bcdUSB == 0x0200 && d.bDeviceClass == 0 &&
              d.bMaxPacketSize0 == 64 && d.bNumConfigurations == 1 && connection.CurrentConfigurationValue == 1 &&
              connection.Speed == 1;
    printf("DEVICE port=%lu VID=%04x PID=%04x revision=%04x bcdUSB=%04x class=%02x ep0=%u active_config=%u speed=%u\n",
           port, d.idVendor, d.idProduct, d.bcdDevice, d.bcdUSB, d.bDeviceClass, d.bMaxPacketSize0,
           connection.CurrentConfigurationValue, connection.Speed);
    std::array<unsigned char, sizeof(USB_DESCRIPTOR_REQUEST) + 512> storage = {};
    auto req = reinterpret_cast<USB_DESCRIPTOR_REQUEST *>(storage.data());
    req->ConnectionIndex = port;
    req->SetupPacket.bmRequest = 0x80;
    req->SetupPacket.bRequest = 6;
    req->SetupPacket.wValue = 0x0200;
    req->SetupPacket.wLength = 512;
    if (!ok || !DeviceIoControl(h, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, req, sizeof(storage), req,
                                sizeof(storage), &got, nullptr)) {
        printf("USB_DESCRIPTOR read_failed winerror=%lu\n", GetLastError());
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    if (got < sizeof(*req) + 9 || got > storage.size()) return false;
    const unsigned char *p = req->Data;
    unsigned n = got - sizeof(*req), required = revision == 0x101 ? 4 : 3;
    unsigned interfaces = 0, ep = 0, hid = 0;
    int last = -1;
    ok = p[0] == 9 && p[1] == 2 && (unsigned)(p[2] | p[3] << 8) == n && p[4] == required && p[5] == 1 && p[7] == 0x80 &&
         p[8] == 250;
    printf("CONFIG bytes=%u interfaces=%u attributes=%02x max_power_mA=%u\n", n, p[4], p[7], 2 * p[8]);
    unsigned ep_mask[4] = {}, hid_mask = 0;
    for (unsigned off = 9; off < n;) {
        const auto *v = p + off;
        if (n - off < 2 || v[0] < 2 || v[0] > n - off) return false;
        if (v[1] == 4) {
            if (v[0] != 9 || v[2] >= required || v[2] != interfaces) return false;
            last = v[2];
            ++interfaces;
            printf("IF%u class=%02x subclass=%02x protocol=%02x endpoints=%u\n", last, v[5], v[6], v[7], v[4]);
            ok &= v[3] == 0 && v[4] == (last == 3 ? 2 : 1) && v[5] == 3 && v[6] == (last < 2 ? 1 : 0) &&
                  v[7] == (last < 2 ? last + 1 : 0);
        }
        else if (v[1] == 0x21) {
            if (last < 0 || v[0] != 9) return false;
            ++hid;
            hid_mask |= 1u << last;
            unsigned length = v[7] | v[8] << 8;
            printf("HID IF%u bcdHID=%02x%02x report_descriptor_bytes=%u\n", last, v[3], v[2], length);
            ok &= v[2] == 0x11 && v[3] == 1 && v[5] == 1 && v[6] == 0x22 && length > 0;
        }
        else if (v[1] == 5) {
            if (last < 0 || v[0] != 7) return false;
            ++ep;
            unsigned address = v[2], packet = v[4] | v[5] << 8;
            printf("EP IF%u address=%02x attributes=%02x max_packet=%u interval=%u\n", last, address, v[3], packet,
                   v[6]);
            bool out = last == 3 && address == 5;
            ok &= (address == unsigned(0x81 + last) || out) && v[3] == 3 &&
                  packet == (last < 2 || required == 3 ? 8u : 64u) &&
                  v[6] == (last < 2        ? 1
                           : required == 3 ? 10
                           : last == 2     ? 1
                                           : 4);
            ep_mask[last] |= out ? 2 : 1;
        }
        off += v[0];
    }
    for (unsigned i = 0; i < required; ++i)
        ok &= ep_mask[i] == (i == 3 ? 3u : 1u);
    return ok && interfaces == required && ep == (required == 4 ? 5u : 3u) && hid == required &&
           hid_mask == (1u << required) - 1;
}

int main(int argc, char **argv)
{
    const bool inspect = argc == 3 && !strcmp(argv[2], "--inspect");
    if ((argc != 2 && !inspect) || strlen(argv[1]) != 12 ||
        strspn(argv[1], "0123456789ABCDEFabcdef") != 12) {
        fputs("Usage: hpe-get-windows BOARD_USB_SERIAL [--inspect]\n", stderr);
        return 2;
    }
    std::wstring board_id = L"USB\\VID_303A&PID_400C\\";
    board_id.append(argv[1], argv[1] + 12);
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Read-only probe 303a:400c mode=%s\n", inspect ? "inspect" : "hpe-feature");
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 2;
    unsigned found = 0, passed = 0, failed = 0, revision = 0, mask = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA iface = {};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &iface)) break;
        DWORD len = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &len, nullptr);
        if (!len || len > 8192) continue;
        std::vector<unsigned char> storage(len);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info = {};
        info.cbSize = sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, len, nullptr, &info) ||
            !board_ancestor(info.DevInst, board_id))
            continue;
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        CM_Get_Device_IDW(info.DevInst, id, MAX_DEVICE_ID_LEN, 0);
        HANDLE dev =
            CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (dev == INVALID_HANDLE_VALUE) {
            printf("OPEN node=%ls winerror=%lu\n", id, GetLastError());
            ++failed;
            continue;
        }
        HIDD_ATTRIBUTES attr = {};
        attr.Size = sizeof(attr);
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS c = {};
        bool identity = HidD_GetAttributes(dev, &attr) && attr.VendorID == 0x303a && attr.ProductID == 0x400c &&
                        (attr.VersionNumber == 0x0101 || (inspect && attr.VersionNumber == 0x0100)) &&
                        (!revision || revision == attr.VersionNumber);
        if (!identity || !HidD_GetPreparsedData(dev, &pp) || HidP_GetCaps(pp, &c) != HIDP_STATUS_SUCCESS) {
            printf("SKIP node=%ls bcd=%04x caps_unavailable_or_wrong_revision\n", id, attr.VersionNumber);
            ++failed;
        }
        else {
            ++found;
            revision = attr.VersionNumber;
            printf("TLC node=%ls page=%04x usage=%04x in=%u out=%u feature=%u\n", id, c.UsagePage, c.Usage,
                   c.InputReportByteLength, c.OutputReportByteLength, c.FeatureReportByteLength);
            auto check = [&](unsigned fid) {
                if (get(dev, fid, c.FeatureReportByteLength))
                    ++passed;
                else
                    ++failed;
            };
            if (inspect) {
                struct Top {
                    unsigned mi, page, usage;
                };
                const bool hpe = revision == 0x101;
                const Top tlcs[] = {{0, 1, 6},      {1, 1, 2},  {2, hpe ? 13u : 12u, hpe ? 5u : 1u},
                                    {2, 13, 14},    {3, 1, 14}, {3, 12, 1},
                                    {3, 0xff61, 1}, {3, 1, 4}};
                unsigned idx = 0, limit = hpe ? 8 : 3;
                for (; idx < limit; ++idx) {
                    wchar_t token[12] = {};
                    swprintf(token, 12, L"&MI_%02X", tlcs[idx].mi);
                    if (tlcs[idx].page == c.UsagePage && tlcs[idx].usage == c.Usage && wcsstr(id, token)) break;
                }
                Layout layout{pp, c, {}, {}, {}};
                bool loaded = idx < limit && !(mask & (1u << idx)) && layout.load();
                bool packets = loaded && layout.check(idx, hpe);
                bool extents = loaded && layout.byte_extents();
                bool good = packets && extents;
                if (hpe && (idx == 2 || idx == 6))
                    printf("FIELD_CHECK MI_%02x usage=%04x/%04x packet_and_fields=%s byte_extents=%s\n",
                           tlcs[idx].mi, c.UsagePage, c.Usage, packets ? "PASS" : "FAIL", extents ? "PASS" : "FAIL");
                if (idx < limit) mask |= 1u << idx;
                printf("LAYOUT node=%ls %s\n", id, good ? "PASS" : "FAIL");
                if (!good) ++failed;
            }
            else if (c.UsagePage == 0xd && c.Usage == 5) {
                check(HPE_CAPS);
                check(HPE_CERT);
                check(HPE_LATENCY);
            }
            if (!inspect && c.UsagePage == 0xd && c.Usage == 0xe) {
                check(HPE_INPUT_MODE);
                check(HPE_SELECT);
            }
            if (!inspect && c.UsagePage == 0xff61 && c.Usage == 1) check(HPE_STUDIO_FEATURE);
        }
        if (pp) HidD_FreePreparsedData(pp);
        CloseHandle(dev);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (inspect) {
        bool config_ok = revision && configuration(revision, board_id);
        unsigned expected = revision == 0x101 ? 8 : 3;
        if (!config_ok || mask != (1u << expected) - 1 || found != expected) ++failed;
        printf("INSPECT revision=%04x configuration=%s TLCs=%u HPE=%s errors=%u\n", revision,
               config_ok ? "PASS" : "FAIL", found,
               revision == 0x101   ? "present"
               : revision == 0x100 ? "absent"
                                   : "unknown",
               failed);
        return failed ? 1 : 0;
    }
    printf("TLCs=%u features_pass=%u errors=%u\n", found, passed, failed);
    return found == 8 && passed == 6 && !failed ? 0 : 1;
}
