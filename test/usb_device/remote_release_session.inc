// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

static void remote_release_session(void)
{
    const uint64_t client = 81, other = 82;
    const kvm_hid_intent_t key = {.type = KVM_HID_KEY_DOWN, .data.key.usage_id = 4};
    const kvm_hid_intent_t later = {.type = KVM_HID_KEY_DOWN, .data.key.usage_id = 5};
    const kvm_hid_intent_t mouse = {.type = KVM_HID_MOUSE_BTN_DOWN, .data.mouse_button.button_id = KVM_MOUSE_BTN_LEFT};
    const kvm_hid_intent_t clear = {.type = KVM_HID_RELEASE_ALL};
    uint32_t epoch;
    reset_bus(); configure(1); drain();
    now += 10000000;
    kvm_hid_snapshot_t initial = snapshot();
    dispatch_wait_mode = TIMEOUT_BEFORE_PUMP;
    assert(hid_dispatch_remote_grant(client, &epoch) == KVM_HID_ERR_CONTROL_DENIED);
    drain();
    assert(snapshot().owner_kind == initial.owner_kind && snapshot().owner_epoch == initial.owner_epoch);
    dispatch_wait_mode = TIMEOUT_AFTER_PUMP;
    assert(hid_dispatch_remote_grant(client, &epoch) == KVM_HID_ERR_CONTROL_DENIED);
    drain();
    assert(snapshot().owner_kind == OWNER_NONE);
    assert(hid_dispatch_remote_grant(client, &epoch) == KVM_HID_OK);
    drain();
    assert(hid_dispatch_remote(client, epoch, &key) == ESP_OK);
    assert(hid_dispatch_remote(client, epoch, &mouse) == ESP_OK);
    drain();
    check_input_report(0, 8, (const uint8_t[]){0,0,4,0,0,0,0,0}, 8);
    check_input_report(1, 5, (const uint8_t[]){1,0,0,0,0}, 5);
    // A refused second client or a queued duplicate of the same grant cannot revoke this lease.
    uint32_t unused;
    assert(hid_dispatch_remote_grant(other, &unused) == KVM_HID_ERR_CONTROL_DENIED);
    dispatch_wait_mode = TIMEOUT_BEFORE_PUMP;
    assert(hid_dispatch_remote_grant(client, &unused) == KVM_HID_ERR_CONTROL_DENIED);
    drain();
    assert(snapshot().owner_kind == OWNER_REMOTE && snapshot().owner_epoch == epoch);

    assert(hid_dispatch_remote(other, epoch, &clear) == ESP_OK);
    assert(hid_dispatch_remote(client, epoch - 1, &clear) == ESP_OK);
    assert(hid_dispatch_remote_disconnect_sync(other, KVM_RA_REMOTE_DISCONNECT) == epoch);
    assert(!endpoint[0x81].pending && !endpoint[0x82].pending);
    assert(snapshot().keys_down == 1 && snapshot().owner_kind == OWNER_REMOTE);

    unsigned presses = keyboard_presses;
    assert(hid_dispatch_remote(client, epoch, &later) == ESP_OK);
    assert(hid_dispatch_remote(client, epoch, &clear) == ESP_OK);
    assert(hid_dispatch_remote(client, epoch, &key) == ESP_OK);
    service();
    assert(endpoint[0x81].pending && endpoint[0x81].len == 8);
    assert(!memcmp(endpoint[0x81].data, (const uint8_t[8]){0}, 8));
    assert(endpoint[0x82].pending && endpoint[0x82].len == 5);
    assert(!memcmp(endpoint[0x82].data, (const uint8_t[5]){0}, 5));
    drain();
    assert(keyboard_presses == presses + 1 && snapshot().owner_epoch == epoch);
    check_input_report(0, 8, (const uint8_t[]){0,0,4,0,0,0,0,0}, 8);

    for (unsigned i = 0; i < 64; ++i) assert(hid_dispatch_remote(client, epoch, &later) == ESP_OK);
    assert(hid_dispatch_remote(client, epoch, &clear) == ESP_OK);
    drain();
    assert(keyboard_presses == presses + 1 && snapshot().ledger_empty && snapshot().owner_epoch == epoch);
    assert(hid_dispatch_remote(client, epoch, &key) == ESP_OK);
    drain();
    for (unsigned i = 0; i < 64; ++i) assert(hid_dispatch_remote(client, epoch, &later) == ESP_OK);
    assert(hid_dispatch_remote(client, epoch, &later) == ESP_ERR_TIMEOUT);
    drain();
    assert(keyboard_presses == presses + 2 && snapshot().ledger_empty && snapshot().owner_epoch == epoch);
    assert(hid_dispatch_remote(client, epoch, &key) == ESP_OK);
    drain();
    presses = keyboard_presses;
    // A full producer queue must not delay the loss behind the key presses, or change its reason.
    for (unsigned i = 0; i < 64; ++i) assert(hid_dispatch_remote(client, epoch, &later) == ESP_OK);
    kvm_hid_counters_t before, after;
    hid_dispatch_get_counters(&before);
    assert(hid_dispatch_remote_disconnect_sync(client, KVM_RA_LEASE_TIMEOUT) != epoch);
    assert(snapshot().owner_kind == OWNER_NONE && snapshot().ledger_empty);
    assert(endpoint[0x81].pending && !memcmp(endpoint[0x81].data, (const uint8_t[8]){0}, 8));
    assert(endpoint[0x82].pending && !memcmp(endpoint[0x82].data, (const uint8_t[5]){0}, 5));
    drain();
    assert(keyboard_presses == presses);
    hid_dispatch_get_counters(&after);
    assert(after.release_all == before.release_all + 1);
    assert(after.release_by_reason[KVM_RA_LEASE_TIMEOUT] == before.release_by_reason[KVM_RA_LEASE_TIMEOUT] + 1);

    assert(hid_dispatch_remote_grant(client, &epoch) == KVM_HID_OK);
    drain();
    assert(hid_dispatch_remote(client, epoch, &key) == ESP_OK);
    drain();
    assert(hid_dispatch_local(&mouse) == ESP_OK);
    drain();
    assert(snapshot().owner_kind == OWNER_LOCAL);
    uint32_t local_epoch = snapshot().owner_epoch;
    for (unsigned i = 0; i < 64; ++i) assert(hid_dispatch_remote(client, epoch, &key) == ESP_OK);
    assert(hid_dispatch_remote(client, epoch, &key) == ESP_ERR_TIMEOUT);
    assert(hid_dispatch_remote(client, epoch, &clear) == ESP_OK);
    hid_dispatch_remote_disconnect(client, KVM_RA_REMOTE_DISCONNECT);
    assert(hid_dispatch_remote_disconnect_sync(client, KVM_RA_REMOTE_DISCONNECT) == local_epoch);
    drain();
    check_input_report(1, 5, (const uint8_t[]){1,0,0,0,0}, 5);
    assert(snapshot().owner_kind == OWNER_LOCAL && !snapshot().ledger_empty);
    const kvm_hid_intent_t up = {.type = KVM_HID_MOUSE_BTN_UP, .data.mouse_button.button_id = KVM_MOUSE_BTN_LEFT};
    assert(hid_dispatch_local(&up) == ESP_OK);
    drain();
    now += 10000000;
    assert(hid_dispatch_remote_grant(client, &epoch) == KVM_HID_OK);
    drain();
    assert(hid_dispatch_remote(client, epoch, &key) == ESP_OK);
    assert(hid_dispatch_local(&mouse) == ESP_OK);
    hid_dispatch_remote_release(client);
    drain();
    assert(snapshot().owner_kind == OWNER_LOCAL && snapshot().keys_down == 0);
    check_input_report(1, 5, (const uint8_t[]){1,0,0,0,0}, 5);
    puts("PASS session-scoped release: neutral USB reports, retained local drag, full queue, cutoff and reason");
}
