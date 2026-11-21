// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

// Private operations of the single scheduler, compiled only in the four-interface build.
static void hpe_track_delivery(kvm_hid_core_t *core, const uint8_t *wire)
{
    core->ptp_active.count = 0;
    core->ptp_lift.count = 0;
    core->ptp_inflight_tips = 0;
    for (unsigned i = 0; i < wire[33]; i++) {
        const uint8_t *slot = wire + 1 + i * 6;
        hpe_contact_t c = {.confidence = (slot[0] & 2) != 0, .id = slot[1],
                           .x = (uint16_t)(slot[2] | slot[3] << 8), .y = (uint16_t)(slot[4] | slot[5] << 8)};
        core->ptp_lift.contact[core->ptp_lift.count++] = c;
        if (slot[0] & 1) {
            core->ptp_inflight_tips |= (uint8_t)(1u << i);
            c.tip = true;
            core->ptp_active.contact[core->ptp_active.count++] = c;
        }
    }
}

static void hpe_complete_ptp(kvm_hid_core_t *core)
{
    // A mixed frame can both move and lift fingers. Keep unconfirmed lifts for
    // transport failure, but retire them once the host has read that frame.
    // An intervening RELEASE_ALL may already have cleared the logical ledger.
    unsigned keep = 0;
    for (unsigned i = 0; i < core->ptp_lift.count; ++i) {
        if (core->ptp_inflight_tips & (1u << i))
            core->ptp_lift.contact[keep++] = core->ptp_lift.contact[i];
    }
    core->ptp_lift.count = (uint8_t)keep;
}

static void hpe_clear(kvm_hid_core_t *core)
{
    hpe_studio_cancel(&core->studio);
    core->neutral_barrier = true;
    core->control_neutral_phase = 0;
    core->radial_pressed = false;
    core->joystick_active = false;
    core->ptp_active.count = 0;
}

static uint8_t hpe_neutral(kvm_hid_core_t *core, uint8_t itf, uint8_t *wire)
{
    if (itf == KVM_HID_ITF_PTP) {
        core->ptp_lift.scan_time = (uint16_t)(core->now_us / 100);
        return (uint8_t)hpe_encode_ptp(wire, &core->ptp_lift);
    }
    if (itf == KVM_HID_ITF_CONSUMER) {
        core->neutral_pending[itf] = core->control_neutral_phase < 2;
        switch (core->control_neutral_phase++) {
        case 0:
            wire[0] = HPE_CONSUMER;
            return (uint8_t)(1 + kvm_hid_build_consumer(wire + 1, 0));
        case 1:
            return (uint8_t)hpe_encode_radial(wire, false, 0);
        default: {
            const hpe_joystick_t neutral = {.hat = 8};
            return (uint8_t)hpe_encode_joystick(wire, &neutral);
        }
        }
    }
    return (uint8_t)kvm_hid_build_neutral(itf, protocol_of(core, itf), wire);
}

static void hpe_process(kvm_hid_core_t *core, kvm_hid_qentry_t *e, uint8_t itf)
{
    staged_t *s = staged(core, itf);
    bool complete = true;
    switch (e->kind) {
    case Q_RADIAL: {
        int16_t delta = e->x < -3600 ? -3600 : e->x > 3600 ? 3600 : (int16_t)e->x;
        e->x -= delta;
        core->radial_pressed = e->a != 0;
        s->len = (uint8_t)hpe_encode_radial(s->buf, core->radial_pressed, delta);
        complete = !e->x;
        break;
    }
    case Q_JOYSTICK: {
        const hpe_joystick_t *j = &e->advanced.joystick;
        s->len = (uint8_t)hpe_encode_joystick(s->buf, j);
        core->joystick_active = j->x || j->y || j->rz || j->z || j->buttons || j->hat != 8;
        break;
    }
    case Q_PTP:
        s->len = (uint8_t)hpe_encode_ptp(s->buf, &e->advanced.ptp);
        break;
    }
    if (complete) q_pop(core);
    s->pending = true;
    try_send(core, itf);
}

static kvm_hid_err_t hpe_enqueue(kvm_hid_core_t *core, const kvm_hid_intent_t *in)
{
    if (in->type == KVM_HID_RADIAL) {
        return q_push(core, Q_RADIAL, in->data.radial.pressed, 0, in->data.radial.delta, 0);
    }
    bool ptp = in->type == KVM_HID_PTP_FRAME;
    kvm_hid_err_t err = q_push(core, ptp ? Q_PTP : Q_JOYSTICK, 0, 0, 0, 0);
    if (err == KVM_HID_OK) {
        kvm_hid_qentry_t *tail = q_at(core, (uint16_t)(core->q_count - 1));
        if (ptp) tail->advanced.ptp = in->data.ptp.frame;
        else tail->advanced.joystick = in->data.joystick;
    }
    return err;
}

void kvm_hid_core_ptp_route(kvm_hid_core_t *core, bool native, bool surface_enabled, uint32_t generation)
{
    if (core->ptp_native != native || core->ptp_generation != generation || core->ptp_surface != surface_enabled) {
        core->ptp_generation = generation;
        core->ptp_native = native;
        core->ptp_surface = surface_enabled;
        kvm_hid_core_release_all(core, KVM_RA_GESTURE_CANCEL);
    }
}
