// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "hid_dispatch.h"
#include "hpe_access_config.h"
#include "hpe_panel.h"
#include "sim_platform.h"
#include "usb_hid_reports.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static kvm_hid_core_t core;
static bool in_flight[KVM_HID_ITF_COUNT];
static unsigned keys, mouse_buttons;
#if KVM_HID_HAS_HPE
static unsigned media, requests, transmissions;
static unsigned radial_moves, radial_down, radial_up;
static int radial_sum;
static bool dial_pressed;
static uint32_t request_id;
static bool consumer_neutral;
#endif
static uint8_t last_key, last_mods;
static bool keyboard_neutral;
static lv_obj_t *base;

static uint64_t now_us(void) { return (uint64_t)lv_tick_get() * 1000; }

// Memory-only interrupt IN endpoints. Complete on the next scheduler tick.
static bool send_report(void *ctx, uint8_t itf, const uint8_t *p, uint8_t n)
{
    (void)ctx;
    assert(itf < KVM_HID_ITF_COUNT && !in_flight[itf]);
    in_flight[itf] = true;
    if (itf == KVM_HID_ITF_KEYBOARD) {
        assert(n == 8);
        keyboard_neutral = !memcmp(p, (uint8_t[8]){0}, 8);
        if (p[2]) { keys++; last_key = p[2]; last_mods = p[0]; }
    } else if (itf == KVM_HID_ITF_MOUSE) {
        if (p[0]) mouse_buttons++;
#if KVM_HID_HAS_HPE
    } else if (itf == KVM_HID_ITF_CONSUMER && p[0] == HPE_RADIAL) {
        assert(n == 3);
        int payload = p[1] | (p[2] << 8);
        int delta = payload >> 1;
        if (delta & 0x4000) delta -= 0x8000;
        assert(delta >= -3600 && delta <= 3600);
        if (delta) { radial_moves++; radial_sum += delta; }
        bool pressed = (payload & 1) != 0;
        if (pressed && !dial_pressed) radial_down++;
        if (!pressed && dial_pressed) radial_up++;
        dial_pressed = pressed;
    } else if (itf == KVM_HID_ITF_CONSUMER && p[0] == HPE_CONSUMER) {
        assert(n == 3);
        consumer_neutral = !(p[1] | p[2]);
        if (!consumer_neutral) { assert(p[1] == 0xcd && !p[2]); media++; }
    } else if (itf == KVM_HID_ITF_CONSUMER && p[0] == HPE_STUDIO_IN) {
        hpe_packet_t wire;
        assert(hpe_decode_packet(p, n, HPE_STUDIO_IN, &wire));
        if (wire.opcode == 0x10) {
            assert(wire.payload[0] == 1 && wire.payload[1] == 1 && wire.payload[2] == 1);
            uint32_t id = wire.payload[4] | (uint32_t)wire.payload[5] << 8 |
                          (uint32_t)wire.payload[6] << 16 | (uint32_t)wire.payload[7] << 24;
            assert(id);
            if (id != request_id) { requests++; request_id = id; }
            transmissions++;
        }
#else
    } else {
        assert(KVM_HID_ITF_COUNT == 3 && itf == KVM_HID_ITF_CONSUMER && n == 2 && !p[0] && !p[1]);
#endif
    }
    return true;
}

static void pump(lv_timer_t *unused)
{
    (void)unused;
    for (uint8_t i = 0; i < KVM_HID_ITF_COUNT; i++) if (in_flight[i]) {
        in_flight[i] = false;
        kvm_hid_core_on_report_complete(&core, i);
    }
    kvm_hid_core_poll(&core, now_us());
}

