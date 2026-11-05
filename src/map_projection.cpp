#include "map_projection.h"
#include "map_mercator.h"
#include <string.h>

// Live view centre (map_renderer.cpp).
extern float view_center_lat;
extern float view_center_lon;

static constexpr uint32_t kQ31Max = 0x7FFFFFFFu;
static constexpr uint8_t kMaxFrameZoom = 16;  // TileCoord x/y are uint16_t

WorldPoint world_from_latlon(float lat, float lon) {
    // Same wrap as the legacy latlon_to_tile(): +180 stays +180.
    while (lon > 180.0f) lon -= 360.0f;
    while (lon < -180.0f) lon += 360.0f;

    WorldPoint p;

    // Scale in double: converting a float that rounds to 2^32 into uint32 is
    // undefined, and the float step near 2^32 is 256 units.
    double sx = (((double)lon + 180.0) / 360.0) * 4294967296.0;
    if (sx < 0.0) sx = 0.0;
    p.x = (sx >= 4294967295.0) ? 0xFFFFFFFFu : (uint32_t)(uint64_t)sx;

    double sy = (double)lat_to_mercator_y(lat) * 2147483648.0;  // clamps lat, [0, 1]
    if (sy < 0.0) sy = 0.0;
    p.y = (sy >= (double)kQ31Max) ? kQ31Max : (uint32_t)(uint64_t)sy;
    return p;
}

void view_frame_compute(ViewFrame* vf, float center_lat, float center_lon, uint8_t z) {
    if (z < 1) z = 1;
    if (z > kMaxFrameZoom) z = kMaxFrameZoom;

    const WorldPoint c = world_from_latlon(center_lat, center_lon);
    vf->cx = c.x;
    vf->cy = c.y;
    vf->z = z;
    vf->ctx = (uint16_t)(c.x >> (32 - z));
    vf->cty = (uint16_t)(c.y >> (31 - z));

    // Position of the centre inside its tile, in pixels, rounded half-up.
    const uint32_t rem_x = c.x - ((uint32_t)vf->ctx << (32 - z));
    const uint32_t rem_y = c.y - ((uint32_t)vf->cty << (31 - z));
    const int px = (int)((rem_x + (1u << (23 - z))) >> (24 - z));  // [0, 256]
    const int py = (int)((rem_y + (1u << (22 - z))) >> (23 - z));  // [0, 256]
    vf->sub_x = (int16_t)(px - 128);
    vf->sub_y = (int16_t)(py - 128);
}

const ViewFrame& view_frame_get(uint8_t z) {
    static ViewFrame s_frame;
    static uint32_t s_lat_bits = 0;
    static uint32_t s_lon_bits = 0;
    static uint8_t s_z = 0;  // 0 = nothing cached yet

    uint32_t lat_bits;
    uint32_t lon_bits;
    memcpy(&lat_bits, &view_center_lat, sizeof(lat_bits));
    memcpy(&lon_bits, &view_center_lon, sizeof(lon_bits));

    if (z != s_z || lat_bits != s_lat_bits || lon_bits != s_lon_bits) {
        view_frame_compute(&s_frame, view_center_lat, view_center_lon, z);
        s_lat_bits = lat_bits;
        s_lon_bits = lon_bits;
        s_z = z;
    }
    return s_frame;
}

bool view_frame_project(const ViewFrame& vf, WorldPoint p, int screen_size,
                        int16_t* out_x, int16_t* out_y) {
    const uint8_t z = vf.z;

    // Point tile and its in-tile pixel offset (legacy: latlon_to_tile +
    // lroundf(frac * 256)). x < 2^32 and y < 2^31 keep both tiles <= max_tile.
    const uint32_t ptx = p.x >> (32 - z);
    const uint32_t pty = p.y >> (31 - z);
    const uint32_t rem_x = p.x - (ptx << (32 - z));
    const uint32_t rem_y = p.y - (pty << (31 - z));
    const int point_px = (int)((rem_x + (1u << (23 - z))) >> (24 - z));
    const int point_py = (int)((rem_y + (1u << (22 - z))) >> (23 - z));

    const int dx = view_frame_wrap_dx((int)ptx - (int)vf.ctx, z);
    const int dy = (int)pty - (int)vf.cty;

    int x = view_frame_tile_screen_x(vf, dx, screen_size) + point_px;
    const int y = view_frame_tile_screen_y(vf, dy, screen_size) + point_py;

    // Legacy screen-space wrap (map_renderer.cpp latlon_to_screen_mercator).
    const int world_width = 256 << z;
    if (x < -50 && (x + world_width) < (screen_size + 50)) {
        x += world_width;
    } else if (x >= (screen_size + 50) && (x - world_width) >= -50) {
        x -= world_width;
    }

    *out_x = (int16_t)x;
    *out_y = (int16_t)y;
    return (x >= -50 && x < screen_size + 50 && y >= -50 && y < screen_size + 50);
}
