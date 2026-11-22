// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "session_platform.h"
#include "device/dcd.h"
#include "hid_dispatch.h"
#include "usb_hid_device.h"
#include <stdio.h>
#if KVM_HPE_FOUR
#include "../../components/usb_hid_device/src/hpe_cert.h"
#endif

// The device controller only records buffers. The test host clocks every
// control stage and interrupt IN completion. No OS HID APIs are involved.
typedef struct { uint8_t *data; unsigned len; bool pending; } transfer_t;
static transfer_t endpoint[256];
static tinyusb_config_t config;
static unsigned keyboard_presses, keyboard_neutrals;
static bool expect_stall;
static bool sof_enabled;
static unsigned stalls;
static uint64_t now = 1000000;
#if CONFIG_KVM_USB_SCREENSHOT
static bool testing_storage;
#endif
static void service(void) { while (tud_task_event_ready()) tud_task_ext(0, false); }
uint32_t tusb_time_millis_api(void) { return (uint32_t)(now / 1000); }
int64_t esp_timer_get_time(void) { return (int64_t)now; }
uint32_t esp_random(void) { static uint32_t nonce; return ++nonce; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *t) { *t = a->callback; return ESP_OK; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) { assert(t && us); return ESP_OK; }
esp_err_t tinyusb_driver_install(const tinyusb_config_t *cfg)
{
    config = *cfg;
    const tusb_rhport_init_t init = {.role=TUSB_ROLE_DEVICE, .speed=TUSB_SPEED_FULL};
    return tud_rhport_init(0, &init) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)config.descriptor.device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t i) { assert(i == 0); return config.descriptor.full_speed_config; }
const uint16_t *tud_descriptor_string_cb(uint8_t i, uint16_t lang)
{
    static uint16_t desc[32];
    (void)lang;
    if (i == 0 || i >= config.descriptor.string_count) return NULL;
    size_t n = strlen(config.descriptor.string[i]);
    assert(n < 32);
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 + n * 2));
    for (size_t j = 0; j < n; ++j) desc[j + 1] = (uint8_t)config.descriptor.string[i][j];
    return desc;
}
static void event(unsigned id, bool wake)
{
    tinyusb_event_t e = {.id=id, .suspended={.remote_wakeup=wake}};
    config.event(&e, NULL);
}
void tud_mount_cb(void) { event(TINYUSB_EVENT_ATTACHED, false); }
void tud_umount_cb(void) { event(TINYUSB_EVENT_DETACHED, false); }
void tud_suspend_cb(bool wake) { event(TINYUSB_EVENT_SUSPENDED, wake); }
void tud_resume_cb(void) { event(TINYUSB_EVENT_RESUMED, false); }

