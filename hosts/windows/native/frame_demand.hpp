// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <cstdint>
#include <utility>

// Each demand consumes at most one current frame. A quiet, already-running pool may
// need a new first frame from the same capture item before the broker's IPC deadline.
struct FrameDemand {
    char command{};
    uint64_t renew_at{};

    void begin(char next, uint64_t now, bool running) {
        command = next;
        renew_at = running ? now + 250 : 0;
    }

    template<class Next, class Renew, class Emit>
    void poll(uint64_t now, Next next, Renew renew, Emit emit) {
        auto frame = next();
        if (!frame) {
            if (renew_at && now >= renew_at) {
                renew_at = 0;
                renew();
            }
            return;
        }
        if (command) {
            if (auto newer = next()) {
                frame.Close();
                frame = std::move(newer);
            }
            emit(frame, command == 'M');
            *this = {};
        }
        frame.Close();
    }
};
