// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Guest-local producer for the fixed probes on Linux's dummy USB controller.
#if __has_include(<linux/usb/g_hid.h>)
#include <linux/usb/g_hid.h>
#else
// The prepared 6.1 userspace headers predate this ioctl in the pinned 6.18 kernel.
// Keep the layout from include/uapi/linux/usb/g_hid.h, including compat padding.
struct usb_hidg_report {
    uint8_t report_id, userspace_req;
    uint16_t length;
    uint8_t data[64], padding[4];
};
#define GADGET_HID_WRITE_GET_REPORT _IOW('g', 0x42, struct usb_hidg_report)
#endif

static void dummy_key_state(int fd, uint8_t usage, bool update_control)
{
    struct usb_hidg_report report = {.length = 8, .data = {0, 0, usage}};
    // With userspace_req=0 the function returns this state on subsequent EP0 reads.
    if (update_control) REQUIRE(ioctl(fd, GADGET_HID_WRITE_GET_REPORT, &report) == 0);
    REQUIRE(write(fd, report.data, report.length) == report.length);
    usleep(75000); // Give the host time to process a down and sample it on EP0.
}

static int dummy_keyboard(const char *sequence)
{
    char controller[32];
    FILE *udc = fopen("/sys/kernel/config/usb_gadget/test/UDC", "r");
    REQUIRE(udc && fgets(controller, sizeof(controller), udc) && fclose(udc) == 0);
    REQUIRE(!strcmp(controller, "dummy_udc.0\n"));
    int fd = open("/dev/hidg0", O_RDWR | O_CLOEXEC);
    REQUIRE(fd >= 0);
    if (!strcmp(sequence, "neutral")) {
        dummy_key_state(fd, 0, true);
    } else if (!strcmp(sequence, "wrong-key")) {
        dummy_key_state(fd, 0x69, true);
    } else if (!strcmp(sequence, "keys") || !strcmp(sequence, "stale-state")) {
        bool update_control = !strcmp(sequence, "keys");
        for (uint8_t usage = 0x68; usage <= 0x73; ++usage) {
            dummy_key_state(fd, usage, update_control);
            dummy_key_state(fd, 0, update_control);
        }
    } else {
        return 1;
    }
    return close(fd) != 0;
}
