#pragma once

// Integer Web Mercator core for the tile renderer.
//
// World coordinates are fixed-point fractions of the Mercator world:
//   x: Q32 (uint32) - wraps at the antimeridian by plain unsigned overflow
//   y: Q31 (uint32 in [0, 2^31)) - no wrap
// At tile zoom z the world is (256 << z) pixels wide, so one world pixel is
// 2^(24 - z) Q32 units in x and 2^(23 - z) Q31 units in y.
//
// The only transcendental math is one tanf + logf per world_from_latlon().
// A ViewFrame caches everything that depends only on the view centre, so
// per-tile and per-point placement is pure integer work.
//
// Placement reproduces the legacy float code in map_tiles.cpp/map_renderer.cpp
// (latlon_to_tile + tile_get_bounds + lroundf sub-tile offsets) up to rounding
// ties; see test/test_projection_equivalence.cpp.

#include <stdint.h>

struct WorldPoint {
    uint32_t x;  // Q32
    uint32_t y;  // Q31
};

// Cold path (ingestion, once per view change). Longitude is wrapped into
// [-180, 180] like the legacy code and +180 saturates to the last world unit,
// matching the legacy latlon_to_tile() clamp to max_tile. Latitude is clamped
// to the Mercator limit.
WorldPoint world_from_latlon(float lat, float lon);

struct ViewFrame {
    uint32_t cx;       // view centre, Q32
    uint32_t cy;       // view centre, Q31
    uint16_t ctx;      // centre tile (legacy latlon_to_tile semantics)
    uint16_t cty;
    int16_t sub_x;     // legacy sub-tile offset: round(frac * 256) - 128, [-128, 128]
    int16_t sub_y;
    uint8_t z;         // tile zoom, 1..MAX_TILE_LEVEL
};
static_assert(sizeof(ViewFrame) == 20, "ViewFrame must stay a compact POD");

// Pure function: fill a frame for an explicit centre.
void view_frame_compute(ViewFrame* out, float center_lat, float center_lon, uint8_t z);

// LVGL task only. Frame for the live view (view_center_lat/lon), cached on the
// exact bit layout of lat/lon and on z, so repeated calls within one pan
// frame cost a compare.
const ViewFrame& view_frame_get(uint8_t z);

// Legacy tile screen position for a tile offset (dx, dy) from the centre tile:
// screen = W/2 - 128 + d*256 - sub.
inline int view_frame_tile_screen_x(const ViewFrame& vf, int dx, int screen_size) {
    return (screen_size / 2) - 128 + dx * 256 - vf.sub_x;
}
inline int view_frame_tile_screen_y(const ViewFrame& vf, int dy, int screen_size) {
    return (screen_size / 2) - 128 + dy * 256 - vf.sub_y;
}

// Legacy tile-delta wrap (map_tiles.cpp): for z > 1 bring dx into the window
// the old double-if produced. z == 1 is never wrapped (ghost tiles use the
// caller's original_dx instead).
inline int view_frame_wrap_dx(int dx, uint8_t z) {
    if (z > 1) {
        const int max_x = 1 << z;
        if (dx >= max_x / 2) dx -= max_x;
        if (dx <= -max_x / 2) dx += max_x;
    }
    return dx;
}

// Same view (the remaining fields derive from these three).
inline bool view_frame_equal(const ViewFrame& a, const ViewFrame& b) {
    return a.cx == b.cx && a.cy == b.cy && a.z == b.z;
}

// Screen position of a world point in the frame, reproducing the legacy
// latlon_to_screen_mercator() in map_renderer.cpp including its screen-space
// wrap and its [-50, size + 50) visibility window. Integer only.
bool view_frame_project(const ViewFrame& vf, WorldPoint p, int screen_size,
                        int16_t* out_x, int16_t* out_y);
