// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "region.hpp"
#include "frame_demand.hpp"
#include <cassert>
#include <deque>
#include <cstdio>
#include <stdexcept>

static void test_quiet_capture() {
    struct Frame {
        int cursor;
        int* closed;
        explicit operator bool() const { return closed != nullptr; }
        void Close() { ++*closed; }
    };
    std::deque<int> pending;
    int cursor = 512, closed = 0, renewals = 0, sent = 0, emitted_cursor = -1;
    bool fresh_on_open = true, with_metadata = false, granted = true;
    FrameDemand request;
    auto tick = [&](uint64_t time) {
        request.poll(time, [&] {
            if (pending.empty()) return Frame{};
            int sampled = pending.front(); pending.pop_front();
            return Frame{sampled, &closed};
        }, [&] {
            if (!granted) throw std::runtime_error("grant ended");
            ++renewals;
            if (fresh_on_open) pending.push_back(cursor);
        }, [&](const Frame& frame, bool metadata) {
            assert(granted);
            ++sent; emitted_cursor = frame.cursor; with_metadata = metadata;
        });
    };

    // Normal motion returns the newest of two frames, once, without reopening WGC.
    request.begin('M', 0, false);
    pending = {100, 101}; tick(5);
    assert(sent==1 && emitted_cursor==101 && with_metadata && closed==2 && renewals==0);
    // The final desktop change can arrive while the board is withholding credit.
    pending = {108}; tick(10); tick(900);
    assert(closed==3 && sent==1 && renewals==0);
    // A late F on a quiet pool needs a new sample. The cursor moved to 512 in the meantime.
    request.begin('F', 1000, true);
    tick(1249); assert(sent==1 && renewals==0);
    tick(1250); assert(sent==1 && renewals==1);
    tick(1260); assert(sent==2 && emitted_cursor==512 && !with_metadata && closed==4);
    request.begin('M', 1300, true); tick(1550); tick(1560);
    assert(sent==3 && emitted_cursor==512 && with_metadata && closed==5);
    // A normal update arriving just at the deadline wins over a renewal.
    request.begin('F', 2000, true); pending = {513}; tick(2250);
    assert(sent==4 && emitted_cursor==513 && renewals==2);
    // A genuinely silent device cannot turn an old frame into new evidence of liveness.
    fresh_on_open = false;
    request.begin('M', 3000, true); tick(3250); tick(4500); tick(6000);
    assert(sent==4 && renewals==3);
    // P discards the demand and its retry. The initial request after resume starts a new pool.
    request.begin('M', 7000, true); request = {}; tick(8000);
    request.begin('M', 8100, false); tick(8500);
    assert(sent==4 && renewals==3);
    // Revocation at the renewal boundary cannot publish or reacquire a frame.
    request.begin('M', 9000, true); granted=false;
    try { tick(9250); assert(false); } catch (const std::runtime_error&) {}
    assert(sent==4 && renewals==3);
    puts("PASS first demand, motion, static pixels, latest cursor sample, one bounded renewal, pause, revoke");
}

static void test_selected_monitor_mapping() {
    MonitorBounds selected{-1920, -120, 0, 960};
    assert(selected.matches_size(1920, 1080));
    assert(!selected.matches_size(1080, 1920));
    assert(!selected.matches_size(0, 1080));
    assert(check_cursor_mapping(selected, selected, -1920, -120, true, true) == CursorMapping::inside);
    assert(check_cursor_mapping(selected, selected, 0, 100, true, false) == CursorMapping::outside);
    assert(check_cursor_mapping(selected, selected, -12, 100, true, false) == CursorMapping::geometry);
    assert(check_cursor_mapping(selected, selected, 0, 100, true, true) == CursorMapping::geometry);
    assert(check_cursor_mapping(selected, selected, 0, 100, false, false) == CursorMapping::geometry);
    // The frame can keep its width and height when Windows moves the display during encoding.
    MonitorBounds moved{-1919, -120, 1, 960};
    assert(moved.matches_size(1920, 1080));
    assert(check_cursor_mapping(selected, moved, -12, 100, true, true) == CursorMapping::topology);
    MonitorBounds resized{-1920, -120, -1, 960};
    assert(check_cursor_mapping(selected, resized, -1, 100, true, false) == CursorMapping::topology);
    puts("Selected monitor mapping: physical bounds, separate cursor exit and mapping failure, moved and resized displays passed");
}

int main() {
    test_selected_monitor_mapping();
    test_quiet_capture();
    for (int first_entry : {0, 3, 6}) {
        int tick = 0, prompts = 0;
        bool ready = wait_for_cursor_entry([&] { return tick == first_entry; },
            [&] { ++prompts; }, [&] { return ++tick < 5; });
        assert(ready == (first_entry < 5));
        assert(prompts == (first_entry != 0));
        assert(tick == std::min(first_entry, 5));
    }
    // Closing the interactive desktop during the wait must unwind before readiness.
    int checks = 0;
    try {
        wait_for_cursor_entry([] { return false; }, [] {}, [&] {
            if (++checks == 2) throw std::runtime_error("desktop closed");
            return checks < 5;
        });
        assert(false && "cancel was swallowed");
    } catch (const std::runtime_error&) { assert(checks == 2); }
    Region r{};
    for (int side : {256,512,1024}) {
        assert(cursor_region(-1920,-120,0,960,-960,420,1920,1080,side,side,r));
        assert(r.left==960-side/2 && r.top==540-side/2 && !r.clamped);
        assert(cursor_region(-1920,-120,0,960,-1,-120,1920,1080,side,side,r));
        assert(r.left==1920-side && r.top==0 && r.clamped);
    }
    assert(cursor_region(1920,0,3000,1920,2500,1000,1080,1920,960,540,r));
    assert(!cursor_region(1920,0,3000,1920,2500,1000,1920,1080,960,540,r));
    assert(!cursor_region(-1920,-120,0,960,0,0,1920,1080,512,512,r));
    assert(cursor_region(0,0,400,300,200,150,400,300,512,512,r));
    assert(r.width==400 && r.height==300 && r.pad_x==56 && r.pad_y==106 && r.clamped);
    assert(cursor_region(0,0,511,511,256,256,511,511,512,512,r));
    assert(r.pad_x==0 && r.pad_y==0 && r.clamped); // one pixel padded at right and bottom
}
