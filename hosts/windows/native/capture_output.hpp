// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "cinepak.hpp"
#include "jpeg.hpp"

// Recheck the capture grant after encoding, immediately before releasing pixels to the broker.
template<class CheckGrant, class Write>
void publish_frame(std::vector<uint32_t>& pixels, int width, int height, bool jpeg_output,
                   CheckGrant check_grant, Write write) {
    if (width == cinepak::width) {
        auto packet = cinepak::encode(pixels.data());
        check_grant();
        write(packet.data(), packet.size());
    } else if (jpeg_output) {
        auto packet = jpeg::encode(pixels.data(), width, height);
        uint32_t count = static_cast<uint32_t>(packet.size());
        check_grant();
        write(&count, sizeof(count));
        if (count) write(packet.data(), count);
    } else {
        check_grant();
        write(pixels.data(), pixels.size() * sizeof(uint32_t));
    }
}