#if KVM_HID_HAS_HPE
void hid_dispatch_set_profile(uint8_t profile) { core.active_profile = profile; }
void hid_dispatch_studio_status(hpe_studio_status_t *out) { *out = core.studio.status; }
#endif
void hid_dispatch_get_snapshot(kvm_hid_snapshot_t *s, uint32_t *seq)
{
    kvm_hid_core_snapshot(&core, s);
    if (seq) *seq = 0;
}
void hid_dispatch_release_all(kvm_release_reason_t reason) { kvm_hid_core_release_all(&core, reason); }
esp_err_t hid_dispatch_local(const kvm_hid_intent_t *i) { return kvm_hid_core_local(&core, i, now_us()); }
esp_err_t hid_dispatch_burst_start(const kvm_hid_burst_step_t *v, uint16_t n,
                                   const kvm_hid_burst_step_t *cancel, uint8_t nc)
{
    return kvm_hid_core_burst_start(&core, v, n, cancel, nc, now_us());
}
static hpe_access_timing_t saved = {.scan_ms=600, .dwell_ms=600, .debounce_ms=40, .cooldown_ms=1000};
bool app_config_access_read(hpe_access_timing_t *t)
{
    *t = saved;
    return true;
}
bool app_config_access_save(hpe_access_timing_t t) { assert(hpe_access_valid(t)); saved=t; return true; }

static void wait_ms(unsigned ms) { lv_test_wait(ms); }
static void tap(int x, int y) { sim_press_point(x, y); sim_release(); }
static void home(void) { lv_screen_load(base); }
static bool shows(lv_obj_t *o, const char *s)
{
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), s)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(o); i++)
        if (shows(lv_obj_get_child(o, i), s)) return true;
    return false;
}
static void usb(kvm_host_usb_t state, uint32_t epoch)
{
    kvm_hid_core_set_usb(&core, state, epoch, now_us());
    wait_ms(80);
}

#if KVM_HID_HAS_HPE
static void move(int x, int y)
{
    lv_test_mouse_move_to(x, y);
    wait_ms(60);
}

static void dial_trajectory(void)
{
    assert(shows(lv_screen_active(), "Host Radial support unverified"));
    lv_point_t center = {512,290};
    lv_obj_t *dial = lv_indev_search_obj(lv_screen_active(), &center);
    assert(dial && lv_obj_check_type(dial, &lv_button_class));
    lv_area_t box;
    lv_obj_get_coords(dial, &box);
    int x=(box.x1+box.x2)/2, y=(box.y1+box.y2)/2;
    sim_press_point(x+160,y);
    move(x,y+160);
    int quarter=radial_sum;
    assert(quarter >= 899 && quarter <= 901 && radial_moves==1);
    move(x+160,y);
    assert(radial_sum >= -1 && radial_sum <= 1 && radial_moves==2);
    sim_release();

    sim_press_point(x-160,y+3); // Clockwise across atan2's +pi/-pi discontinuity.
    int before=radial_sum;
    move(x-160,y-3);
    int wrapped=radial_sum-before;
    assert(wrapped >= 20 && wrapped <= 23);
    move(x-160,y+3);
    assert(radial_sum-before >= -1 && radial_sum-before <= 1);
    sim_release();

    sim_press_point(x+160,y-3); // 359 degrees to 1 degree.
    before=radial_sum;
    move(x+160,y+3);
    assert(radial_sum-before >= 20 && radial_sum-before <= 23);
    unsigned count=radial_moves;
    move(x,y); move(x-160,y);
    assert(radial_moves==count && !dial_pressed);
    move(x-160,y-4);
    assert(radial_moves==count+1);
    count=radial_moves;
    move(x-200,y-170); move(x+160,y);
    assert(radial_moves==count && !dial_pressed);
    sim_release();
    tap(125,270);
    sim_press_point(x+160,y); before=radial_sum;
    move(x,y+160);
    int fine=radial_sum-before;
    assert(fine >= 224 && fine <= 226);
    sim_release();
    tap(125,270);

    count=radial_moves;
    sim_press_point(x,y);
    move(x+160,y); move(x,y+160); wait_ms(800);
    assert(dial_pressed && radial_down==1 && radial_moves==count);
    sim_release();
    assert(!dial_pressed && radial_up==1 && !mouse_buttons);
    sim_press_point(x,y);
    lv_obj_send_event(dial,LV_EVENT_PRESS_LOST,NULL);
    wait_ms(50); sim_release();
    assert(!dial_pressed && radial_down==2 && radial_up==2);
    sim_press_point(x,y);
    usb(KVM_HOST_USB_SUSPENDED,1);
    usb(KVM_HOST_USB_MOUNTED,1); // Resume the same enumeration.
    assert(!dial_pressed && radial_up==3);
    move(x+160,y); sim_release();
    assert(radial_moves==count && radial_down==3);
    sim_press_point(x,y);
    assert(dial_pressed);
    lv_screen_load(base); // The normal screen-unloaded callback releases all input.
    wait_ms(80);
    assert(!dial_pressed && radial_up==4 && core.active_profile==HPE_MOUSE_PROFILE);
    sim_release();
    assert(hpe_panel_open(home)); wait_ms(100);
    printf("PASS radial trajectory: quarter=%d, fine=%d, signed wrap=%d tenths, "
           "ring gaps=0 extra moves, center holds=%u/%u, mouse buttons=%u\n",
           quarter,fine,wrapped,radial_down,radial_up,mouse_buttons);
}