bool dcd_init(uint8_t port, const tusb_rhport_init_t *init) { return !port && init->role == TUSB_ROLE_DEVICE; }
void dcd_int_handler(uint8_t port) { (void)port; assert(false); }
void dcd_int_enable(uint8_t port) { assert(!port); }
void dcd_int_disable(uint8_t port) { assert(!port); }
void dcd_remote_wakeup(uint8_t port) { (void)port; assert(false); }
void dcd_sof_enable(uint8_t port, bool enable) { assert(!port); sof_enabled = enable; }
void dcd_set_address(uint8_t port, uint8_t addr) { assert(!port && addr < 128); }
bool dcd_edpt_open(uint8_t port, const tusb_desc_endpoint_t *desc) { return !port && desc->wMaxPacketSize <= 64; }
bool dcd_edpt_iso_alloc(uint8_t port, uint8_t ep, uint16_t size) { (void)port; (void)ep; (void)size; return false; }
bool dcd_edpt_iso_activate(uint8_t port, const tusb_desc_endpoint_t *d) { (void)port; (void)d; return false; }
void dcd_edpt_close_all(uint8_t port) { assert(!port); memset(endpoint, 0, sizeof(endpoint)); }
void dcd_edpt_stall(uint8_t port, uint8_t ep)
{
    assert(expect_stall && port == 0 && (ep == 0 || ep == 0x80));
    ++stalls;
}
void dcd_edpt_clear_stall(uint8_t port, uint8_t ep) { assert(!port && !endpoint[ep].pending); }
bool dcd_edpt_xfer(uint8_t port, uint8_t ep, uint8_t *data, uint16_t len, bool isr)
{
    bool bulk_storage = false;
#if CONFIG_KVM_USB_SCREENSHOT
    bulk_storage = testing_storage && (ep == 1 || ep == 0x81);
#endif
    assert(!port && !isr && len <= (bulk_storage ? 512 : 64) && !endpoint[ep].pending);
    endpoint[ep] = (transfer_t){data, len, true};
    if (ep == 0x81 && !bulk_storage) {
        static const uint8_t neutral[8];
        assert(len == sizeof(neutral));
        if (memcmp(data, neutral, len)) ++keyboard_presses;
        else ++keyboard_neutrals;
    }
    return true;
}
static void complete(uint8_t ep)
{
    assert(endpoint[ep].pending);
    endpoint[ep].pending = false;
    dcd_event_xfer_complete(0, ep, endpoint[ep].len, XFER_RESULT_SUCCESS, false);
    service();
}
static void drain(void)
{
    for (unsigned pass = 0; pass < 50; ++pass) {
        service();
        bool active = false;
        for (unsigned ep = 0x81; ep <= 0x84; ++ep) {
            if (endpoint[ep].pending) { complete((uint8_t)ep); active = true; }
        }
        if (!active) return;
    }
    assert(false);
}
static void setup(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length)
{
    const uint8_t wire[] = {type, request, (uint8_t)value, (uint8_t)(value >> 8),
                            (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    dcd_event_setup_received(0, wire, false);
    service();
}
static void status_stage(uint8_t ep)
{
    assert(endpoint[ep].pending && !endpoint[ep].len);
    complete(ep);
}
static void configure(uint8_t n)
{
    setup(0, TUSB_REQ_SET_CONFIGURATION, n, 0, 0);
    status_stage(0x80);
}
static void check_input_report(uint8_t interface, uint16_t requested, const uint8_t *expected, uint16_t length)
{
    setup(0xa1, HID_REQ_CONTROL_GET_REPORT, HID_REPORT_TYPE_INPUT << 8, interface, requested);
    assert(endpoint[0x80].pending && endpoint[0x80].len == length);
    assert(memcmp(endpoint[0x80].data, expected, length) == 0);
    complete(0x80);
    status_stage(0);
}

static void set_keyboard_idle(uint8_t keyboard, uint8_t rate)
{
    setup(0x21, HID_REQ_CONTROL_SET_IDLE, (uint16_t)rate << 8, keyboard, 0);
    status_stage(0x80);
    setup(0xa1, HID_REQ_CONTROL_GET_IDLE, 0, keyboard, 1);
    assert(endpoint[0x80].pending && endpoint[0x80].len == 1 && endpoint[0x80].data[0] == rate);
    complete(0x80);
    status_stage(0);
}

static void frames(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) {
        now += 1000;
        if (sof_enabled) dcd_event_sof(0, (uint32_t)(now / 1000), false);
        service();
    }
}

static void check_keyboard_idle(uint8_t keyboard, uint8_t ep)
{
    drain();
    kvm_hid_intent_t key = {.type=KVM_HID_KEY_DOWN, .data.key.usage_id=4};
    assert(hid_dispatch_local(&key) == ESP_OK);
    drain();
    assert(!sof_enabled);
    set_keyboard_idle(keyboard, 2);
    frames(7);
    assert(sof_enabled && !endpoint[ep].pending);
    frames(1);
    assert(endpoint[ep].pending && endpoint[ep].len == 8 && endpoint[ep].data[2] == 4);
    frames(4); // A busy endpoint keeps a single copy in flight.
    complete(ep);
    frames(5);
    set_keyboard_idle(keyboard, 1); // Fewer than 4 ms left: finish the old period.
    frames(2);
    assert(!endpoint[ep].pending);
    frames(1);
    assert(endpoint[ep].pending && endpoint[ep].data[2] == 4);
    complete(ep);
    frames(2);
    key.type = KVM_HID_KEY_UP;
    assert(hid_dispatch_local(&key) == ESP_OK);
    service();
    const uint8_t released[8] = {0};
    assert(endpoint[ep].pending && !memcmp(endpoint[ep].data, released, 8));
    complete(ep);
    frames(3);
    assert(!endpoint[ep].pending);
    frames(1);
    assert(endpoint[ep].pending && !memcmp(endpoint[ep].data, released, 8));
    complete(ep);
    set_keyboard_idle(keyboard, 0);
    frames(20);
    assert(!sof_enabled && !endpoint[ep].pending);
    set_keyboard_idle(keyboard, 255);
    frames(999);
    assert(!endpoint[ep].pending);
    frames(1);
    assert(endpoint[ep].pending); // 20 + 1000 ms since the last transfer.
    complete(ep);
    frames(12);
    set_keyboard_idle(keyboard, 2); // The new interval has already elapsed.
    frames(1);
    assert(endpoint[ep].pending);
    complete(ep);
    frames(6);
    set_keyboard_idle(keyboard, 0); // The final 2 ms belong to the previous interval.
    frames(2);
    assert(endpoint[ep].pending);
    complete(ep);
    assert(!sof_enabled);
    puts("PASS keyboard SET_IDLE: 4/8/1020 ms, duration changes, held and released state, zero rate.");
}
static void reset_bus(void)
{
    memset(endpoint, 0, sizeof(endpoint)); // controller drops transfers at reset
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
    service();
}
static void set_report(uint8_t itf, uint16_t value, const uint8_t *bytes, unsigned length)
{
    setup(0x21, HID_REQ_CONTROL_SET_REPORT, value, itf, (uint16_t)length);
    assert(endpoint[0].pending && endpoint[0].len == length);
    memcpy(endpoint[0].data, bytes, length);
    complete(0);
    status_stage(0x80);
}
static kvm_hid_snapshot_t snapshot(void)
{
    kvm_hid_snapshot_t snap;
    hid_dispatch_get_snapshot(&snap, NULL);
    return snap;
}
static usb_hid_status_t usb(void)
{
    usb_hid_status_t st;
    usb_hid_device_get_status(&st);
    return st;
}

static void check_led(bool known, uint8_t value, uint32_t reports)
{
    usb_hid_status_t s = usb();
    if (s.led_known != known || s.led_state != value || s.led_reports != reports)
        fprintf(stderr,"LED: expected known=%u value=%u reports=%u, actual known=%u value=%u reports=%u\n",
                (unsigned)known, (unsigned)value, (unsigned)reports,
                (unsigned)s.led_known, (unsigned)s.led_state, (unsigned)s.led_reports);
    assert(s.led_known == known && s.led_state == value && s.led_reports == reports);
}

static void check_first_led(uint8_t keyboard, bool on)
{
    uint32_t reports = usb().led_reports;
    check_led(false, 0, reports);
    const uint8_t bytes[2] = {0, 2};
    set_report(keyboard, 0x0201, bytes, 1);   // This keyboard has no numbered reports.
    set_report(keyboard, 0x0200, bytes, 2);   // A Windows API's ID prefix is absent on this wire.
    set_report(keyboard, 0x0000, bytes, 1);   // Undefined control report type.
    set_report(keyboard + 1, 0x0200, bytes, 1); // Mouse.
    check_led(false, 0, reports);
    // The wire length in SETUP promises one byte, but DATA actually ends in a ZLP.
    setup(0x21, HID_REQ_CONTROL_SET_REPORT, 0x0200, keyboard, 1);
    assert(endpoint[0].pending && endpoint[0].len == 1);
    endpoint[0].len = 0;
    expect_stall = true;
    complete(0);
    expect_stall = false;
    if (endpoint[0x80].pending) status_stage(0x80);
    check_led(false, 0, reports);
    uint8_t led = on ? 2 : 0;
    setup(0x21, HID_REQ_CONTROL_SET_REPORT, 0x0200, keyboard, 1);
    assert(endpoint[0].pending && endpoint[0].len == 1);
    endpoint[0].data[0] = led;
    complete(0);
    check_led(false, 0, reports);
    status_stage(0x80);
    check_led(true, led, reports+1);
}
#if CONFIG_KVM_USB_MONITOR_EVAL
#include "monitor_wire.h"
#include "usb_monitor_chart.h"

static esp_timer_handle_t reconnect_timer;
static unsigned disconnects, connects, received;
static monitor_wire_t wire;
static uint8_t chart[16 + MONITOR_JPEG_LIMIT], jpeg[MONITOR_JPEG_LIMIT];
static size_t chart_size;
static unsigned pictures;
static bool have_picture;

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    assert(!reconnect_timer && us >= 250000);
    reconnect_timer = t;
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t t) { assert(t); return ESP_OK; }
void dcd_connect(uint8_t port) { assert(port == 0); ++connects; }
void dcd_disconnect(uint8_t port) { assert(port == 0); ++disconnects; }
static void collect_chart(const uint8_t *data, size_t n)
{
    assert(n <= sizeof(chart) - chart_size);
    memcpy(chart + chart_size, data, n);
    chart_size += n;
}
static uint8_t *reserve_jpeg(void) { return jpeg; }
static void complete_jpeg(size_t n)
{
    assert(n == chart_size - 16 && !memcmp(jpeg, chart + 16, n));
    ++pictures;
    have_picture = true;
}
void usb_monitor_reset(void)
{
    wire = (monitor_wire_t){0};
    have_picture = false;
}
void usb_monitor_feed(const uint8_t *data, size_t n)
{
    received += (unsigned)n;
    monitor_wire_feed(&wire, data, n, reserve_jpeg, complete_jpeg);
}
static void wait_for_reconnect(void)
{
    assert(disconnects == connects + 1 && reconnect_timer);
    now += 250000;
    esp_timer_handle_t fired = reconnect_timer;
    reconnect_timer = NULL;
    fired(NULL);
    service();
    assert(disconnects == connects);
}
static unsigned descriptor(uint8_t type, uint8_t index, uint8_t *out)
{
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, (uint16_t)((type << 8) | index),
          type == TUSB_DESC_STRING ? 0x0409 : 0, 255);
    unsigned total = 0;
    while (endpoint[0x80].pending) {
        assert(total + endpoint[0x80].len <= 255);
        memcpy(out + total, endpoint[0x80].data, endpoint[0x80].len);
        total += endpoint[0x80].len;
        complete(0x80);
    }
    status_stage(0);
    return total;
}
static void queue_bulk(const uint8_t *data, size_t size)
{
    assert(endpoint[1].pending && size <= endpoint[1].len);
    memcpy(endpoint[1].data, data, size);
    endpoint[1].pending = false;
    dcd_event_xfer_complete(0, 1, (unsigned)size, XFER_RESULT_SUCCESS, false);
}
static void send_chart_from(size_t offset)
{
    while (offset < chart_size) {
        size_t n = chart_size - offset;
        if (n > 64) n = 64;
        queue_bulk(chart + offset, n);
        service();
        offset += n;
    }
}
static void check_monitor_state(kvm_usb_state_t expected)
{
    assert(usb_hid_device_monitor_state() == expected);
    assert(usb_hid_device_monitor_mounted() == (expected == KVM_USB_MOUNTED));
}
static void check_monitor_suspend(void)
{
    usb_monitor_test_chart(collect_chart);
    assert(chart_size > 128);
    usb_monitor_port_bytes_t initial = usb_hid_device_monitor_bytes();
    send_chart_from(0);
    assert(have_picture && pictures == 1 && received == chart_size);

    // A completed OUT packet precedes suspend in the USB task queue. The DCD's
    // live suspended bit is already set by the time the task reads that packet.
    queue_bulk(chart, 64);
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    assert(tud_suspended());
    service();
    check_monitor_state(KVM_USB_SUSPENDED);
    assert(received == chart_size + 64);
    assert(have_picture && pictures == 1 && wire.remaining == chart_size - 64);
    queue_bulk(chart, 16);  // Completion delivered while the task already considers the bus suspended.
    service();
    assert(received == chart_size + 64 && wire.remaining == chart_size - 64);
    usb_monitor_port_bytes_t suspended = usb_hid_device_monitor_bytes();
    assert(suspended.passed_bytes - initial.passed_bytes == chart_size + 64);
    assert(suspended.filtered_bytes - initial.filtered_bytes == 16);

    // Resume, a second packet, and another suspend all arrive before the task runs.
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
    queue_bulk(chart + 64, 64);
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    service();
    check_monitor_state(KVM_USB_SUSPENDED);
    assert(received == chart_size + 128);
    assert(have_picture && pictures == 1 && wire.remaining == chart_size - 128);
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
    service();
    check_monitor_state(KVM_USB_MOUNTED);
    assert(have_picture && pictures == 1);
    send_chart_from(128);
    assert(pictures == 2 && received == 2 * chart_size && !wire.remaining && !wire.used);
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    service();
    check_monitor_state(KVM_USB_SUSPENDED);
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
    service();
    check_monitor_state(KVM_USB_MOUNTED);
    assert(have_picture && pictures == 2 && received == 2 * chart_size);
    usb_monitor_port_bytes_t resumed = usb_hid_device_monitor_bytes();
    assert(resumed.passed_bytes - initial.passed_bytes == 2 * chart_size);
    assert(resumed.filtered_bytes == suspended.filtered_bytes);
    printf("PASS configured suspend/resume retained the last complete JPEG without further transfers. Both "
           "%zu-byte chart records matched, including queued OUT before suspend and between resume/suspend. "
           "OUT while suspended was ignored.\n", chart_size);
}
static void composite_session(const uint8_t *hid_device, const uint8_t *serial, unsigned serial_len)
{
    uint8_t bytes[255];
    const char *const sizes[] = {"esp32s3udisp0_R640x480_Ejpg4_Fps10_Bl96",
                                 "esp32s3udisp0_R1024x600_Ejpg4_Fps10_Bl96",
                                 "esp32s3udisp0_Ejpg4_Fps10_Bl96"};
    for (unsigned mode = 0; mode < 3; ++mode) {
        if (mode == 2) assert(usb_hid_device_set_monitor_windows(true) == ESP_OK);
        else assert(usb_hid_device_set_monitor_controls(true, mode == 1) == ESP_OK);
        service();
        assert(usb_hid_device_interface_count() == 0 && usb().state == KVM_USB_DETACHED);
        wait_for_reconnect();
        tud_mount_cb(); // A stale configuration event before the new bus reset is ignored by both classes.
        assert(usb().state == KVM_USB_DETACHED && !usb_hid_device_monitor_mounted());
        reset_bus();
        assert(descriptor(TUSB_DESC_DEVICE, 0, bytes) == 18 && bytes[4] == 0 && bytes[8] == 0x3a &&
               bytes[9] == 0x30 && bytes[10] == 0x86 && bytes[11] == 0x29 && bytes[16] == 3);
        assert(descriptor(TUSB_DESC_STRING, 3, bytes) == serial_len && !memcmp(bytes, serial, serial_len));
        unsigned n = descriptor(TUSB_DESC_CONFIGURATION, 0, bytes);
        const uint8_t vendor[] = {9,4,0,0,2,0xff,0,0,4, 7,5,1,2,64,0,0, 7,5,0x81,2,64,0,0};
        assert(n == 82 && bytes[2] == n && !bytes[3] && bytes[4] == 3 && !memcmp(bytes+9,vendor,sizeof(vendor)));
        const uint8_t keyboard_report[] = {TUD_HID_REPORT_DESC_KEYBOARD()};
        const uint8_t mouse_report[] = {TUD_HID_REPORT_DESC_MOUSE()};
        const uint8_t *report[] = {keyboard_report, mouse_report};
        const size_t lengths[] = {sizeof(keyboard_report),sizeof(mouse_report)};
        for (unsigned hid = 0; hid < 2; ++hid) {
            const uint8_t *itf = bytes + 32 + 25*hid;
            assert(itf[0] == 9 && itf[1] == TUSB_DESC_INTERFACE && itf[2] == hid+1 && itf[4] == 1 &&
                   itf[5] == TUSB_CLASS_HID && itf[6] == 1 && itf[7] == hid+1 && itf[8] == hid+5);
            assert(itf[10] == HID_DESC_TYPE_HID && itf[15] == HID_DESC_TYPE_REPORT &&
                   itf[16] == lengths[hid] && itf[17] == 0);
            assert(itf[19] == TUSB_DESC_ENDPOINT && itf[20] == 0x82+hid && itf[21] == 3 &&
                   itf[22] == 8 && itf[23] == 0 && itf[24] == 1);
        }
        const char *names[] = {sizes[mode], "KVMpad Keyboard", "KVMpad Mouse"};
        for (unsigned string = 0; string < 3; ++string) {
            assert(descriptor(TUSB_DESC_STRING, 4+string, bytes) == 2+2*strlen(names[string]));
            for (unsigned j = 0; names[string][j]; ++j)
                assert(bytes[2+2*j] == names[string][j] && bytes[3+2*j] == 0);
        }
        configure(1);
        check_monitor_state(KVM_USB_MOUNTED);
        assert(usb_hid_device_interface_count() == 2 && usb().state == KVM_USB_MOUNTED);
        drain();
        assert(!snapshot().neutralizing && snapshot().ledger_empty);
        for (unsigned hid = 0; hid < 2; ++hid) {
            setup(0x81,TUSB_REQ_GET_DESCRIPTOR,HID_DESC_TYPE_REPORT << 8,hid+1,(uint16_t)lengths[hid]);
            for (unsigned pos = 0; pos < lengths[hid];) {
                unsigned chunk = endpoint[0x80].len;
                assert(chunk && chunk <= lengths[hid]-pos && !memcmp(endpoint[0x80].data,report[hid]+pos,chunk));
                pos += chunk;
                complete(0x80);
            }
            status_stage(0);
        }
        expect_stall = true;
        unsigned before_stall = stalls;
        setup(0xa1,HID_REQ_CONTROL_GET_REPORT,0x0300|HPE_CERT,2,257);
        expect_stall = false;
        assert(stalls == before_stall+2);
        const uint8_t led = 2;
        check_first_led(1, true);
        check_keyboard_idle(1, 0x82);
        uint32_t led_reports = usb().led_reports;
        setup(0x21,HID_REQ_CONTROL_SET_PROTOCOL,HID_PROTOCOL_BOOT,2,0);
        status_stage(0x80);
        assert(usb().led_state == led && usb().protocol[1] == HID_PROTOCOL_BOOT);
        kvm_hid_intent_t key = {.type=KVM_HID_KEY_DOWN, .data.key.usage_id=4};
        assert(hid_dispatch_local(&key) == ESP_OK);
        service();
        assert(endpoint[0x82].pending && endpoint[0x82].len == 8 && endpoint[0x82].data[2] == 4);
        assert(!endpoint[0x81].pending && !endpoint[0x84].pending);
        complete(0x82);
        check_input_report(1, 8, (const uint8_t[]){0, 0, 4, 0, 0, 0, 0, 0}, 8);
        kvm_hid_intent_t move = {.type=KVM_HID_MOUSE_MOVE,.data.mouse_move={.dx=5,.dy=-3}};
        assert(hid_dispatch_local(&move) == ESP_OK);
        service();
        assert(endpoint[0x83].pending && endpoint[0x83].len == 3 && endpoint[0x83].data[1] == 5 &&
               endpoint[0x83].data[2] == (uint8_t)-3);
        complete(0x83);
        unsigned before = received;
        queue_bulk(chart,64);
        service();
        assert(received == before+64 && snapshot().keys_down == 1);
        dcd_event_bus_signal(0,DCD_EVENT_SUSPEND,false);
        service();
        check_monitor_state(KVM_USB_SUSPENDED);
        assert(usb().state == KVM_USB_SUSPENDED && snapshot().ledger_empty);
        check_led(true, led, led_reports);
        dcd_event_bus_signal(0,DCD_EVENT_RESUME,false);
        service();
        drain();
        assert(usb().state == KVM_USB_MOUNTED && usb_hid_device_monitor_mounted() && !snapshot().neutralizing);
        check_led(true, led, led_reports);
        // DATA completed, then the bus resets before the host acknowledges status.
        setup(0x21,HID_REQ_CONTROL_SET_REPORT,0x0200,1,1);
        endpoint[0].data[0]=0;
        complete(0);
        reset_bus();
        assert(!usb_hid_device_monitor_mounted() && snapshot().ledger_empty && !usb().led_state);
        check_led(false, 0, led_reports);
        configure(1);
        drain();
        check_first_led(1, false);
        assert(hid_dispatch_local(&key) == ESP_OK);
        service();
        assert(snapshot().keys_down == 1 && endpoint[0x82].pending);
        // Leave this key held and its transfer awaiting completion while the profile switches.
    }
    assert(usb_hid_device_set_monitor_controls(false,false) == ESP_OK);
    service();
    assert(snapshot().ledger_empty && !usb_hid_device_interface_count());
    wait_for_reconnect();
    reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE,0,bytes) == 18 && !memcmp(bytes,hid_device,18));
    assert(descriptor(TUSB_DESC_CONFIGURATION,0,bytes) == config.descriptor.full_speed_config[2]);
    assert(bytes[4] == KVM_HID_ITF_COUNT && !memcmp(bytes, config.descriptor.full_speed_config, bytes[2]));
    configure(1);
    drain();
    assert(usb_hid_device_interface_count() == KVM_HID_ITF_COUNT && !snapshot().neutralizing && snapshot().ledger_empty);
    printf("PASS composite 303A:2986 MI_00 VGA/native/Windows, real HID report descriptors, local keyboard/mouse and bulk OUT, "
           "neutral barriers, loss/resume/reset and return to %u HID interfaces\n", KVM_HID_ITF_COUNT);
    puts("PASS MI_01 LED On survives suspend/resume; interrupted SET_REPORT is discarded on reset; "
         "remount is Unknown until a complete one-byte output confirms Off.");
}
static void monitor_session(void)
{
    uint8_t bytes[255], serial[255], previous[255];
    check_monitor_state(KVM_USB_DETACHED);
    reset_bus();
    configure(1);
    drain();
    unsigned serial_len = descriptor(TUSB_DESC_STRING, 3, serial);
    assert(serial_len == 26);
    assert(descriptor(TUSB_DESC_DEVICE, 0, previous) == 18 && previous[10] == 0x0c && previous[11] == 0x40);
    assert(usb_hid_device_set_monitor(true) == ESP_OK);
    service();
    assert(!usb_hid_device_monitor_mounted() && usb().state == KVM_USB_DETACHED && snapshot().ledger_empty);
    unsigned before_stall = stalls;
    expect_stall = true;
#if KVM_HPE_FOUR
    setup(0xa1, HID_REQ_CONTROL_GET_REPORT, (HID_REPORT_TYPE_FEATURE << 8) | HPE_CAPS, KVM_HID_ITF_PTP, 2);
#else
    setup(0xa1, HID_REQ_CONTROL_GET_PROTOCOL, 0, 0, 1);
#endif
    expect_stall = false;
    assert(stalls == before_stall+2);
    assert(usb_hid_device_set_monitor(false) == ESP_OK);
    service();
    wait_for_reconnect();
    reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, bytes) == 18 && !memcmp(bytes, previous, 18));
    configure(1);
    drain();

    assert(usb_hid_device_set_monitor(true) == ESP_OK);
    service();
    wait_for_reconnect();
    // Soft connect leaves the previous TinyUSB configuration intact. No RX is
    // allowed until the new controller reset and SET_CONFIGURATION sequence.
    assert(tud_mounted());
    check_monitor_state(KVM_USB_DETACHED);
    tud_mount_cb();
    check_monitor_state(KVM_USB_DETACHED);
    reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, bytes) == 18 && bytes[8] == 0x3a && bytes[9] == 0x30 &&
           bytes[10] == 0x87 && bytes[11] == 0x29 && bytes[14] == 1 && bytes[15] == 2 && bytes[16] == 3);
    assert(descriptor(TUSB_DESC_STRING, 3, bytes) == serial_len && !memcmp(serial, bytes, serial_len));
    const char *caps = "esp32s3udisp0_R640x480_Ejpg4_Fps10_Bl96";
    for (unsigned index = 2; index <= 4; index += 2) {
        unsigned size = descriptor(TUSB_DESC_STRING, (uint8_t)index, bytes);
        assert(size == 2 + 2 * strlen(caps) && bytes[0] == size && bytes[1] == TUSB_DESC_STRING);
        for (size_t j = 0; caps[j]; ++j) assert(bytes[2 + 2*j] == caps[j] && !bytes[3 + 2*j]);
    }
    assert(descriptor(TUSB_DESC_CONFIGURATION, 0, bytes) == 32 && bytes[4] == 1);
    const uint8_t vendor[] = {9,4,0,0,2,0xff,0,0,4, 7,5,1,2,64,0,0, 7,5,0x81,2,64,0,0};
    assert(!memcmp(bytes + 9, vendor, sizeof(vendor)));
    configure(1);
    check_monitor_state(KVM_USB_MOUNTED);
    assert(usb().state == KVM_USB_DETACHED);
    check_monitor_suspend();
    queue_bulk(chart, 64);
    service();
    assert(have_picture && wire.remaining == chart_size - 64);
    configure(0);
    check_monitor_state(KVM_USB_DETACHED);
    assert(!have_picture && !wire.remaining && !wire.used);
    // Unconfigured bus traffic must not revive a detached monitor session.
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    service();
    check_monitor_state(KVM_USB_DETACHED);
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
    service();
    check_monitor_state(KVM_USB_DETACHED);
    configure(1);
    check_monitor_state(KVM_USB_MOUNTED);
    send_chart_from(0);
    assert(have_picture && pictures == 3);
    queue_bulk(chart, 64);
    service();
    reset_bus();
    check_monitor_state(KVM_USB_DETACHED);
    assert(!have_picture && !wire.remaining && !wire.used);
    configure(1);
    send_chart_from(0);
    assert(have_picture && pictures == 4);
    queue_bulk(chart, 64);
    service();
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    service();
    check_monitor_state(KVM_USB_SUSPENDED);
    memset(endpoint, 0, sizeof(endpoint));
    dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
    service();
    check_monitor_state(KVM_USB_DETACHED);
    assert(!have_picture && !wire.remaining && !wire.used);
    reset_bus();
    configure(1);
    assert(usb_hid_device_monitor_mounted() && usb().state == KVM_USB_DETACHED);
    assert(usb_hid_device_set_monitor_mode(true, true) == ESP_OK);
    service();
    wait_for_reconnect();
    reset_bus();
    const char *native = "esp32s3udisp0_R1024x600_Ejpg4_Fps10_Bl96";
    for (unsigned index = 2; index <= 4; index += 2) {
        unsigned size = descriptor(TUSB_DESC_STRING, (uint8_t)index, bytes);
        assert(size == 2 + 2 * strlen(native) && bytes[1] == TUSB_DESC_STRING);
        for (size_t j = 0; native[j]; ++j) assert(bytes[2 + 2*j] == native[j] && !bytes[3 + 2*j]);
    }
    assert(descriptor(TUSB_DESC_STRING, 3, bytes) == serial_len && !memcmp(bytes, serial, serial_len));
    configure(1);
    check_monitor_state(KVM_USB_MOUNTED);
    assert(usb_hid_device_set_monitor(false) == ESP_OK);
    service();
    check_monitor_state(KVM_USB_DETACHED);
    unsigned before = received;
    queue_bulk(chart, 1); // a pending OUT completion from the previous generation
    service();
    assert(received == before);
    wait_for_reconnect();
    reset_bus();
    assert(descriptor(TUSB_DESC_DEVICE, 0, bytes) == 18 && !memcmp(bytes, previous, 18));
    assert(descriptor(TUSB_DESC_STRING, 3, bytes) == serial_len && !memcmp(bytes, serial, serial_len));
    configure(1);
    drain();
    assert(usb().state == KVM_USB_MOUNTED && snapshot().ledger_empty);
    composite_session(previous, serial, serial_len);
    puts("PASS HID -> display-only -> HID: EP0 descriptors and serial, fresh JPEG after deconfiguration/reset, "
         "detached after unplug while suspended, stale OUT and canceled switch.");
}
#endif
#if CONFIG_KVM_USB_GUD
#include "gud_session.inc"
#endif
#if CONFIG_KVM_USB_SCREENSHOT
#include "screenshot_session.inc"
#endif
#if KVM_HPE_FOUR
static void check_feature(uint8_t id, uint8_t value)
{
    const uint8_t data[] = {id, value};
    setup(0xa1, HID_REQ_CONTROL_GET_REPORT, 0x0300 | id, 2, 2);
    assert(endpoint[0x80].pending && endpoint[0x80].len == 2 && !memcmp(endpoint[0x80].data, data, 2));
    complete(0x80);
    status_stage(0);
}
static void feature(uint8_t id, uint8_t value)
{
    const uint8_t data[] = {id, value};
    set_report(2, 0x0300 | id, data, sizeof(data));
    check_feature(id, value);
}
static void certification(uint16_t requested)
{
    setup(0xa1, HID_REQ_CONTROL_GET_REPORT, 0x0300 | HPE_CERT, 2, requested);
    const unsigned expected = requested < sizeof(s_ptp_cert_report) ? requested : sizeof(s_ptp_cert_report);
    for (unsigned offset = 0; offset < expected;) {
        const unsigned remaining = expected - offset;
        unsigned chunk = remaining > 64 ? 64 : remaining;
        assert(endpoint[0x80].pending && endpoint[0x80].len == chunk);
        assert(!memcmp(endpoint[0x80].data, s_ptp_cert_report + offset, chunk));
        offset += chunk;
        complete(0x80);
    }
    status_stage(0);
}
#endif
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
static void check_service_window(void)
{
    usb_hid_service_window_t w;
    assert(!usb_hid_service_end(&w) && !usb_hid_service_begin(99) && !usb_hid_service_begin(120001));
    assert(usb_hid_service_begin(1000) && !usb_hid_service_begin(1000));
    now += 2000;
    usb_hid_device_request_pump();
    now += 350;
    usb_hid_device_request_pump(); // coalescing must preserve the first waiting timestamp
    now += 800;
    service();
    now += 21000;
    usb_hid_device_request_pump();
    now += 800;
    service();
    now += 200;
    usb_hid_device_request_pump();
    now += 500;
    assert(usb_hid_service_end(&w));
    assert(w.elapsed_us == 25650 && w.services == 2 && w.all_ready == 2 && w.timed_requests == 2);
    assert(w.max_gap_us == 21800 && w.gaps_over_20ms == 1 && w.max_defer_us == 1150 && w.pending_us == 500);
    assert(!w.expired && !w.sent && !w.complete && !w.refused && !w.failed && !w.link_changes);
    assert(!usb_hid_service_end(&w));
    service(); // finish the outstanding pump outside the window
    assert(usb_hid_service_begin(100));
    now += 100001; // no intervening USB service
    usb_hid_device_request_pump();
    service();
    assert(usb_hid_service_end(&w) && w.expired && !w.services && !w.timed_requests);
    assert(w.elapsed_us == 100000 && w.max_gap_us == 100000 && w.gaps_over_20ms == 1);
    puts("PASS: idle TinyUSB service window, first request to callback, ready endpoints, pending tail and expiry.");
}
#endif
int main(int argc, char **argv)
{
    assert(hid_dispatch_start() == ESP_OK && usb_hid_device_start() == ESP_OK);
    reset_bus();
    configure(1);
    drain();
    assert(usb().mounts == 1 && usb().usb_epoch == 1 && keyboard_neutrals == 1 && !keyboard_presses);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    check_service_window();
#endif
    const uint8_t led = 2;
    check_first_led(0, true);
    uint32_t led_reports = usb().led_reports;
    setup(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_BOOT, 1, 0);
    status_stage(0x80);
    assert(usb().led_state == led && usb().protocol[1] == HID_PROTOCOL_BOOT);
#if CONFIG_TINYUSB_HID_COUNT == 2
    assert(usb_hid_device_interface_count() == 2 && snapshot().ledger_empty);
    kvm_hid_intent_t media = {.type=KVM_HID_CONSUMER_DOWN};
    assert(hid_dispatch_local(&media) == ESP_ERR_NOT_SUPPORTED);
#elif KVM_HID_ITF_COUNT == 3
    kvm_hid_intent_t media = {.type=KVM_HID_CONSUMER_DOWN, .data.consumer.usage=KVM_CONSUMER_MUTE};
    assert(hid_dispatch_local(&media) == ESP_OK);
    drain();
    check_input_report(2, 2, (const uint8_t[]){KVM_CONSUMER_MUTE, 0}, 2);
    media.type = KVM_HID_CONSUMER_UP;
    assert(hid_dispatch_local(&media) == ESP_OK);
    drain();
    check_input_report(2, 2, (const uint8_t[2]){0}, 2);
#endif
    kvm_hid_intent_t button = {.type=KVM_HID_MOUSE_BTN_DOWN, .data.mouse_button.button_id=KVM_MOUSE_BTN_RIGHT};
    assert(hid_dispatch_local(&button) == ESP_OK);
    drain();
    const uint8_t boot_move[] = {2, 5, (uint8_t)-3};
    kvm_hid_intent_t move = {.type=KVM_HID_MOUSE_MOVE, .data.mouse_move={.dx=5, .dy=-3}};
    assert(hid_dispatch_local(&move) == ESP_OK);
    service();
    assert(endpoint[0x82].pending && endpoint[0x82].len == sizeof(boot_move));
    assert(!memcmp(endpoint[0x82].data, boot_move, sizeof(boot_move)));
    check_input_report(1, 8, (const uint8_t[]){2, 0, 0}, 3);
    complete(0x82);
    kvm_hid_intent_t wheel = {.type=KVM_HID_MOUSE_WHEEL, .data.wheel={.horizontal=3, .vertical=-2}};
    assert(hid_dispatch_local(&wheel) == ESP_OK);
    service();
    assert(!endpoint[0x82].pending);
    setup(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_BOOT, 0, 0);
    status_stage(0x80);
    setup(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_REPORT, 1, 0);
    status_stage(0x80);
    assert(usb().protocol[0] == HID_PROTOCOL_BOOT && usb().protocol[1] == HID_PROTOCOL_REPORT);
    assert(hid_dispatch_local(&wheel) == ESP_OK);
    service();
    const uint8_t wheel_report[] = {2, 0, 0, (uint8_t)-2, 3};
    assert(endpoint[0x82].pending && endpoint[0x82].len == sizeof(wheel_report));
    assert(!memcmp(endpoint[0x82].data, wheel_report, sizeof(wheel_report)));
    complete(0x82);
    setup(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_REPORT, 0, 0);
    status_stage(0x80);
    check_input_report(1, 8, (const uint8_t[]){2, 0, 0, 0, 0}, 5);
    button.type = KVM_HID_MOUSE_BTN_UP;
    assert(hid_dispatch_local(&button) == ESP_OK);
    drain();
    check_input_report(1, 5, (const uint8_t[5]){0}, 5);
    setup(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_BOOT, 1, 0);
    status_stage(0x80);
#if KVM_HPE_FOUR
    const uint16_t lengths[] = {1, 64, 65, 256, 257, 258};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) certification(lengths[i]);
    check_feature(HPE_CAPS, 0x25);
    feature(HPE_INPUT_MODE, 3);
    drain();
    kvm_hid_intent_t touch = {.type=KVM_HID_PTP_FRAME};
    touch.data.ptp.generation = snapshot().pad_generation;
    touch.data.ptp.frame.count = 1;
    touch.data.ptp.frame.contact[0] = (hpe_contact_t){.id=8, .tip=true, .confidence=true, .x=1234, .y=2345};
    assert(hid_dispatch_local(&touch) == ESP_OK);
    service();
    assert(endpoint[0x83].pending && endpoint[0x83].len == HPE_PTP_WIRE);
    assert(endpoint[0x83].data[1] == 3 && endpoint[0x83].data[2] == 8 && endpoint[0x83].data[33] == 1);
    drain();
    feature(HPE_SELECT, 2);
    assert(endpoint[0x83].pending && endpoint[0x83].data[1] == 2 && endpoint[0x83].data[2] == 8);
    assert(endpoint[0x83].data[33] == 1);
    feature(HPE_LATENCY, 1);
    assert(snapshot().pad_native_requested && !snapshot().pad_surface_enabled);
