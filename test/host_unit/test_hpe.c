// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include <string.h>
#include "hid_core.h"
#include "hpe_reports.h"
#include "kvm_test.h"
#include "ptp_contacts.h"

static kvm_hid_core_t core;
static uint8_t captured[256][64], ids[256];
static unsigned reports;
static bool busy;
static uint64_t now;

static bool send(void *ctx, uint8_t itf, const uint8_t *p, uint8_t n)
{
    (void)ctx;
    if (busy) return false;
    CHECK(reports < 256 && n <= 64);
    if (reports >= 256 || n > 64) return false;
    ids[reports] = itf;
    memcpy(captured[reports++], p, n);
    return true;
}

static void drain(void)
{
    for (unsigned i = 0; i < 12; i++) {
        kvm_hid_core_poll(&core, ++now);
        for (unsigned itf = 0; itf < 4; itf++) kvm_hid_core_on_report_complete(&core, (uint8_t)itf);
    }
}

static void vectors(void)
{
    uint8_t wire[64];
    hpe_frame_t frame = {.scan_time = 100, .count = 1, .contact = {{.id = 1, .tip = true, .confidence = true,
                                                                   .x = 16384, .y = 8192}}};
    CHECK_EQ(hpe_encode_ptp(wire, &frame), 34);
    CHECK(!memcmp(wire, (uint8_t[]){0x10,3,1,0,64,0,32}, 7));
    CHECK_EQ(wire[31], 100);
    CHECK_EQ(wire[33], 1);
    for (unsigned i = 7; i < 31; i++) CHECK_EQ(wire[i], 0);
    frame.count = 2;
    frame.contact[1] = frame.contact[0];
    CHECK_EQ(hpe_encode_ptp(wire, &frame), 0);
    CHECK_EQ(hpe_encode_radial(wire, true, -10), 3);
    CHECK(!memcmp(wire, (uint8_t[]){0x20,0xed,0xff}, 3));
    CHECK_EQ(hpe_encode_radial(wire, false, 3601), 0);
    hpe_joystick_t joy = {.x = -32767, .y = 32767, .rz = -1, .z = 65535, .buttons = 0x8001, .hat = 8};
    CHECK_EQ(hpe_encode_joystick(wire, &joy), 12);
    CHECK(!memcmp(wire, (uint8_t[]){0x30,1,128,255,127,255,255,255,255,1,128,8}, 12));
    joy.hat = 9;
    CHECK(!hpe_joystick_valid(&joy));

    hpe_packet_t ping = {.opcode = 3, .sequence = 1, .count = 1, .length = 4,
                         .payload = {0x78,0x56,0x34,0x12}}, result;
    CHECK(hpe_encode_packet(wire, HPE_STUDIO_IN, &ping));
    CHECK_EQ(wire[62], 0xb1);
    CHECK_EQ(wire[63], 0xe9);
    CHECK(hpe_decode_packet(wire, 64, HPE_STUDIO_IN, &result));
    CHECK(!memcmp(result.payload, ping.payload, 4));
    wire[11] ^= 1;
    CHECK(!hpe_decode_packet(wire, 64, HPE_STUDIO_IN, &result));
    CHECK(hpe_encode_packet(wire, HPE_STUDIO_OUT, &ping));
    CHECK(hpe_decode_output(0, wire, 64, &result));
    CHECK(hpe_decode_output(HPE_STUDIO_OUT, wire + 1, 63, &result));
    CHECK(!hpe_decode_output(HPE_STUDIO_OUT, wire, 64, &result));
    CHECK(!hpe_decode_output(HPE_STUDIO_IN, wire + 1, 63, &result));
    CHECK(!hpe_decode_output(0, wire + 1, 63, &result));
    ping.count = 9;
    CHECK(!hpe_encode_packet(wire, HPE_STUDIO_IN, &ping));
    ping.count = 1;
    ping.length = 52;
    CHECK(!hpe_encode_packet(wire, HPE_STUDIO_IN, &ping));

    hpe_features_t features = HPE_FEATURE_DEFAULTS;
    CHECK_EQ(hpe_feature_get(&features, HPE_CAPS, wire, sizeof(wire)), 1);
    CHECK_EQ(wire[0], 0x25);
    CHECK_EQ(features.mode, 0);
    CHECK_EQ(features.selective, 3);
    CHECK(hpe_feature_set(&features, HPE_INPUT_MODE, (uint8_t[]){3}, 1));
    CHECK_EQ(features.mode, 3);
    CHECK(hpe_feature_set(&features, HPE_INPUT_MODE, (uint8_t[]){7}, 1));
    CHECK_EQ(features.mode, 0);
    CHECK(!hpe_feature_set(&features, HPE_SELECT, (uint8_t[]){4}, 1));
    CHECK(!hpe_feature_set(&features, HPE_LATENCY, wire, 2));
}