static void bank(uint8_t peer)
{
    hpe_studio_start(&core.studio, 5);
    hpe_packet_t hi = {.opcode=2, .sequence=1, .length=12, .count=1, .payload={1,0,HPE_AGENT_CAPS,0,5}};
    hi.payload[8] = peer;
    hpe_packet_t caps = {.opcode=0x14, .sequence=2, .length=3, .count=1, .payload={1,1,1}};
    hpe_packet_t b = {.opcode=0x15, .sequence=3, .length=11, .count=1, .payload={1,0,1,1,0,1,1,3,'O','B','S'}};
    assert(hpe_studio_receive(&core.studio, &hi, now_us()));
    assert(hpe_studio_receive(&core.studio, &caps, now_us()));
    assert(hpe_studio_receive(&core.studio, &b, now_us()));
    wait_ms(80);
}

#endif

int main(void)
{
    size_t baseline = sim_env_start();
    base = lv_screen_active();
    kvm_hid_port_t port = {.send=send_report};
    kvm_hid_core_init(&core, &port, NULL);
    lv_timer_t *scheduler = lv_timer_create(pump, 2, NULL);
    usb(KVM_HOST_USB_MOUNTED, 1);
    assert(hpe_panel_open(home));
    wait_ms(100);
#if KVM_HID_HAS_HPE
    dial_trajectory();
    tap(630,568); // Studio Basic
    kvm_hid_intent_t late={.type=KVM_HID_RADIAL,.data.radial={.pressed=true,.delta=120}};
    assert(kvm_hid_core_local(&core,&late,now_us())==KVM_HID_ERR_CONTROL_DENIED);
    wait_ms(50);
    assert(radial_down==4 && !dial_pressed);
    usb(KVM_HOST_USB_SUSPENDED, 1);
    assert(shows(lv_screen_active(), "Host asleep   |   No host agent   |   Basic controls"));
    sim_press_point(140,120);
    usb(KVM_HOST_USB_MOUNTED, 2);
    sim_release();
    assert(!media);
    tap(140,120);
    wait_ms(60);
    assert(media == 1 && consumer_neutral);
    bank(9);
    assert(shows(lv_screen_active(), "USB connected   |   Studio agent connected   |   Basic controls"));
    wait_ms(6100); // No PONG from this synthetic agent.
    assert(shows(lv_screen_active(), "USB connected   |   No host agent   |   Basic controls"));
    tap(140,120);
    assert(media == 2 && consumer_neutral);
    bank(9);
    tap(900,20); // OBS bank
    sim_press_point(120,240);
    bank(10); // A new agent session while the same tile is held.
    sim_release();
    assert(!requests);
    tap(120,240);
    assert(requests == 1 && core.studio.status.peer_nonce == 10);
    tap(885,568); // Access
    wait_ms(100);
#else
    assert(shows(lv_screen_active(), "Direct input"));
    assert(!shows(lv_screen_active(), "Dial") && !shows(lv_screen_active(), "Studio Basic"));
    assert(!shows(lv_screen_active(), "OBS bank"));
#endif
    usb(KVM_HOST_USB_DETACHED, 2);
    sim_press_point(90,130);
    usb(KVM_HOST_USB_MOUNTED, 3);
    sim_release();
    assert(!keys);
    tap(90,130);
    assert(keys == 1 && last_key == KVM_KEY_TAB && keyboard_neutral);
    tap(520,415);
    tap(90,130);
    assert(keys == 2 && last_key == KVM_KEY_TAB && last_mods == KVM_MOD_BIT(KVM_KEY_LEFT_CTRL) &&
           keyboard_neutral);
    tap(520,415); // Arm one-shot Ctrl locally.
    usb(KVM_HOST_USB_DETACHED, 3);
    usb(KVM_HOST_USB_MOUNTED, 4);
    tap(90,130);
    assert(keys == 3 && last_mods == 0 && keyboard_neutral);

    tap(365,415); // Dwell on.
    sim_press_point(90,130);
    wait_ms(620);
    sim_release();
    assert(keys == 4);
    sim_press_point(475,130); // Press during cooldown, hold well beyond it.
    wait_ms(1300);
    sim_release();
    assert(keys == 4);
    sim_press_point(475,130);
    wait_ms(5000);
    lv_test_mouse_move_to(90,130);
    wait_ms(1200); // Dwell cannot rearm on a different key without lifting.
    sim_release();
    assert(keys == 5 && last_key == KVM_KEY_ENTER && keyboard_neutral);
    sim_press_point(90,130);
    lv_test_mouse_move_to(740,310);
    wait_ms(1400);
    sim_release();
    assert(keys == 5);

    tap(80,415); // Scan at 600ms, cooldown 1000ms.
    tap(900,480); // Far edge of the enlarged on-screen switch.
    assert(keys == 6 && last_key == KVM_KEY_TAB);
    wait_ms(740);
    assert(shows(lv_screen_active(), "Scanning 1 of 6: Tab"));
    sim_press_point(900,480); // A press begun during cooldown cannot send, but can Stop.
    wait_ms(2100);
    sim_release();
    wait_ms(1200);
    assert(keys == 6 && shows(lv_screen_active(), "Scan paused"));
    tap(80,415);
    tap(900,480);
    wait_ms(1150);
    assert(keys == 7 && shows(lv_screen_active(), "Scanning 1 of 6: Tab"));
    wait_ms(620);
    assert(shows(lv_screen_active(), "Scanning 2 of 6: Enter"));
    tap(900,480);
    assert(keys == 8 && last_key == KVM_KEY_ENTER);
    assert(keyboard_neutral && !mouse_buttons);
#if KVM_HID_HAS_HPE
    assert(consumer_neutral && requests == 1 && transmissions >= requests);
#endif
    tap(900,20); // Access timing
    wait_ms(80);
    assert(shows(lv_screen_active(), "Dwell time"));
    tap(800,180); // Increase dwell time by 100 ms
    tap(820,460); // Apply
    assert(saved.dwell_ms == 700 && shows(lv_screen_active(), "Timings applied"));
    assert(keys == 8);
    tap(108,568);
    wait_ms(100);
    assert(lv_screen_active() == base && hpe_panel_open(home));
    wait_ms(100);
#if KVM_HID_HAS_HPE
    tap(885,568);
#endif
    tap(900,20);
    assert(shows(lv_screen_active(), "700 ms"));
    tap(108,568);
    wait_ms(100);
    lv_timer_delete(scheduler);
    assert(lv_screen_active() == base);
    kvm_hid_snapshot_t final;
    kvm_hid_core_snapshot(&core,&final);
    assert(final.ledger_empty && !final.neutralizing && !core.q_count);
    sim_env_stop(baseline);
    printf("PASS HID count %u: eight keyboard taps, one-shot Ctrl, stale press suppressed, dwell, scan, "
           "cooldown, Stop, saved timing on reopen and neutral Home\n", (unsigned)KVM_HID_ITF_COUNT);
    return 0;
}