#endif
    drain();

    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    service();
    assert(usb().state == KVM_USB_SUSPENDED && snapshot().host_usb == KVM_HOST_USB_SUSPENDED);
    check_led(true, led, led_reports);
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
    service();
    drain();
    assert(usb().usb_epoch == 1 && usb().led_state == led && usb().protocol[1] == HID_PROTOCOL_BOOT);
    check_led(true, led, led_reports);
#if KVM_HPE_FOUR
    assert(usb().ptp.mode == 3 && usb().ptp.selective == 2 && usb().ptp.latency == 1);
    check_feature(HPE_INPUT_MODE, 3);
    check_feature(HPE_SELECT, 2);
    check_feature(HPE_LATENCY, 1);
#endif
    now += 10000000; // the local touch no longer holds the priority window
    uint32_t epoch;
    assert(hid_dispatch_remote_grant(42, &epoch) == KVM_HID_OK);
    drain();
    kvm_hid_intent_t down = {.type=KVM_HID_KEY_DOWN, .data.key.usage_id=4};
    assert(hid_dispatch_remote(42, epoch, &down) == ESP_OK);
    service(); // withhold the interrupt completion to queue another key behind it
    assert(keyboard_presses == 1 && snapshot().keys_down == 1);
    check_input_report(0, 3, (const uint8_t[]){0, 0, 4}, 3);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    usb_hid_service_window_t blocked;
    assert(usb_hid_service_begin(100));
    usb_hid_device_request_pump();
    service();
    assert(usb_hid_service_end(&blocked) && blocked.services == 1 && !blocked.all_ready);
    assert(!blocked.sent && !blocked.complete); // the pending transfer predates this sample
