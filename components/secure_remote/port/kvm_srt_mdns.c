// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Optional `_kvmpad._tcp` advertisement (TASK-100; FR-723, ADR-029 §4, ADR-032).
//
// Discovery is a convenience, never a precondition: the CLIENT can always be pointed at an address
// by hand (ADR-028 §2), and a device that cannot advertise must still pair and still serve. So this
// file compiles to three stubs when the managed `espressif/mdns` component is absent, and the
// component declares the dependency as optional. Nothing else in secure_remote references mDNS.
//
// The TXT record is built by the pairing core (kvm_pair_mdns_txt), so the keys and their order are
// decided in one place for both the DEVICE and the CLIENT. `pair=1` appears only while a window is
// open, which is the one bit of state the record carries (SEC-103).

#include <string.h>

#include "esp_log.h"
#include "kvm_srt_server.h"

#if defined(__has_include)
#if __has_include("mdns.h")
#define SRT_HAVE_MDNS 1
#endif
#endif

#ifdef SRT_HAVE_MDNS

#include "kvm_pairing_wire.h"
#include "mdns.h"

static const char *TAG = "srt_mdns";

static bool s_up;
static uint16_t s_port;
static char s_name[KVM_PAIR_NAME_CAP];

bool kvm_srt_mdns_available(void)
{
    return true;
}

static esp_err_t publish(bool pair_open)
{
    kvm_pair_txt_item_t items[KVM_PAIR_MDNS_TXT_ITEMS];
    char scratch[32];
    if (!kvm_pair_mdns_txt(s_name, pair_open, items, scratch, sizeof(scratch))) {
        return ESP_ERR_INVALID_ARG;
    }
    mdns_txt_item_t txt[KVM_PAIR_MDNS_TXT_ITEMS];
    for (size_t i = 0; i < KVM_PAIR_MDNS_TXT_ITEMS; i++) {
        txt[i].key = items[i].name;
        txt[i].value = items[i].value;
    }
    if (!s_up) {
        esp_err_t err = mdns_service_add(NULL, KVM_PAIR_MDNS_SERVICE, KVM_PAIR_MDNS_PROTO, s_port,
                                         txt, KVM_PAIR_MDNS_TXT_ITEMS);
        if (err != ESP_OK) {
            return err;
        }
        s_up = true;
        return ESP_OK;
    }
    return mdns_service_txt_set(KVM_PAIR_MDNS_SERVICE, KVM_PAIR_MDNS_PROTO, txt,
                                KVM_PAIR_MDNS_TXT_ITEMS);
}

esp_err_t kvm_srt_mdns_start(const char *device_name, uint16_t port)
{
    if (device_name == NULL || device_name[0] == '\0' || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_name, 0, sizeof(s_name));
    strncpy(s_name, device_name, sizeof(s_name) - 1u);
    s_port = port;

    // mdns_init() is idempotent in the sense that a second call returns ESP_OK on an initialized
    // stack; the integrator may well have started it already for something else.
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    // The hostname and instance name are the same human-readable name the pairing screen shows, so
    // what the user sees on the device and what the CLIENT lists cannot disagree (FR-723).
    (void)mdns_hostname_set(s_name);
    (void)mdns_instance_name_set(s_name);

    err = publish(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "service not added: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t kvm_srt_mdns_set_pairing(bool pair_open)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    return publish(pair_open);
}

esp_err_t kvm_srt_mdns_stop(void)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mdns_service_remove(KVM_PAIR_MDNS_SERVICE, KVM_PAIR_MDNS_PROTO);
    s_up = false;
    // mdns_free() is deliberately not called: the stack may be shared with other advertisements.
    return err;
}

#else // no mDNS component in the build

bool kvm_srt_mdns_available(void)
{
    return false;
}

esp_err_t kvm_srt_mdns_start(const char *device_name, uint16_t port)
{
    (void)device_name;
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t kvm_srt_mdns_set_pairing(bool pair_open)
{
    (void)pair_open;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t kvm_srt_mdns_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
