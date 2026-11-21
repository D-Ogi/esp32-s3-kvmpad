// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "hpe_studio.h"
#include <stdbool.h>
#ifdef _WIN32
#define TEST_API __declspec(dllexport)
#else
#define TEST_API
#endif

// Test-only host ABI around the firmware state machine. All storage uses the firmware types.
static hpe_studio_t link;
TEST_API void peer_reset(void) { hpe_studio_start(&link,42); }
TEST_API bool peer_receive(const uint8_t *wire) {
    hpe_packet_t packet;
    return hpe_decode_packet(wire,64,HPE_STUDIO_OUT,&packet) && hpe_studio_receive(&link,&packet,1000);
}
TEST_API bool peer_next(uint8_t *wire) {
    hpe_packet_t packet;
    return hpe_studio_next(&link,1000,&packet) && hpe_encode_packet(wire,HPE_STUDIO_IN,&packet);
}
TEST_API bool peer_action(void) {
    return hpe_studio_action(&link,1,0x101,link.status.peer_nonce,1000);
}
TEST_API bool peer_finished(void) {
    return link.status.phase==HPE_FINISHED && link.status.result==0 && link.status.state[0]==2;
}