#endif
    down.data.key.usage_id = 5;
    assert(hid_dispatch_remote(42, epoch, &down) == ESP_OK);
    service();
    // The host resets first. A physical approval for the outgoing session
    // is already waiting for the USB task behind that bus event.
    memset(endpoint, 0, sizeof(endpoint));
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
    uint32_t orphan_grant = 0;
    kvm_hid_err_t grant_result = hid_dispatch_remote_grant(42, &orphan_grant);
    assert(!tud_mounted() && usb().state == KVM_USB_ENUMERATING && !usb().umounts);
    assert(grant_result == KVM_HID_ERR_CONTROL_DENIED && !orphan_grant);
    assert(snapshot().ledger_empty && snapshot().owner_kind == OWNER_NONE && snapshot().owner_epoch != epoch);
    check_led(false, 0, led_reports);
#if KVM_HPE_FOUR
    assert(!usb().ptp.mode && usb().ptp.selective == 3 && !usb().ptp.latency);
    assert(!snapshot().pad_native_requested && snapshot().pad_surface_enabled);
#endif
    unsigned before = keyboard_neutrals;
    configure(1);
    drain();
    assert(usb().usb_epoch == 2 && keyboard_neutrals == before + 1 && keyboard_presses == 1);
    check_input_report(0, 8, (const uint8_t[8]){0}, 8);
    puts("PASS GET_REPORT(Input): held key, short read, neutral after remount; "
         "held/released mouse button without repeated motion in boot and report protocols.");
    check_first_led(0, false);
    assert(hid_dispatch_remote(42, epoch, &down) == ESP_OK);
    drain();
    assert(keyboard_presses == 1 && snapshot().ledger_empty && usb().protocol[1] == HID_PROTOCOL_REPORT);
