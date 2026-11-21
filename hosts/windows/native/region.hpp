// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <algorithm>
#include <cstdint>

struct Region { int left, top, width, height, pad_x, pad_y; bool clamped; };

struct MonitorBounds {
    int left, top, right, bottom;
    bool matches_size(int width, int height) const {
        return width > 0 && height > 0 &&
               int64_t(right) - left == width && int64_t(bottom) - top == height;
    }
};

enum class CursorMapping { inside, outside, geometry, topology };

inline CursorMapping check_cursor_mapping(MonitorBounds chosen, MonitorBounds current,
                                          int x, int y, bool has_monitor, bool on_chosen) {
    if (chosen.left != current.left || chosen.top != current.top ||
        chosen.right != current.right || chosen.bottom != current.bottom)
        return CursorMapping::topology;
    bool inside = x >= chosen.left && x < chosen.right && y >= chosen.top && y < chosen.bottom;
    if (!has_monitor || inside != on_chosen) return CursorMapping::geometry;
    return inside ? CursorMapping::inside : CursorMapping::outside;
}

// Wait for the user's first entry before announcing the display to the board.
// This gate is used only at startup. Leaving an admitted monitor ends the session.
template<class Inside, class Prompt, class Poll>
bool wait_for_cursor_entry(Inside inside, Prompt prompt, Poll poll) {
    if (inside()) return true;
    prompt();
    while (poll()) {
        if (inside()) return true;
    }
    return false;
}

// Coordinates are physical virtual-desktop pixels and the right/bottom edges are exclusive.
inline bool cursor_region(int left,int top,int right,int bottom,int cursor_x,int cursor_y,
                          int frame_w,int frame_h,int crop_w,int crop_h,Region& result) {
    if (int64_t(right)-left!=frame_w || int64_t(bottom)-top!=frame_h ||
        crop_w<=0 || crop_h<=0 || frame_w<=0 || frame_h<=0 ||
        cursor_x<left || cursor_x>=right || cursor_y<top || cursor_y>=bottom) return false;
    int center_x=cursor_x-left-crop_w/2,center_y=cursor_y-top-crop_h/2;
    result.width=std::min(crop_w,frame_w);
    result.height=std::min(crop_h,frame_h);
    result.left=std::clamp(center_x,0,frame_w-result.width);
    result.top=std::clamp(center_y,0,frame_h-result.height);
    result.pad_x=(crop_w-result.width)/2;
    result.pad_y=(crop_h-result.height)/2;
    result.clamped=center_x!=result.left || center_y!=result.top ||
                   result.width!=crop_w || result.height!=crop_h;
    return true;
}

// KCM1 native IPC prefix. The picker emits these 32 little-endian bytes only for a metadata demand.
struct CropPrefix {
    char magic[4];
    int32_t cursor_x,cursor_y,left,top;
    uint16_t width,height,pad_x,pad_y;
    uint8_t clamped,reserved[3];
};
static_assert(sizeof(CropPrefix)==32);
inline CropPrefix crop_prefix(int monitor_x,int monitor_y,int cursor_x,int cursor_y,const Region& r) {
    return {{'K','C','M','1'},cursor_x,cursor_y,monitor_x+r.left,monitor_y+r.top,
            uint16_t(r.width),uint16_t(r.height),uint16_t(r.pad_x),uint16_t(r.pad_y),uint8_t(r.clamped),{}};
}
