// Equivalence tests: integer ViewFrame projection (src/map_projection.cpp)
// against the legacy float placement code it replaces. The legacy functions
// below are verbatim copies from the pre-change sources (tile_manager.cpp,
// map_tiles.cpp, map_renderer.cpp) with only the names prefixed legacy_.
//
// Host float math is not bit-identical to the ESP32-S3 FPU + newlib, so these
// tests bound the difference (|delta| <= 1 px) rather than require equality.

#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "map_mercator.h"
#include "map_projection.h"

float view_center_lat = 0.0f;
float view_center_lon = 0.0f;

namespace {

constexpr int kScreen = 480;  // square216 board profile

struct LegacyTile { uint8_t z; uint16_t x; uint16_t y; };

// --- tile_manager.cpp (pre-change) -------------------------------------------
LegacyTile legacy_latlon_to_tile(float lat, float lon, uint8_t z) {
    LegacyTile coord;
    coord.z = z;
    if (lat > 85.0511f) lat = 85.0511f;
    if (lat < -85.0511f) lat = -85.0511f;
    while (lon > 180.0f) lon -= 360.0f;
    while (lon < -180.0f) lon += 360.0f;
    float n = powf(2.0f, z);
    coord.x = (uint16_t)floorf((lon + 180.0f) / 360.0f * n);
    float lat_rad = lat * M_PI / 180.0f;
    coord.y = (uint16_t)floorf((1.0f - logf(tanf(lat_rad) + 1.0f / cosf(lat_rad)) / M_PI) / 2.0f * n);
    uint16_t max_tile = (1 << z) - 1;
    if (coord.x > max_tile) coord.x = max_tile;
    if (coord.y > max_tile) coord.y = max_tile;
    return coord;
}

void legacy_tile_get_bounds(LegacyTile coord, float* min_lat, float* min_lon, float* max_lat, float* max_lon) {
    float n = powf(2.0f, coord.z);
    *min_lon = coord.x / n * 360.0f - 180.0f;
    *max_lon = (coord.x + 1) / n * 360.0f - 180.0f;
    float min_lat_rad = atanf(sinhf(M_PI * (1.0f - 2.0f * (coord.y + 1) / n)));
    float max_lat_rad = atanf(sinhf(M_PI * (1.0f - 2.0f * coord.y / n)));
    *min_lat = min_lat_rad * 180.0f / M_PI;
    *max_lat = max_lat_rad * 180.0f / M_PI;
}

// --- map_tiles.cpp lat_to_mercator_y_local / map_renderer.cpp static copy ----
float legacy_lat_to_mercator_y_double_pi(float lat) {
    if (lat > 85.0511f) lat = 85.0511f;
    if (lat < -85.0511f) lat = -85.0511f;
    float lat_rad = lat * M_PI / 180.0f;
    return (1.0f - logf(tanf(lat_rad) + 1.0f / cosf(lat_rad)) / M_PI) / 2.0f;
}

// --- map_tiles.cpp calculate_tile_screen_pos (uses the global float Mercator)
void legacy_calculate_tile_screen_pos(LegacyTile tc, float vlat, float vlon, int* sx, int* sy) {
    LegacyTile center = legacy_latlon_to_tile(vlat, vlon, tc.z);
    int dx = (int)tc.x - (int)center.x;
    int dy = (int)tc.y - (int)center.y;
    if (tc.z > 1) {
        int max_x = (1 << tc.z);
        if (dx >= max_x / 2) dx -= max_x;
        if (dx <= -max_x / 2) dx += max_x;
    }
    int tile_size = 256;
    int half_tile = tile_size / 2;
    float tile_min_lat, tile_min_lon, tile_max_lat, tile_max_lon;
    legacy_tile_get_bounds(center, &tile_min_lat, &tile_min_lon, &tile_max_lat, &tile_max_lon);
    float frac_x = (vlon - tile_min_lon) / (tile_max_lon - tile_min_lon);
    float view_merc_y = lat_to_mercator_y(vlat);
    float tile_merc_top = lat_to_mercator_y(tile_max_lat);
    float tile_merc_bot = lat_to_mercator_y(tile_min_lat);
    float frac_y = (view_merc_y - tile_merc_top) / (tile_merc_bot - tile_merc_top);
    int sub_offset_x = lroundf((frac_x - 0.5f) * tile_size);
    int sub_offset_y = lroundf((frac_y - 0.5f) * tile_size);
    *sx = (kScreen / 2) - half_tile + (dx * tile_size) - sub_offset_x;
    *sy = (kScreen / 2) - half_tile + (dy * tile_size) - sub_offset_y;
}

// --- map_tiles.cpp render_tile_from_psram placement (double-pi local Mercator)
void legacy_render_tile_pos(LegacyTile tc, int original_dx, int original_dy, float vlat, float vlon,
                            int* sx, int* sy) {
    LegacyTile center = legacy_latlon_to_tile(vlat, vlon, tc.z);
    int dx, dy;
    if (tc.z == 1) {
        dx = original_dx;
        dy = original_dy;
    } else {
        dx = (int)tc.x - (int)center.x;
        dy = (int)tc.y - (int)center.y;
        int max_x = (1 << tc.z);
        if (dx >= max_x / 2) dx -= max_x;
        if (dx <= -max_x / 2) dx += max_x;
    }
    int tile_size = 256;
    int half_tile = tile_size / 2;
    float tile_min_lat, tile_min_lon, tile_max_lat, tile_max_lon;
    legacy_tile_get_bounds(center, &tile_min_lat, &tile_min_lon, &tile_max_lat, &tile_max_lon);
    float frac_x = (vlon - tile_min_lon) / (tile_max_lon - tile_min_lon);
    float view_merc_y = legacy_lat_to_mercator_y_double_pi(vlat);
    float tile_merc_top = legacy_lat_to_mercator_y_double_pi(tile_max_lat);
    float tile_merc_bot = legacy_lat_to_mercator_y_double_pi(tile_min_lat);
    float frac_y = (view_merc_y - tile_merc_top) / (tile_merc_bot - tile_merc_top);
    int sub_offset_x = lroundf((frac_x - 0.5f) * tile_size);
    int sub_offset_y = lroundf((frac_y - 0.5f) * tile_size);
    *sx = (kScreen / 2) - half_tile + (dx * tile_size) - sub_offset_x;
    *sy = (kScreen / 2) - half_tile + (dy * tile_size) - sub_offset_y;
}

// --- map_renderer.cpp latlon_to_screen_mercator (dots, double-pi Mercator) ---
bool legacy_dot_screen(float lat, float lon, uint8_t z, float vlat, float vlon, int16_t* sx, int16_t* sy) {
    LegacyTile center = legacy_latlon_to_tile(vlat, vlon, z);
    LegacyTile point = legacy_latlon_to_tile(lat, lon, z);
    int tile_size = 256;
    int half_tile = tile_size / 2;
    float tile_min_lat, tile_min_lon, tile_max_lat, tile_max_lon;
    legacy_tile_get_bounds(center, &tile_min_lat, &tile_min_lon, &tile_max_lat, &tile_max_lon);
    float frac_x_center = (vlon - tile_min_lon) / (tile_max_lon - tile_min_lon);
    float center_merc_y = legacy_lat_to_mercator_y_double_pi(vlat);
    float tile_merc_top = legacy_lat_to_mercator_y_double_pi(tile_max_lat);
    float tile_merc_bot = legacy_lat_to_mercator_y_double_pi(tile_min_lat);
    float frac_y_center = (center_merc_y - tile_merc_top) / (tile_merc_bot - tile_merc_top);
    int sub_offset_x = lroundf((frac_x_center - 0.5f) * tile_size);
    int sub_offset_y = lroundf((frac_y_center - 0.5f) * tile_size);
    float point_min_lat, point_min_lon, point_max_lat, point_max_lon;
    legacy_tile_get_bounds(point, &point_min_lat, &point_min_lon, &point_max_lat, &point_max_lon);
    float frac_x_point = (lon - point_min_lon) / (point_max_lon - point_min_lon);
    float point_y = legacy_lat_to_mercator_y_double_pi(lat);
    float tile_y_top = legacy_lat_to_mercator_y_double_pi(point_max_lat);
    float tile_y_bottom = legacy_lat_to_mercator_y_double_pi(point_min_lat);
    float frac_y_point = (point_y - tile_y_top) / (tile_y_bottom - tile_y_top);
    int point_px = lroundf(frac_x_point * tile_size);
    int point_py = lroundf(frac_y_point * tile_size);
    int dx = (int)point.x - (int)center.x;
    int dy = (int)point.y - (int)center.y;
    if (z > 1) {
        int max_x = (1 << z);
        if (dx >= max_x / 2) dx -= max_x;
        if (dx <= -max_x / 2) dx += max_x;
    }
    int x = (kScreen / 2) - half_tile + (dx * tile_size) - sub_offset_x + point_px;
    int y = (kScreen / 2) - half_tile + (dy * tile_size) - sub_offset_y + point_py;
    int world_width = (1 << z) * tile_size;
    if (x < -50 && (x + world_width) < (kScreen + 50)) {
        x += world_width;
    } else if (x >= (kScreen + 50) && (x - world_width) >= -50) {
        x -= world_width;
    }
    *sx = (int16_t)x;
    *sy = (int16_t)y;
    return (x >= -50 && x < kScreen + 50 && y >= -50 && y < kScreen + 50);
}

// --- new path helpers ----------------------------------------------------------
void new_tile_pos(const ViewFrame& vf, LegacyTile tc, int original_dx, int original_dy, bool use_original,
                  int* sx, int* sy) {
    int dx, dy;
    if (use_original && tc.z == 1) {
        dx = original_dx;
        dy = original_dy;
    } else {
        dx = view_frame_wrap_dx((int)tc.x - (int)vf.ctx, tc.z);
        dy = (int)tc.y - (int)vf.cty;
    }
    *sx = view_frame_tile_screen_x(vf, dx, kScreen);
    *sy = view_frame_tile_screen_y(vf, dy, kScreen);
}

uint32_t g_lcg = 0x9E3779B9u;
float rnd01() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (float)(g_lcg >> 8) * (1.0f / 16777216.0f);
}

