# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# secure_remote (TASK-100, transport + pairing part).
#
# Two layers in one component:
#   * pure C cores -- `src/` (session glue, admission, `pair.*` codec) and `pairing/src/` (EC-JPAKE,
#     wire blobs, session state). These have no ESP-IDF dependency beyond PSA crypto and are built
#     by the host suites in test/secure_remote and test/pairing.
#   * ESP-IDF adapters -- `port/`: the per-device TLS identity, the `esp_https_server` WebSocket
#     endpoints and the optional mDNS advertisement. They are the only files here that know about
#     TLS, sockets, tasks or the MAC address.
#
# `pairing/src/kvm_pairing_client.c` is deliberately NOT built into the firmware: it is the CLIENT
# half of the handshake, used by the host interop tests to drive the DEVICE half. Shipping it would
# put the CLIENT's key schedule in the image for nothing.
#
# REQUIRES is public because the public headers use those types (intents, protocol v1 sessions, the
# approval-policy enum). Everything TLS-related is private: no caller of this component includes an
# esp_https_server header.
idf_component_register(
    SRCS "src/kvm_srt.c"
         "src/kvm_srt_auth.c"
         "src/kvm_srt_pair_json.c"
         "pairing/src/kvm_pairing_pake.c"
         "pairing/src/kvm_pairing_session.c"
         "pairing/src/kvm_pairing_wire.c"
         "port/kvm_srt_identity.c"
         "port/kvm_srt_server.c"
         "port/kvm_srt_mdns.c"
    INCLUDE_DIRS "include" "pairing/include"
    REQUIRES hid_core remote_protocol config_store
    PRIV_REQUIRES mbedtls esp_https_server esp_timer esp_hw_support log freertos)

# FR-723 is a convenience, not a precondition: without the managed `espressif/mdns` component the
# adapter in port/kvm_srt_mdns.c compiles to stubs that report ESP_ERR_NOT_SUPPORTED, and pairing by
# address keeps working. Adding the dependency is the integrator's call (see README).
idf_component_optional_requires(PRIVATE espressif__mdns)
