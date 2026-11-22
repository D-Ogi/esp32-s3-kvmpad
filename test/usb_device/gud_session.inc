// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "gud_panel.h"
#include "gud_receiver.h"

static gud_receiver_t gud;
static uint8_t gud_scratch[GUD_R1_MAX_BYTES];
static unsigned gud_received, gud_frames, gud_resets;

static void gud_picture(void *context, gud_rect_t rect, const uint8_t *rows)
{
    (void)context;
    assert(rect.x == 0 && rect.y == 0 && rect.width == 640 && rect.height == 1);
    for (unsigned x = 0; x < 80; x++) assert(rows[x] == (uint8_t)x);
    gud_frames++;
}
void gud_panel_reset(void)
{
    gud_resets++;
    assert(gud_receiver_init(&gud, gud_scratch, sizeof(gud_scratch), gud_picture, NULL));
}
int gud_panel_get(uint8_t req, uint16_t value, uint8_t *p, size_t cap)
{
    return gud_receiver_get(&gud, req, value, p, cap);
}
bool gud_panel_set(uint8_t req, uint16_t value, const uint8_t *p, size_t len)
{
    return gud_receiver_set(&gud, req, value, p, len);
}
bool gud_panel_bulk(const uint8_t *data, size_t n)
{
    gud_received += (unsigned)n;
    return gud_receiver_bulk(&gud, data, n);
}
static void gud_stall(uint8_t type, uint8_t req, uint16_t index, uint16_t length)
{
    unsigned before = stalls;
    expect_stall = true;
    setup(type, req, 0, index, length);
    expect_stall = false;
    assert(stalls == before + 2);
}
static void gud_get(uint8_t req, uint16_t requested, uint8_t *data, unsigned expected)
{
    setup(0xc1, req, 0, 0, requested);
    assert(endpoint[0x80].pending && endpoint[0x80].len == expected);
    if (expected) memcpy(data, endpoint[0x80].data, expected);
    complete(0x80);
    status_stage(0);
}
static void gud_set(uint8_t req, const uint8_t *p, uint16_t length)
{
    setup(0x41, req, 0, 0, length);
    if (length) {
        assert(endpoint[0].pending && endpoint[0].len == length);
        memcpy(endpoint[0].data, p, length);
        complete(0);
    }
    status_stage(0x80);
}