struct Stats {
    long n = 0, mismatch = 0, max_abs = 0;
    void add(int a, int b) {
        ++n;
        const long d = labs((long)a - (long)b);
        if (d) ++mismatch;
        if (d > max_abs) max_abs = d;
    }
};

}  // namespace

// Tile placement: pan path (global float Mercator) and full-render path
// (double-pi local Mercator) against the ViewFrame, all zooms, 3x3 grid.
void test_projection_tile_placement_equivalence() {
    Stats pan_path, render_path;
    for (uint8_t z = 1; z <= 6; ++z) {
        const int max_tile = (1 << z) - 1;
        for (float vlat = -85.0f; vlat <= 85.0f; vlat += 0.37f) {
            for (float vlon = -180.0f; vlon <= 180.0f; vlon += 0.53f) {
                ViewFrame vf;
                view_frame_compute(&vf, vlat, vlon, z);
                const LegacyTile c = legacy_latlon_to_tile(vlat, vlon, z);
                // Centre tile must agree except at float tile-boundary ties.
                TEST_ASSERT_TRUE(abs((int)c.x - (int)vf.ctx) <= 1 || abs((int)c.x - (int)vf.ctx) == max_tile);
                TEST_ASSERT_TRUE(abs((int)c.y - (int)vf.cty) <= 1);
                if (c.x != vf.ctx || c.y != vf.cty) continue;  // boundary tie: both valid, skip
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int tx = (int)c.x + dx;
                        if (tx < 0) tx += max_tile + 1;
                        if (tx > max_tile) tx -= max_tile + 1;
                        int ty = (int)c.y + dy;
                        if (ty < 0) ty = 0;
                        if (ty > max_tile) ty = max_tile;
                        const LegacyTile tc = {z, (uint16_t)tx, (uint16_t)ty};
                        int lx, ly, nx, ny;
                        legacy_calculate_tile_screen_pos(tc, vlat, vlon, &lx, &ly);
                        new_tile_pos(vf, tc, dx, dy, false, &nx, &ny);
                        pan_path.add(lx, nx);
                        pan_path.add(ly, ny);
                        legacy_render_tile_pos(tc, dx, dy, vlat, vlon, &lx, &ly);
                        new_tile_pos(vf, tc, dx, dy, true, &nx, &ny);
                        render_path.add(lx, nx);
                        render_path.add(ly, ny);
                    }
                }
            }
        }
    }
    printf("\n[EQUIV] tile pan-path    : %ld coords, %ld differ (%.4f%%), max |d| = %ld px\n",
           pan_path.n, pan_path.mismatch, 100.0 * pan_path.mismatch / pan_path.n, pan_path.max_abs);
    printf("[EQUIV] tile render-path : %ld coords, %ld differ (%.4f%%), max |d| = %ld px\n",
           render_path.n, render_path.mismatch, 100.0 * render_path.mismatch / render_path.n, render_path.max_abs);
    TEST_ASSERT_LESS_OR_EQUAL(1, pan_path.max_abs);
    TEST_ASSERT_LESS_OR_EQUAL(1, render_path.max_abs);
}