static void studio_link(void)
{
    hpe_studio_t link;
    hpe_packet_t out;
    hpe_studio_start(&link, 0x12345678);
    CHECK(hpe_studio_next(&link, 1, &out));
    CHECK_EQ(out.opcode, 1);
    CHECK(!memcmp(out.payload + 4, (uint8_t[]){0x78,0x56,0x34,0x12}, 4));
    hpe_packet_t ack = {.opcode=2, .sequence=65535, .count=2, .length=8, .payload={1,0,6,0,0x78,0x56,0x34,0x12}};
    CHECK(hpe_studio_receive(&link, &ack, 10));
    CHECK(!link.ready);
    ack.index = 1;
    ack.length = 4;
    ack.payload[0] = 4;
    CHECK(!hpe_studio_receive(&link, &ack, 300011));
    ack.index = 0;
    ack.length = 8;
    ack.payload[0] = 1;
    CHECK(hpe_studio_receive(&link, &ack, 300012));
    ack.index = 1;
    ack.length = 4;
    CHECK(hpe_studio_receive(&link, &ack, 300014));
    CHECK(link.ready);
    CHECK(hpe_studio_next(&link, 300015, &out));
    CHECK_EQ(out.opcode, 3);
    out.opcode = 4;
    out.sequence = 0;
    CHECK(hpe_studio_receive(&link, &out, 300016));
    CHECK(!hpe_studio_receive(&link, &out, 300017));
    CHECK(hpe_studio_next(&link, 6300017, &out));
    CHECK_EQ(out.opcode, 1);
    CHECK(!link.ready);
}

static void studio_bank(hpe_studio_t *link)
{
    hpe_studio_start(link, 5);
    hpe_packet_t hi={.opcode=2,.sequence=1,.length=12,.count=1,.payload={1,0,HPE_AGENT_CAPS,0,5,0,0,0,9}};
    CHECK(hpe_studio_receive(link,&hi,1));
    hpe_packet_t caps={.opcode=0x14,.sequence=2,.length=3,.count=1,.payload={1,1,1}};
    CHECK(hpe_studio_receive(link,&caps,2));
    hpe_packet_t bank={.opcode=0x15,.sequence=3,.length=11,.count=1,.payload={1,0,1,1,0,1,1,3,'O','B','S'}};
    CHECK(hpe_studio_receive(link,&bank,3));
    CHECK(!strcmp(link->status.label[0], "OBS"));
    bank.sequence++;
    bank.payload[8]=0xc0;
    CHECK(!hpe_studio_receive(link,&bank,4));
    CHECK(!strcmp(link->status.label[0],"OBS"));
}