static void gud_session(void)
{
    uint8_t normal[32], desc[255], product[64], state[26], row[80];
    const uint8_t rect[25] = {0,0,0,0, 0,0,0,0, 0x80,2,0,0, 1,0,0,0, 80};
    uint8_t on = 1;
    for (unsigned i = 0; i < sizeof(row); i++) row[i] = (uint8_t)i;
    unsigned windows_bytes = received;
    assert(descriptor(TUSB_DESC_DEVICE, 0, normal) == 18);
    gud_stall(0xc1, 1, 0, 30);
    assert(usb_hid_device_set_gud(true) == ESP_OK);
    service();
    wait_for_reconnect();
    gud_stall(0xc1, 1, 0, 30);
    reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, desc) == 18);
    assert(desc[8] == 0xfe && desc[9] == 0xff && desc[10] == 0x0d && desc[11] == 0xf0);
    assert(descriptor(TUSB_DESC_CONFIGURATION, 0, desc) == 75 && desc[2] == 75 && desc[4] == 3);
    assert(desc[9] == 9 && desc[13] == 1 && desc[14] == 255 && desc[20] == 1 &&
           desc[21] == TUSB_XFER_BULK && desc[22] == 64);
    for (unsigned hid = 0; hid < 2; ++hid) {
        const uint8_t *itf = desc + 25 + hid * TUD_HID_DESC_LEN;
        assert(itf[2] == 1 + hid && itf[4] == 1 && itf[5] == TUSB_CLASS_HID &&
               itf[6] == 1 && itf[7] == 1 + hid && itf[8] == 0);
        assert(itf[20] == 0x82 + hid && itf[21] == TUSB_XFER_INTERRUPT && itf[22] == 8 && itf[24] == 1);
    }
    assert(descriptor(TUSB_DESC_STRING, 2, product) == 22);
    for (unsigned i = 0; i < 10; i++) assert(product[2+2*i] == "KVMpad GUD"[i] && !product[3+2*i]);
    configure(1);
    check_monitor_state(KVM_USB_MOUNTED);
    assert(usb_hid_device_interface_count() == 2 && usb().state == KVM_USB_MOUNTED);
    drain();
    gud_stall(0xc0, 1, 0, 30); // Wrong recipient.
    gud_stall(0xc1, 1, 1, 30); // The keyboard interface cannot receive a GUD request.
    gud_get(1, 30, desc, 30);
    assert(!memcmp(desc, (uint8_t[]){0x4d, 0x61, 0x50, 0x1d, 1}, 5));
    gud_get(0x41, 320, NULL, 0); // Optional property list terminates with a zero-length IN.
    gud_get(0x55, 3072, desc, 48);
    assert(desc[28] == 0x20 && desc[29] == 3 && desc[36] == 0x58 && desc[37] == 2);
    memcpy(state, desc, 24);
    state[24] = 1; state[25] = 0;
    gud_set(0x61, state, sizeof(state));
    gud_set(0x63, &on, 1);
    gud_set(0x62, NULL, 0);
    gud_set(0x64, &on, 1);
    gud_set(0x60, rect, sizeof(rect));
    // A short OUT after a valid transfer must never re-use its old zero tail.
    setup(0x41, 0x60, 0, 0, sizeof(rect));
    memcpy(endpoint[0].data, rect, 24);
    endpoint[0].len = 24;
    unsigned before = stalls;
    expect_stall = true;
    complete(0);
    expect_stall = false;
    assert(stalls == before + 2 && !gud.expected);
    gud_get(0, 1, desc, 1);
    assert(desc[0] == 4);
    setup(0x41, 0x63, 0, 0, 1);
    endpoint[0].len = 0;
    before = stalls;
    expect_stall = true;
    complete(0);
    expect_stall = false;
    assert(stalls == before + 2 && gud.controller_enabled);
    gud_get(0, 1, desc, 1);
    assert(desc[0] == 4);
    gud_set(0x60, rect, sizeof(rect));
    queue_bulk(row, 64);
    service();
    kvm_hid_intent_t key = {.type = KVM_HID_KEY_DOWN, .data.key.usage_id = 4};
    assert(hid_dispatch_local(&key) == ESP_OK);
    service();
    assert(endpoint[0x82].pending && endpoint[0x82].len == 8 && endpoint[0x82].data[2] == 4);
    complete(0x82);
    kvm_hid_intent_t move = {.type = KVM_HID_MOUSE_MOVE, .data.mouse_move = {.dx = 3, .dy = -5}};
    assert(hid_dispatch_local(&move) == ESP_OK);
    service();
    assert(endpoint[0x83].pending && endpoint[0x83].data[1] == 3 && endpoint[0x83].data[2] == (uint8_t)-5);
    complete(0x83);
    assert(gud.used == 64 && snapshot().keys_down == 1);
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    service();
    assert(gud.used == 64 && !gud_frames && snapshot().ledger_empty);
    queue_bulk(row+64, 16);
    service();
    assert(gud.used == 64 && !gud_frames);
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
    service();
    drain();
    queue_bulk(row+64, 16);
    service();
    assert(gud_frames == 1 && gud_received == 80 && received == windows_bytes);

    gud_set(0x60, rect, sizeof(rect));
    queue_bulk(row, 64);
    service();
    unsigned before_reset = gud_resets;
    reset_bus();
    assert(gud_resets > before_reset && !gud.expected && !gud.committed);
    configure(1);
    queue_bulk(row+64, 16);
    service();
    assert(gud_frames == 1);
    configure(0);
    gud_stall(0xc1, 1, 0, 30);
    assert(usb_hid_device_set_gud(false) == ESP_OK);
    service(); wait_for_reconnect(); reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, desc) == 18 && !memcmp(normal, desc, 18));
    configure(1); drain();
    assert(usb_hid_device_interface_count() == KVM_HID_ITF_COUNT && snapshot().ledger_empty);
    gud_stall(0xc1, 1, 0, 30);
    puts("PASS GUD + boot keyboard/mouse: dispatched HID during fragmented video, neutral on suspend, resumed frame, reset and HID return");
}
