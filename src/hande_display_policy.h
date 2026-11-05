#pragma once

// Pure display math (no Arduino, no LVGL): host-tested on the host.
// Orientation numbering and the LVGL rotation each one needs follow one table: the CO5300 always scans in its
// native direction and
// every non-zero rotation is done in software (hardware 180-degree scan was tried and reverted).

#include <stdint.h>

namespace hande_display_policy {

// Physical USB position. Values are stored in NVS and sent over the web APIs: never renumber.
enum Orientation : uint8_t {
    kOrient0 = 0,    // USB at the bottom
    kOrient90 = 1,   // USB on the left (device turned 90 degrees clockwise)
    kOrient180 = 2,  // USB at the top
    kOrient270 = 3,  // USB on the right
};

// LVGL 9 rotation values (lv_display_rotation_t), repeated here so this header needs no LVGL.
enum Rotation : uint8_t {
    kRot0 = 0,
    kRot90 = 1,
    kRot180 = 2,
    kRot270 = 3,
};

struct Area {
    int32_t x1, y1, x2, y2;  // inclusive, like lv_area_t
};

// The LVGL rotation that shows the UI upright for a physical orientation. Out-of-range input means kOrient0.
constexpr Rotation rotation_for(uint8_t orientation) {
    return orientation == kOrient90    ? kRot270
           : orientation == kOrient180 ? kRot180
           : orientation == kOrient270 ? kRot90
                                       : kRot0;
}

// CO5300 QSPI moves 2 pixels (4 bytes) per burst: every flushed area must start on an even pixel and end on an
// odd one, on BOTH axes (90/270 software rotation swaps them). Clamping to the panel keeps that parity.
inline void round_area(Area& a, int32_t width, int32_t height) {
    a.x1 &= ~1;
    if (!(a.x2 & 1)) a.x2++;
    a.y1 &= ~1;
    if (!(a.y2 & 1)) a.y2++;
    if (a.x1 < 0) a.x1 = 0;
    if (a.y1 < 0) a.y1 = 0;
    if (a.x2 > width - 1) {
        a.x2 = width - 1;
        if (!(a.x2 & 1)) a.x2--;
    }
    if (a.y2 > height - 1) {
        a.y2 = height - 1;
        if (!(a.y2 & 1)) a.y2--;
    }
}

struct PhysicalBlit {
    int32_t x, y, w, h;  // where the rotated strip lands on the panel, and its size there
};

// Where an area that LVGL rendered in its rotated frame lands on the square panel after lv_draw_sw_rotate.
// size = panel width = height.
inline PhysicalBlit physical_blit(const Area& a, Rotation rotation, int32_t size) {
    const int32_t w = a.x2 - a.x1 + 1;
    const int32_t h = a.y2 - a.y1 + 1;
    return rotation == kRot90    ? PhysicalBlit{a.y1, size - 1 - a.x2, h, w}
           : rotation == kRot180 ? PhysicalBlit{size - 1 - a.x2, size - 1 - a.y2, w, h}
           : rotation == kRot270 ? PhysicalBlit{size - 1 - a.y2, a.x1, h, w}
                                 : PhysicalBlit{a.x1, a.y1, w, h};
}

// A visual (upright UI) touch point from the raw panel point, for app gestures. LVGL itself gets raw points.
struct Point {
    int32_t x, y;
};
constexpr Point visual_point(Point raw, uint8_t orientation, int32_t size) {
    return orientation == kOrient90    ? Point{raw.y, size - 1 - raw.x}
           : orientation == kOrient180 ? Point{size - 1 - raw.x, size - 1 - raw.y}
           : orientation == kOrient270 ? Point{size - 1 - raw.y, raw.x}
                                       : raw;
}

// The raw panel point for a visual one (inverse of visual_point): the touch pipeline reports raw points, the UI
// works in visual ones.
constexpr Point raw_point(Point visual, uint8_t orientation, int32_t size) {
    return orientation == kOrient90    ? Point{size - 1 - visual.y, visual.x}
           : orientation == kOrient180 ? Point{size - 1 - visual.x, size - 1 - visual.y}
           : orientation == kOrient270 ? Point{visual.y, size - 1 - visual.x}
                                       : visual;
}

// Rectangle fills on the CO5300 over QSPI show the wrong colour above about 1024 pixels per write
// on the panel used here: fills are split into rows of at most this many pixels.
constexpr int32_t kMaxFillPixels = 1024;

}  // namespace hande_display_policy
