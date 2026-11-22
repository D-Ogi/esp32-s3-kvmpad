// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "kvm_srt.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Route the small set of SSH operations through the same validator, rate limits,
// key shadow and lease checks as WSS. Only fixed vocabulary or integers enter JSON.
static bool send_frame(kvm_srt_t *srt, uint64_t id, const char *type, const char *payload, uint64_t now)
{
    int slot = kvm_rp_slot_of(&srt->rp, id);
    if (slot < 0) return false;
    const kvm_rp_session_t *s = &srt->rp.s[slot];
    char frame[256];
    int n = snprintf(frame, sizeof(frame),
                     "{\"v\":1,\"type\":\"%s\",\"seq\":%" PRIu64 ",\"session_id\":\"%s\","
                     "\"lease_epoch\":%" PRIu32 ",\"payload\":{%s}}",
                     type, s->last_seq + UINT64_C(1), s->label, srt->ssh_epoch, payload);
    return n > 0 && (size_t)n < sizeof(frame) &&
           kvm_rp_on_frame(&srt->rp, id, frame, (size_t)n, now) == KVM_RP_OK;
}

void kvm_srt_ssh_end(kvm_srt_t *srt, uint64_t id, kvm_srt_loss_t loss, uint64_t now)
{
    if (srt && id && id == srt->ssh_session)
        kvm_srt_conn_close(srt, id, loss, now);
}

bool kvm_srt_ssh_begin(kvm_srt_t *srt, uint64_t id, uint64_t sponsor,
                       const uint8_t fingerprint[KVM_PAIR_SHA256_LEN], uint64_t now)
{
    if (!srt || !id || !sponsor || sponsor == id || srt->ssh_session ||
        srt->host_usb != KVM_HOST_USB_MOUNTED || srt->rp.owner_kind == OWNER_REMOTE ||
        kvm_srt_ssh_sponsor(srt, fingerprint) != sponsor)
        return false;
    kvm_srt_admission_t admission = {.kind = KVM_SRT_CONN_CONTROL, .operator_role = true, .client_index = -1};
    for (int i = 0; i < KVM_SRT_MAX_CONNS; ++i)
        if (srt->conn[i].state && srt->conn[i].id == sponsor)
            admission.client_index = srt->conn[i].client_index;
    if (admission.client_index < 0 || kvm_srt_conn_open(srt, id, &admission, now) < 0) return false;
    srt->ssh_session = id;
    srt->ssh_sponsor = sponsor;
    memcpy(srt->ssh_client_key, fingerprint, sizeof(srt->ssh_client_key));
    static const char hello[] = "{\"v\":1,\"type\":\"hello\",\"seq\":1,\"payload\":"
                                "{\"client_name\":\"KVMpad SSH\",\"capabilities\":[\"hid_v1\"]}}";
    if (kvm_rp_on_frame(&srt->rp, id, hello, sizeof(hello)-1, now) == KVM_RP_OK &&
        send_frame(srt, id, "control.request", "\"reason\":\"SSH keyboard and mouse\"", now))
        return true;
    kvm_srt_ssh_end(srt, id, KVM_SRT_LOSS_CLIENT_CLOSE, now);
    return false;
}

kvm_srt_ssh_state_t kvm_srt_ssh_state(kvm_srt_t *srt, uint64_t id, uint64_t now)
{
    if (!srt || !id || id != srt->ssh_session) return KVM_SSH_DENIED;
    srt->now_us = now;
    int slot = kvm_rp_slot_of(&srt->rp, id);
    if (slot >= 0 && srt->host_usb == KVM_HOST_USB_MOUNTED &&
        kvm_srt_ssh_sponsor(srt, srt->ssh_client_key) == srt->ssh_sponsor) {
        const kvm_rp_session_t *s = &srt->rp.s[slot];
        if (!srt->ssh_granted && s->approval_pending && now < s->approval_deadline_us)
            return KVM_SSH_PENDING;
        if (s->state == KVM_RP_SESS_READY && !s->stale && srt->rp.owner_kind == OWNER_REMOTE &&
            srt->rp.owner_session == id && (!srt->ssh_granted || srt->ssh_epoch == srt->rp.owner_epoch) &&
            now - s->last_heartbeat_us < srt->rp.cfg.heartbeat_stale_us) {
            srt->ssh_epoch = srt->rp.owner_epoch;
            srt->ssh_granted = true;
            return KVM_SSH_GRANTED;
        }
    }
    kvm_srt_ssh_end(srt, id, KVM_SRT_LOSS_TIMEOUT, now);
    return KVM_SSH_DENIED;
}