// Dot projection: legacy latlon_to_screen_mercator vs world_from_latlon +
// view_frame_project on random view/point pairs (points within +-2 tiles, and
// some anywhere), all zooms.
void test_projection_dot_equivalence() {
    Stats pos;
    long vis_n = 0, vis_mismatch_interior = 0, vis_mismatch_edge = 0;
    for (uint8_t z = 1; z <= 6; ++z) {
        const float tile_deg = 360.0f / (float)(1 << z);
        for (int i = 0; i < 400000; ++i) {
            const float vlat = -84.0f + 168.0f * rnd01();
            const float vlon = -180.0f + 360.0f * rnd01();
            float lat, lon;
            if (i % 5 == 0) {
                lat = -85.0f + 170.0f * rnd01();
                lon = -180.0f + 360.0f * rnd01();
            } else {
                lat = vlat + (rnd01() * 4.0f - 2.0f) * tile_deg * 0.5f;
                lon = vlon + (rnd01() * 4.0f - 2.0f) * tile_deg;
                if (lat > 85.0f) lat = 85.0f;
                if (lat < -85.0f) lat = -85.0f;
                if (lon > 180.0f) lon -= 360.0f;
                if (lon < -180.0f) lon += 360.0f;
            }
            int16_t lx, ly, nx, ny;
            const bool lv = legacy_dot_screen(lat, lon, z, vlat, vlon, &lx, &ly);
            ViewFrame vf;
            view_frame_compute(&vf, vlat, vlon, z);
            const bool nv = view_frame_project(vf, world_from_latlon(lat, lon), kScreen, &nx, &ny);
            ++vis_n;
            if (lv != nv) {
                // Visibility may only flip when a coordinate sits on the window edge.
                const bool edge = (abs(lx + 50) <= 1 || abs(lx - (kScreen + 50)) <= 1 ||
                                   abs(ly + 50) <= 1 || abs(ly - (kScreen + 50)) <= 1);
                if (edge) ++vis_mismatch_edge; else ++vis_mismatch_interior;
                continue;
            }
            if (lv) {
                pos.add(lx, nx);
                pos.add(ly, ny);
            }
        }
    }
    printf("[EQUIV] dots position    : %ld coords, %ld differ (%.4f%%), max |d| = %ld px\n",
           pos.n, pos.mismatch, 100.0 * pos.mismatch / (pos.n ? pos.n : 1), pos.max_abs);
    printf("[EQUIV] dots visibility  : %ld pairs, %ld edge flips, %ld interior flips\n",
           vis_n, vis_mismatch_edge, vis_mismatch_interior);
    TEST_ASSERT_LESS_OR_EQUAL(1, pos.max_abs);
    TEST_ASSERT_EQUAL(0, vis_mismatch_interior);
}