static void studio_actions(void)
{
    hpe_studio_t link;
    studio_bank(&link);
    hpe_packet_t out;
    CHECK(hpe_studio_next(&link,5,&out));
    CHECK_EQ(out.opcode,3);
    CHECK(!hpe_studio_action(&link,1,0x101,8,10));
    CHECK(!hpe_studio_action(&link,1,0x102,9,10));
    CHECK(!hpe_studio_action(&link,2,0x102,9,10));
    CHECK(hpe_studio_action(&link,1,0x101,9,10));
    CHECK(!hpe_studio_action(&link,1,0x101,9,11));
    CHECK(hpe_studio_next(&link,11,&out));
    CHECK_EQ(out.opcode,0x10);
    CHECK_EQ(out.flags,1);
    CHECK(!memcmp(out.payload,(uint8_t[]){1,1,1,0,1,0,0,0},8));
    hpe_packet_t first=out;
    CHECK(!hpe_studio_next(&link,99999,&out));
    CHECK(hpe_studio_next(&link,100010,&out));
    CHECK_EQ(out.sequence,first.sequence);
    CHECK(!memcmp(&first,&out,sizeof(out)));
    CHECK(hpe_studio_next(&link,200010,&out));
    CHECK(!memcmp(&first,&out,sizeof(out)));
    CHECK(!hpe_studio_next(&link,300010,&out));
    hpe_packet_t ack={.count=1,.sequence=4,.opcode=0x21,.length=2,.ack=first.sequence};
    ack.payload[0]=(uint8_t)ack.ack;
    CHECK(hpe_studio_receive(&link,&ack,300012));
    CHECK_EQ(link.status.phase,HPE_DELIVERED);
    hpe_packet_t accepted={.count=1,.sequence=5,.opcode=0x11,.length=5,.payload={9,0,0,0,1}};
    CHECK(!hpe_studio_receive(&link,&accepted,300013));
    accepted.payload[0]=1;
    CHECK(hpe_studio_receive(&link,&accepted,300014));
    CHECK_EQ(link.status.phase,HPE_ACCEPTED);
    hpe_packet_t result={.count=1,.sequence=6,.opcode=0x12,.flags=1,.length=10,.payload={1,0,0,0,6,0,13}};
    CHECK(hpe_studio_receive(&link,&result,300020));
    CHECK_EQ(link.status.phase,HPE_FINISHED);
    CHECK_EQ(link.status.result,6);
    CHECK_EQ(link.status.result_version,13);
    CHECK_EQ(link.status.state[0],0);
    CHECK(hpe_studio_next(&link,300021,&out));
    CHECK_EQ(out.opcode,0x21);
    CHECK_EQ(out.ack,6);
    CHECK(hpe_studio_receive(&link,&result,300022));
    CHECK(hpe_studio_next(&link,300023,&out));
    CHECK_EQ(out.ack,6);
    hpe_packet_t state={.count=1,.sequence=7,.opcode=0x13,.length=9,.payload={1,0,1,0,2,13}};
    CHECK(hpe_studio_receive(&link,&state,300024));
    CHECK_EQ(link.status.state[0],2);
    state.sequence++;
    CHECK(!hpe_studio_receive(&link,&state,300025));
    CHECK(hpe_studio_action(&link,1,0x101,9,400000));
    CHECK(hpe_studio_next(&link,400001,&out));
    CHECK_EQ(out.payload[4],2);
    hpe_studio_tick(&link,1000001);
    CHECK_EQ(link.status.result,3);
    CHECK(hpe_studio_action(&link,1,0x101,9,1000002));
    CHECK(hpe_studio_next(&link,1000003,&out));
    accepted.sequence=9;
    accepted.payload[0]=3;
    CHECK(hpe_studio_receive(&link,&accepted,1000004));
    hpe_studio_tick(&link,3000003);
    CHECK_EQ(link.status.result,3);
    state.index=1;
    state.count=2;
    state.sequence=10;
    CHECK(!hpe_studio_receive(&link,&state,3000004));
    CHECK(hpe_studio_next(&link,3000005,&out));
    CHECK_EQ(out.opcode,0x20);
    CHECK_EQ(out.payload[2],10);
    hpe_studio_tick(&link,6000001);
    CHECK(!link.ready && !link.status.bank && !link.status.peer_nonce);
    CHECK(link.device_nonce!=5);
}

static void contacts(void)
{
    touch_rect_t rect = {100,40,900,532};
    kvm_touch_sample_t sample = {.valid = true, .point_count = 5, .timestamp_us = 6553500};
    for (uint8_t i = 0; i < 5; i++) sample.point[i] = (kvm_touch_point_t){.track_id=i,.x_px=900,.y_px=40};
    hpe_frame_t active = {0}, frames[2];
    CHECK_EQ(ptp_contacts_update(&active, &rect, &sample, frames), 1);
    CHECK_EQ(frames[0].scan_time, 65535);
    CHECK_EQ(frames[0].contact[0].x, 32767);
    CHECK_EQ(frames[0].contact[0].y, 0);
    sample.timestamp_us += 100;
    for (uint8_t i = 0; i < 5; i++) sample.point[i].track_id = (uint8_t)(9 - i);
    CHECK_EQ(ptp_contacts_update(&active, &rect, &sample, frames), 2);
    CHECK_EQ(frames[0].scan_time, 0);
    for (uint8_t i = 0; i < 5; i++) {
        CHECK(!frames[0].contact[i].tip);
        CHECK_EQ(frames[0].contact[i].id, i);
        CHECK_EQ(frames[1].contact[i].id, 9 - i);
        CHECK(frames[1].contact[i].tip);
    }
    sample.point[1].track_id = sample.point[0].track_id;
    CHECK_EQ(ptp_contacts_update(&active, &rect, &sample, frames), -1);
}