bool kvm_srt_ssh_ping(kvm_srt_t *srt, uint64_t id, uint64_t now)
{
    return kvm_srt_ssh_state(srt, id, now) == KVM_SSH_GRANTED && send_frame(srt, id, "ping", "", now);
}

static const char *button_name(unsigned id)
{
    switch (id) {
    case KVM_MOUSE_BTN_LEFT: return "left";
    case KVM_MOUSE_BTN_MIDDLE: return "middle";
    case KVM_MOUSE_BTN_RIGHT: return "right";
    default: return NULL;
    }
}

static bool key_input(kvm_srt_t *srt, uint64_t id, unsigned usage, bool down, uint64_t now)
{
    char payload[80];
    snprintf(payload, sizeof(payload), "\"kind\":\"keyboard.key\",\"usage_id\":%u,\"down\":%s",
             usage, down ? "true" : "false");
    return send_frame(srt, id, "input", payload, now);
}

static bool button_input(kvm_srt_t *srt, uint64_t id, unsigned button, bool down, uint64_t now)
{
    const char *name = button_name(button);
    if (!name) return false;
    char payload[80];
    snprintf(payload, sizeof(payload), "\"kind\":\"mouse.button\",\"button\":\"%s\",\"down\":%s",
             name, down ? "true" : "false");
    return send_frame(srt, id, "input", payload, now);
}

static bool release_held(kvm_srt_t *srt, uint64_t id, uint64_t now)
{
    int slot = kvm_rp_slot_of(&srt->rp, id);
    if (slot < 0) return false;
    const kvm_rp_session_t *s = &srt->rp.s[slot];
    for (unsigned usage = 0; usage < sizeof(s->keys_down) * 8; ++usage)
        if ((s->keys_down[usage / 8] & (1u << (usage % 8))) && !key_input(srt, id, usage, false, now))
            return false;
    for (unsigned button = 1; button <= 3; ++button)
        if ((s->buttons_down & (1u << (button-1))) && !button_input(srt, id, button, false, now))
            return false;
    return true;
}

bool kvm_srt_ssh_input(kvm_srt_t *srt, uint64_t id, const kvm_hid_intent_t *intent, uint64_t now)
{
    if (!intent || kvm_srt_ssh_state(srt, id, now) != KVM_SSH_GRANTED) return false;
    bool ok = false;
    char payload[80];
    switch (intent->type) {
    case KVM_HID_KEY_DOWN: case KVM_HID_KEY_UP:
        ok = key_input(srt, id, intent->data.key.usage_id, intent->type == KVM_HID_KEY_DOWN, now);
        break;
    case KVM_HID_MOUSE_BTN_DOWN: case KVM_HID_MOUSE_BTN_UP:
        ok = button_input(srt, id, intent->data.mouse_button.button_id, intent->type == KVM_HID_MOUSE_BTN_DOWN, now);
        break;
    case KVM_HID_MOUSE_MOVE:
        snprintf(payload, sizeof(payload), "\"kind\":\"mouse.move\",\"dx\":%d,\"dy\":%d",
                 intent->data.mouse_move.dx, intent->data.mouse_move.dy);
        ok = send_frame(srt, id, "input", payload, now);
        break;
    case KVM_HID_MOUSE_WHEEL:
        snprintf(payload, sizeof(payload), "\"kind\":\"mouse.wheel\",\"horizontal\":%d,\"vertical\":%d",
                 intent->data.wheel.horizontal, intent->data.wheel.vertical);
        ok = send_frame(srt, id, "input", payload, now);
        break;
    case KVM_HID_RELEASE_ALL:
        ok = release_held(srt, id, now);
        break;
    default: break;
    }
    if (ok) return true;
    kvm_srt_ssh_end(srt, id, KVM_SRT_LOSS_PROTOCOL, now);
    return false;
}