// lon == +180.0 exactly at z1: the centre tile must match the legacy clamp
// (x = 1), otherwise the two z1 tile halves swap sides.
void test_projection_antimeridian_z1() {
    static const float kLats[] = {0.0f, 30.0f, -45.0f, 84.9f};
    for (float vlat : kLats) {
        ViewFrame vf;
        view_frame_compute(&vf, vlat, 180.0f, 1);
        const LegacyTile c = legacy_latlon_to_tile(vlat, 180.0f, 1);
        TEST_ASSERT_EQUAL_UINT16(c.x, vf.ctx);
        TEST_ASSERT_EQUAL_UINT16(c.y, vf.cty);
        for (int dx = -1; dx <= 1; ++dx) {
            const LegacyTile tc = {1, (uint16_t)((c.x + dx + 2) % 2), c.y};
            int lx, ly, nx, ny;
            legacy_render_tile_pos(tc, dx, 0, vlat, 180.0f, &lx, &ly);
            new_tile_pos(vf, tc, dx, 0, true, &nx, &ny);
            TEST_ASSERT_INT_WITHIN(1, lx, nx);
            TEST_ASSERT_INT_WITHIN(1, ly, ny);
        }
    }
}

// A dot at a tile's north-west corner must land on that tile's screen corner:
// dots and tiles come from one projection, so they cannot drift apart.
void test_projection_dot_locked_to_tile() {
    for (uint8_t z = 2; z <= 6; ++z) {
        const float vlat = 35.2f, vlon = 139.1f;
        ViewFrame vf;
        view_frame_compute(&vf, vlat, vlon, z);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int max_tile = (1 << z) - 1;
                const LegacyTile tc = {z, (uint16_t)((vf.ctx + dx + max_tile + 1) % (max_tile + 1)),
                                       (uint16_t)(vf.cty + dy)};
                // Exact world coordinate of the tile corner.
                WorldPoint corner = {(uint32_t)tc.x << (32 - z), (uint32_t)tc.y << (31 - z)};
                int16_t px, py;
                view_frame_project(vf, corner, kScreen, &px, &py);
                int tx, ty;
                new_tile_pos(vf, tc, dx, dy, false, &tx, &ty);
                TEST_ASSERT_EQUAL_INT(tx, px);
                TEST_ASSERT_EQUAL_INT(ty, py);
            }
        }
    }
}

// Registered from setup() in test_mercator.cpp (one native test binary).
void run_projection_equivalence_tests() {
    RUN_TEST(test_projection_tile_placement_equivalence);
    RUN_TEST(test_projection_dot_equivalence);
    RUN_TEST(test_projection_antimeridian_z1);
    RUN_TEST(test_projection_dot_locked_to_tile);
}