static void scheduler(void)
{
    const kvm_hid_port_t port = {.send = send};
    kvm_hid_core_init(&core, &port, NULL);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 1, now);
    drain();
    CHECK_EQ(reports, 6);
    CHECK_EQ(captured[4][0], HPE_RADIAL);
    CHECK_EQ(captured[5][11], 8);
    reports = 0;
    kvm_hid_intent_t dial = {.type = KVM_HID_RADIAL, .data.radial = {.pressed = true, .delta = -7201}};
    CHECK_EQ(kvm_hid_core_local(&core, &dial, ++now), KVM_HID_ERR_CONTROL_DENIED);
    core.active_profile=HPE_DIAL_PROFILE;
    CHECK_EQ(kvm_hid_core_local(&core, &dial, ++now), KVM_HID_OK);
    drain();
    CHECK_EQ(reports, 3);
    CHECK_EQ(captured[0][1], 0xe1);
    CHECK_EQ(captured[0][2], 0xe3);
    CHECK_EQ(captured[2][1], 0xff);
    CHECK_EQ(captured[2][2], 0xff);
    core.active_profile=HPE_MOUSE_PROFILE;
    kvm_hid_core_ptp_route(&core, true, true, 17);
    drain();
    CHECK(!core.radial_pressed);
    kvm_hid_intent_t touch = {.type = KVM_HID_PTP_FRAME};
    touch.data.ptp.generation = 17;
    touch.data.ptp.frame = (hpe_frame_t){.count = 1, .contact = {{.id = 77, .x = 900, .tip = true}}};
    CHECK_EQ(kvm_hid_core_local(&core, &touch, ++now), KVM_HID_OK);
    drain();
    CHECK_EQ(core.ptp_active.count, 1);
    kvm_hid_intent_t mouse = {.type = KVM_HID_MOUSE_MOVE, .pad_input = true, .pad_generation = 17};
    CHECK_EQ(kvm_hid_core_local(&core, &mouse, ++now), KVM_HID_ERR_STALE_EPOCH);
    busy = true;
    reports = 0;
    kvm_hid_core_ptp_route(&core, false, true, 18);
    drain();
    CHECK_EQ(reports, 0);
    busy = false;
    drain();
    CHECK_EQ(reports, 6);
    CHECK_EQ(ids[2], 2);
    CHECK_EQ(captured[2][0], HPE_PTP);
    CHECK_EQ(captured[2][1] & 1, 0);
    CHECK_EQ(captured[2][2], 77);
    CHECK_EQ(captured[2][3], 0x84);
    CHECK_EQ(captured[2][33], 1);
    CHECK_EQ(kvm_hid_core_local(&core, &touch, ++now), KVM_HID_ERR_CONTROL_DENIED);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, 1, ++now);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 2, ++now);
    reports = 0;
    drain();
    CHECK_EQ(captured[2][33], 0);
    studio_bank(&core.studio);
    kvm_hid_intent_t action={.type=KVM_HID_STUDIO_ACTION,.data.studio={1,0x101,9}};
    CHECK_EQ(kvm_hid_core_local(&core,&action,++now),KVM_HID_ERR_CONTROL_DENIED);
    core.active_profile=HPE_STUDIO_PROFILE;
    CHECK_EQ(kvm_hid_core_local(&core,&action,++now),KVM_HID_OK);
    busy=true;
    drain();
    CHECK_EQ(core.studio.status.phase,HPE_REQUESTED);
    kvm_hid_core_release_all(&core,KVM_RA_VIEW_CHANGE);
    CHECK_EQ(core.studio.status.phase,HPE_IDLE);
    busy=false;
    reports=0;
    drain();
    for (unsigned i=0;i<reports;i++) CHECK(captured[i][0]!=HPE_STUDIO_IN || captured[i][2]!=0x10);
    CHECK_EQ(kvm_hid_core_local(&core,&action,++now),KVM_HID_OK);
    busy=true;
    drain();
    now+=600001;
    busy=false;
    reports=0;
    drain();
    CHECK_EQ(core.studio.status.phase,HPE_FINISHED);
    for (unsigned i=0;i<reports;i++) CHECK(captured[i][0]!=HPE_STUDIO_IN || captured[i][2]!=0x10);
}

int main(void)
{
    RUN(vectors);
    RUN(contacts);
    RUN(studio_link);
    RUN(studio_actions);
    RUN(scheduler);
    TEST_MAIN_END("test_hpe");
}