#if KVM_HPE_FOUR
    assert(usb().ptp_generation == snapshot().pad_generation);
    check_feature(HPE_INPUT_MODE, 0);
    check_feature(HPE_SELECT, 3);
    check_feature(HPE_LATENCY, 0);
    puts("PTP PASS: host EP0 0x11..0x15, 257B in packets of 64/64/64/64/1, short reads, selective surface "
         "liftoff, settings retained on resume and defaults after reset.");
#endif
    assert(hid_dispatch_local(&down) == ESP_OK);
    drain();
    assert(keyboard_presses == 2);
    set_report(0, 0x0200, &led, 1);
    led_reports = usb().led_reports;
    configure(0);
    assert(usb().state == KVM_USB_DETACHED && usb().umounts == 1 && snapshot().ledger_empty);
    check_led(false, 0, led_reports);
    configure(1);
    drain();
    check_first_led(0, false);
    memset(endpoint, 0, sizeof(endpoint));
    check_keyboard_idle(0, 0x81);
    set_keyboard_idle(0, 2);
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
    service();
    frames(40);
    assert(!sof_enabled && !endpoint[0x81].pending);
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
    service();
    drain();
    frames(7);
    assert(sof_enabled && !endpoint[0x81].pending);
    frames(1);
    assert(endpoint[0x81].pending && !memcmp(endpoint[0x81].data, (const uint8_t[8]){0}, 8));
    complete(0x81);
    set_keyboard_idle(0, 255);
    dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
    service();
    assert(!sof_enabled);
    assert(usb().state == KVM_USB_DETACHED && usb().umounts == 2 && !tud_mounted());
    check_led(false, 0, led_reports+1);
    puts("PASS HID LED On -> suspend/resume On -> detach/remount Unknown -> acknowledged zero Off; "
         "wrong ID, type, interface, length and short DATA ignored; lifetime counter preserved.");
    printf("%u IF PASS: bus reset with a held key and a queued key. Old lease revoked, pending grant refused, "
           "neutral remount, fresh input. EP0 LED/protocol, suspend/resume, deconfigure and unplug passed.\n",
           KVM_HID_ITF_COUNT);
#if CONFIG_KVM_USB_MONITOR_EVAL
    monitor_session();
#endif
#if CONFIG_KVM_USB_GUD
    gud_session();
#endif
#if CONFIG_KVM_USB_SCREENSHOT
    assert(argc == 2);
    screenshot_session(argv[1]);
#endif
    return 0;
}
