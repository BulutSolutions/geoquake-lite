#include "map_renderer.h"
#include "crash_trace.h"
#include "config.h"
#include "board_profile.h"
#include "config_constants.h"
#include "quake_loader.h"
#include "tile_manager.h"
#include "sd_card.h"
#include "settings_manager.h"
#include "map_gestures.h"
#include "map_tiles.h"
#include "map_mercator.h"
#include "map_projection.h"
#include "amoled_ui_tokens.h"
#include "perf_probe.h"
#include "ui_power.h"
#include "hande_time.h"
#include <Arduino.h>
#include <vector>
#include <math.h>

LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(font_montserrat_18_latin);  // Montserrat 18 + Latin-1 Supplement + Latin Extended-A
LV_FONT_DECLARE(font_montserrat_24_latin);  // Montserrat 24 + Latin-1 Supplement + Latin Extended-A

// Include map data (stored in flash)
#include "map_data/map_types.h"
#include "map_data/map_coastlines.h"
#include "map_data/map_countries.h"
#include "map_data/map_states.h"
#include "map_data/map_city_labels.h"
#include "map_data/map_country_labels.h"

// External UI objects for coordinate synchronization
extern lv_obj_t *ui_OverlayPanel;  // HUD overlay from ui_display.cpp

// --- CONFIGURATION ---
// Canvas dimensions come from the runtime board profile.
#define MAP_CANVAS_WIDTH  g_board.screen_size
#define MAP_CANVAS_HEIGHT g_board.screen_size

// --- STATE ---
static lv_obj_t* map_canvas = NULL;
static uint16_t* canvas_buffer = NULL;
static lv_obj_t* map_parent = NULL;

// Current view - Default: Center of z1 4-tile grid (Prime Meridian at equator)
float view_center_lat = 0.0f;
float view_center_lon = 0.0f;
float view_zoom = 1.0f;
// BASE_ZOOM now defined in config_constants.h (value: 2.0f)

// Cached view (to detect changes)
static float cached_center_lat = -999.0f;
static float cached_center_lon = -999.0f;
static float cached_zoom = -1.0f;

// Touch state
static int16_t touch_start_x = 0;
static int16_t touch_start_y = 0;
static float touch_start_lat = 0.0f;
static float touch_start_lon = 0.0f;
bool is_panning = false;

// Pinch state
float pinch_start_distance = 0.0f;
static float pinch_start_zoom = 1.0f;
static float pinch_anchor_lat = 0.0f;
static float pinch_anchor_lon = 0.0f;
static unsigned long last_zoom_change_time = 0;
// ZOOM_COOLDOWN_MS now defined in config_constants.h

// Layer visibility
static bool layer_visible[4] = { true, true, true, true };

// --- LAYER VISIBILITY ---

// Colors (RGB565 format)
static lv_color_t color_ocean = lv_color_hex(0x000000);   // True black
static lv_color_t color_land = lv_color_hex(0x16213e);    // Slightly lighter
static lv_color_t color_border = lv_color_hex(0x0f3460);  // Blue border
static lv_color_t color_city = lv_color_hex(0xe94560);    // Reddish for cities
static lv_color_t color_state = lv_color_hex(0x533483);   // Purple for states

// Earthquake dots - using object pooling for performance (20-30ms savings per frame)
static std::vector<lv_obj_t*> quake_dots;

// === DOT POOL OPTIMIZATION ===
// Pre-allocate a pool of dot objects to avoid LVGL create/delete overhead
// DOT_POOL_SIZE defined in config_constants.h
static lv_obj_t* dot_pool[DOT_POOL_SIZE];
static bool dot_pool_initialized = false;
static int dot_pool_active_count = 0;  // Number of dots currently visible
static lv_obj_t* dot_pool_parent = NULL;  // Parent for pool (set on first use)

static void init_dot_pool(lv_obj_t* parent);
static_assert(DOT_POOL_SIZE >= MAX_STORED_QUAKES,
              "vector dot pool must fit every quake: the tap hit-test treats every on-screen quake as drawn");
static lv_obj_t* get_pooled_dot(int index, lv_obj_t* parent);
static void hide_all_pooled_dots();

static void dots_layer_rebuild();
static void dots_layer_set_visible(bool visible);
static void dots_draw_post_cb(lv_event_t* e);
static bool ensure_vector_canvas(void);
static void release_vector_canvas(void);

// Tile mode state (defined in map_tiles.cpp)
extern bool use_tile_mode;
extern bool tiles_available;

// LVGL gesture-based panning - uses LVGL's built-in gesture filtering
// This works with noisy touch data because LVGL filters internally
static const float GESTURE_PAN_AMOUNT = 50.0f;  // Pixels to pan per gesture event

// Pan debug logging removed for performance

// State now in map_tiles.cpp, accessed via externs in map_tiles.h
// - current_z: extern in map_tiles.h
// - last_center_tile: access via map_tiles_get_last_center_tile()

// --- COORDINATE CONVERSION ---

static float map_effective_zoom() {
    return view_zoom * BASE_ZOOM;
}

// Web Mercator: lat_to_mercator_y / mercator_y_to_lat come from map_mercator.h
// (single float implementation shared with the tile system).

// === Z1 VERTICAL PAN CLAMP ===
// At z1, the map is 512px tall (2 tiles × 256px). The tile helper applies
// the correct clamp for the runtime board dimensions so no black gaps appear.
void apply_z1_vertical_clamp() {
    map_tiles_apply_z1_clamp(&view_center_lat);
}

// Convert lat/lon to screen coordinates using Web Mercator (for tile mode).
// Same integer projection as tile placement (map_projection.h), so dots stay
// locked to tiles; one tanf+logf per call instead of ~10 trig calls.
static bool latlon_to_screen_mercator(float lat, float lon, int16_t* sx, int16_t* sy) {
    return view_frame_project(view_frame_get(current_z), world_from_latlon(lat, lon),
                              MAP_CANVAS_WIDTH, sx, sy);
}

// Convert lat/lon (fixed-point * 100) to screen coordinates (for vector mode)
static bool latlon_fp_to_screen(int16_t lat_fp, int16_t lon_fp, int16_t* sx, int16_t* sy) {
    float lat = lat_fp / 100.0f;
    float lon = lon_fp / 100.0f;

    // Apply view transform
    float rel_lon = lon - view_center_lon;
    float rel_lat = lat - view_center_lat;

    // Handle wraparound for longitude
    if (rel_lon > 180.0f) rel_lon -= 360.0f;
    if (rel_lon < -180.0f) rel_lon += 360.0f;

    // Convert to screen space with zoom
    // Redefined Scale: 180 degrees = MAP_CANVAS_WIDTH pixels at Zoom 1.0
    float pixels_per_degree = (MAP_CANVAS_WIDTH / 180.0f) * map_effective_zoom();

    int16_t x = (int16_t)(MAP_CANVAS_WIDTH / 2 + rel_lon * pixels_per_degree);
    int16_t y = (int16_t)(MAP_CANVAS_HEIGHT / 2 - rel_lat * pixels_per_degree);

    *sx = x;
    *sy = y;

    // Return true if point is visible
    return (x >= -50 && x < MAP_CANVAS_WIDTH + 50 &&
            y >= -50 && y < MAP_CANVAS_HEIGHT + 50);
}

bool map_latlon_to_pixel(float lat, float lon, int16_t* x, int16_t* y) {
    // Use Web Mercator projection when in tile mode for correct alignment
    if (use_tile_mode && tiles_available) {
        return latlon_to_screen_mercator(lat, lon, x, y);
    }
    // Use simple linear projection for vector mode
    return latlon_fp_to_screen((int16_t)(lat * 100), (int16_t)(lon * 100), x, y);
}

bool map_pixel_to_latlon(int16_t x, int16_t y, float* lat, float* lon) {
    if (use_tile_mode) {
        // Mercator-based conversion (matches tile math)
        float world_size = map_tiles_get_world_size(current_z);

        // Longitude is linear
        float pixels_per_degree_x = world_size / 360.0f;
        float rel_lon = (x - MAP_CANVAS_WIDTH / 2) / pixels_per_degree_x;
        *lon = view_center_lon + rel_lon;

        // Latitude: convert center to Mercator, add pixel delta, convert back
        float center_y_mercator = lat_to_mercator_y(view_center_lat);
        float dy_mercator = -(y - MAP_CANVAS_HEIGHT / 2) / world_size;
        float point_y_mercator = center_y_mercator + dy_mercator;
        *lat = mercator_y_to_lat(point_y_mercator);
    } else {
        // Vector mode: linear projection
        float pixels_per_degree = (MAP_CANVAS_WIDTH / 180.0f) * map_effective_zoom();
        float rel_lon = (x - MAP_CANVAS_WIDTH / 2) / pixels_per_degree;
        float rel_lat = (MAP_CANVAS_HEIGHT / 2 - y) / pixels_per_degree;
        *lon = view_center_lon + rel_lon;
        *lat = view_center_lat + rel_lat;
    }

    // Clamp latitude to Mercator limit (±85°)
    // Must match map_pan_by() clamping to prevent polar drift
    if (*lat > 85.0f) *lat = 85.0f;
    if (*lat < -85.0f) *lat = -85.0f;

    // Wrap longitude
    while (*lon > 180.0f) *lon -= 360.0f;
    while (*lon < -180.0f) *lon += 360.0f;

    return true;
}

// --- DRAWING PRIMITIVES ---

// Draw a line on the canvas (Bresenham's algorithm)
static void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color_u16) {
    // Clip to canvas bounds
    if ((x0 < 0 && x1 < 0) || (x0 >= MAP_CANVAS_WIDTH && x1 >= MAP_CANVAS_WIDTH)) return;
    if ((y0 < 0 && y1 < 0) || (y0 >= MAP_CANVAS_HEIGHT && y1 >= MAP_CANVAS_HEIGHT)) return;

    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;

    while (true) {
        // Plot pixel if within bounds
        if (x0 >= 0 && x0 < MAP_CANVAS_WIDTH && y0 >= 0 && y0 < MAP_CANVAS_HEIGHT) {
            canvas_buffer[y0 * MAP_CANVAS_WIDTH + x0] = color_u16;
        }

        if (x0 == x1 && y0 == y1) break;

        int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

// Draw a polyline from map data with LOD (Level of Detail) optimization
// At low zoom, skip points to reduce CPU usage (10-15% savings at world zoom)
static void draw_polyline(const MapPolyline* polyline, uint16_t color_u16) {
    if (polyline->point_count < 2) return;

    // LOD: Calculate point skip factor based on zoom level
    // At world zoom (z < 2), draw 25% of points (skip 3 out of 4)
    // At moderate zoom (z < 4), draw 50% of points (skip every other)
    // At high zoom, draw all points
    int lod_skip = 1;  // Default: draw every point
    if (view_zoom < 1.0f) {
        lod_skip = 4;  // 25% of points at world view
    } else if (view_zoom < 2.0f) {
        lod_skip = 2;  // 50% of points at low zoom
    }

    int16_t prev_x, prev_y;
    bool prev_visible = latlon_fp_to_screen(
        polyline->points[0].lat, polyline->points[0].lon,
        &prev_x, &prev_y
    );

    // Use LOD skip for iteration
    for (uint16_t i = lod_skip; i < polyline->point_count; i += lod_skip) {
        int16_t curr_x, curr_y;
        bool curr_visible = latlon_fp_to_screen(
            polyline->points[i].lat, polyline->points[i].lon,
            &curr_x, &curr_y
        );

        // Draw line if either endpoint is potentially visible
        // (allow some margin for lines crossing the viewport)
        if (prev_visible || curr_visible) {
            draw_line(prev_x, prev_y, curr_x, curr_y, color_u16);
        }

        prev_x = curr_x;
        prev_y = curr_y;
        prev_visible = curr_visible;
    }

    // Ensure we always draw to the last point (even if LOD skipped it)
    if (polyline->point_count > 1 && ((polyline->point_count - 1) % lod_skip) != 0) {
        int16_t last_x, last_y;
        bool last_visible = latlon_fp_to_screen(
            polyline->points[polyline->point_count - 1].lat,
            polyline->points[polyline->point_count - 1].lon,
            &last_x, &last_y
        );
        if (prev_visible || last_visible) {
            draw_line(prev_x, prev_y, last_x, last_y, color_u16);
        }
    }
}

// --- RENDERING ---

// Per-polyline bounding boxes (centidegrees), built once per layer on first use
// (PSRAM), let render_layer() skip polylines that cannot reach the screen. The
// test is conservative: a skipped polyline has no point inside
// latlon_fp_to_screen()'s visibility window, and draw_polyline() only draws
// segments with a visible endpoint, so the canvas stays pixel-identical
// (a culled and an unculled render are pixel-identical).
namespace {  // file-local
struct PolylineBox { int16_t lat0, lat1, lon0, lon1; };
enum CullLayer { CULL_COUNTRIES, CULL_COASTLINES, CULL_STATES, CULL_LAYER_COUNT };
}  // namespace
static PolylineBox* s_polyline_boxes[CULL_LAYER_COUNT] = {};
static bool s_vector_culling = true;

static const PolylineBox* polyline_boxes(CullLayer layer, const MapPolyline* polylines, uint16_t count) {
    if (s_polyline_boxes[layer]) return s_polyline_boxes[layer];
    PolylineBox* boxes = static_cast<PolylineBox*>(heap_caps_malloc(count * sizeof(PolylineBox), MALLOC_CAP_SPIRAM));
    if (!boxes) return nullptr;  // no culling: draw everything, as before
    for (uint16_t i = 0; i < count; i++) {
        PolylineBox b = {INT16_MAX, INT16_MIN, INT16_MAX, INT16_MIN};
        for (uint16_t k = 0; k < polylines[i].point_count; k++) {
            const MapPoint& p = polylines[i].points[k];
            if (p.lat < b.lat0) b.lat0 = p.lat;
            if (p.lat > b.lat1) b.lat1 = p.lat;
            if (p.lon < b.lon0) b.lon0 = p.lon;
            if (p.lon > b.lon1) b.lon1 = p.lon;
        }
        boxes[i] = b;
    }
    s_polyline_boxes[layer] = boxes;
    return boxes;
}

// Whether a point inside the box can land in latlon_fp_to_screen()'s window
// [-50, size + 50) on both axes; 2 px of slack covers float rounding and the
// truncating int16 conversion.
static bool polyline_box_may_be_visible(const PolylineBox& b, float pixels_per_degree) {
    if (b.lat0 > b.lat1) return false;  // no points
    const float half_h = (MAP_CANVAS_HEIGHT / 2 + 52) / pixels_per_degree * 100.0f;  // centidegrees
    const float half_w = (MAP_CANVAS_WIDTH / 2 + 52) / pixels_per_degree * 100.0f;
    const float centre_lat = view_center_lat * 100.0f;
    if (b.lat1 < centre_lat - half_h || b.lat0 > centre_lat + half_h) return false;
    if (half_w >= 18000.0f) return true;  // the window spans every longitude
    // latlon_fp_to_screen() wraps each point's longitude offset once into [-180, 180]
    const float lo = b.lon0 - view_center_lon * 100.0f;
    const float hi = b.lon1 - view_center_lon * 100.0f;
    for (int k = -1; k <= 1; k++) {
        const float shift = 36000.0f * k;
        if (hi + shift >= -half_w && lo + shift <= half_w) return true;
    }
    return false;
}

static void render_layer(CullLayer layer, const MapPolyline* polylines, uint16_t count, uint16_t color_u16) {
    const PolylineBox* boxes = s_vector_culling ? polyline_boxes(layer, polylines, count) : nullptr;
    const float pixels_per_degree = (MAP_CANVAS_WIDTH / 180.0f) * map_effective_zoom();  // as latlon_fp_to_screen()
    for (uint16_t i = 0; i < count; i++) {
        if (boxes && !polyline_box_may_be_visible(boxes[i], pixels_per_degree)) continue;
        draw_polyline(&polylines[i], color_u16);
    }
}

// Draw a small circle (city marker)
static void draw_circle(int16_t cx, int16_t cy, int16_t radius, uint16_t color_u16) {
    for (int16_t dy = -radius; dy <= radius; dy++) {
        for (int16_t dx = -radius; dx <= radius; dx++) {
            if (dx*dx + dy*dy <= radius*radius) {
                int16_t px = cx + dx;
                int16_t py = cy + dy;
                if (px >= 0 && px < MAP_CANVAS_WIDTH && py >= 0 && py < MAP_CANVAS_HEIGHT) {
                    canvas_buffer[py * MAP_CANVAS_WIDTH + px] = color_u16;
                }
            }
        }
    }
}

// Render country name labels at low zoom
static void render_country_labels() {
    int rendered = 0;

    // Set up text drawing descriptor for country names
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_hex(AMOLED_SECONDARY_TEXT_HEX);

    for (int i = 0; i < COUNTRY_LABEL_COUNT && rendered < MAX_COUNTRY_LABELS; i++) {
        int16_t sx, sy;
        if (latlon_fp_to_screen(country_labels[i].lat, country_labels[i].lon, &sx, &sy)) {
            if (sx >= 0 && sx < MAP_CANVAS_WIDTH && sy >= 0 && sy < MAP_CANVAS_HEIGHT) {
                label_dsc.font = country_labels[i].is_huge
                                     ? &font_montserrat_24_latin
                                     : &font_montserrat_18_latin;

                lv_point_t text_size;
                lv_txt_get_size(&text_size, country_labels[i].name, label_dsc.font,
                                label_dsc.letter_space, label_dsc.line_space,
                                LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                lv_coord_t draw_width = text_size.x + 2;
                if (draw_width > MAP_CANVAS_WIDTH) draw_width = MAP_CANVAS_WIDTH;
                lv_coord_t draw_x = sx - draw_width / 2;
                if (draw_x < 0) draw_x = 0;
                if (draw_x + draw_width > MAP_CANVAS_WIDTH) {
                    draw_x = MAP_CANVAS_WIDTH - draw_width;
                }
                lv_coord_t draw_y = sy - text_size.y / 2;
                if (draw_y < 0) draw_y = 0;
                if (draw_y + text_size.y > MAP_CANVAS_HEIGHT) {
                    draw_y = MAP_CANVAS_HEIGHT - text_size.y;
                }

                lv_layer_t layer;
                lv_canvas_init_layer(map_canvas, &layer);
                label_dsc.text = country_labels[i].name;
                lv_area_t coords = { (int16_t)draw_x, (int16_t)draw_y, (int16_t)(draw_x + draw_width - 1), (int16_t)(draw_y + text_size.y - 1) };
                lv_draw_label(&layer, &label_dsc, &coords);
                lv_canvas_finish_layer(map_canvas, &layer);
                rendered++;
            }
        }
    }
}

// Render city markers with text labels at high zoom
static void render_city_markers() {
    int rendered = 0;
    lv_color_t city_marker_color = lv_color_hex(0xFFFFFF);  // White dots

    // Set up text drawing descriptor
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_hex(AMOLED_SECONDARY_TEXT_HEX);
    label_dsc.font = &font_montserrat_18_latin;

    for (int i = 0; i < CITY_LABEL_COUNT && rendered < MAX_CITY_LABELS; i++) {
        // Filter by population to avoid clutter
        if (city_labels[i].pop < MIN_CITY_POPULATION) continue;

        int16_t sx, sy;
        if (latlon_fp_to_screen(city_labels[i].lat, city_labels[i].lon, &sx, &sy)) {
            if (sx >= 0 && sx < MAP_CANVAS_WIDTH && sy >= 0 && sy < MAP_CANVAS_HEIGHT) {
                // Draw dot
                const uint16_t city_marker_u16 = lv_color_to_u16(city_marker_color);
                draw_circle(sx, sy, 2, city_marker_u16);

                lv_point_t text_size;
                lv_txt_get_size(&text_size, city_labels[i].name, label_dsc.font,
                                label_dsc.letter_space, label_dsc.line_space,
                                LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                lv_coord_t draw_width = text_size.x + 2;
                if (draw_width > MAP_CANVAS_WIDTH) draw_width = MAP_CANVAS_WIDTH;
                lv_coord_t draw_x = sx + 5;
                if (draw_x + draw_width > MAP_CANVAS_WIDTH) {
                    draw_x = sx - 5 - draw_width;
                }
                if (draw_x < 0) draw_x = 0;
                lv_coord_t draw_y = sy - text_size.y / 2;
                if (draw_y < 0) draw_y = 0;
                if (draw_y + text_size.y > MAP_CANVAS_HEIGHT) {
                    draw_y = MAP_CANVAS_HEIGHT - text_size.y;
                }

                lv_layer_t layer;
                lv_canvas_init_layer(map_canvas, &layer);
                label_dsc.text = city_labels[i].name;
                lv_area_t coords = { (int16_t)draw_x, (int16_t)draw_y, (int16_t)(draw_x + draw_width - 1), (int16_t)(draw_y + text_size.y - 1) };
                lv_draw_label(&layer, &label_dsc, &coords);
                lv_canvas_finish_layer(map_canvas, &layer);

                rendered++;
            }
        }
    }
}

// --- TILE RENDERING ---

static void clear_tile_images() {
    if (tile_pool_initialized) {
        for (int i = 0; i < TILE_POOL_SIZE; i++) {
            lv_obj_add_flag(tile_pool[i], LV_OBJ_FLAG_HIDDEN);
            tile_pool_state[i].in_use = false;
            tile_pool_state[i].claimed = false;
        }
    } else {
        for (auto img : tile_images) {
            if (img) lv_obj_delete(img);
        }
    }
    tile_images.clear();
    tile_paths.clear();
}

// --- VECTOR RENDERING ---

static bool ensure_vector_canvas(void) {
    if (canvas_buffer && map_canvas) return true;
    if (!map_parent) return false;

    const size_t canvas_bytes = MAP_CANVAS_WIDTH * MAP_CANVAS_HEIGHT * sizeof(uint16_t);
    canvas_buffer = (uint16_t*)heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);

    if (!canvas_buffer) {
        Serial.println("[MAP] Failed to allocate canvas buffer!");
        return false;
    }

    Serial.printf("[MAP] Canvas buffer allocated: %d KB\n",
        (int)(canvas_bytes / 1024));

    map_canvas = lv_canvas_create(map_parent);
    static lv_draw_buf_t map_canvas_draw_buf;
    lv_draw_buf_init(&map_canvas_draw_buf, MAP_CANVAS_WIDTH, MAP_CANVAS_HEIGHT,
                     LV_COLOR_FORMAT_RGB565, MAP_CANVAS_WIDTH * sizeof(uint16_t),
                     canvas_buffer, canvas_bytes);
    lv_canvas_set_draw_buf(map_canvas, &map_canvas_draw_buf);
    lv_obj_center(map_canvas);
    return true;
}

static void release_vector_canvas(void) {
    if (map_canvas) {
        lv_obj_delete(map_canvas);
        map_canvas = NULL;
    }
    if (canvas_buffer) {
        heap_caps_free(canvas_buffer);
        canvas_buffer = NULL;
    }
}

static void render_vector() {
    PERF_PROBE_SCOPE(vec, PP_VECTOR);
    if (!ensure_vector_canvas()) return;
    if (!canvas_buffer) return;

    const uint16_t ocean_u16 = lv_color_to_u16(color_ocean);
    const uint16_t border_u16 = lv_color_to_u16(color_border);

    // Clear canvas with ocean color
    const int total_pixels = MAP_CANVAS_WIDTH * MAP_CANVAS_HEIGHT;
    for (int i = 0; i < total_pixels; i++) {
        canvas_buffer[i] = ocean_u16;
    }

    // Draw layers based on zoom level and visibility

    // 1. Always draw Countries/Coastlines
    if (layer_visible[MAP_LAYER_COUNTRIES]) {
        render_layer(CULL_COUNTRIES, map_countries, MAP_COUNTRIES_COUNT, border_u16);
    }

    if (layer_visible[MAP_LAYER_COASTLINES]) {
        render_layer(CULL_COASTLINES, map_coastlines, MAP_COASTLINES_COUNT, border_u16);
    }

    // 2. Sequential LOD Steps:
    float effective_zoom = map_effective_zoom();

    // Zoom 4.0x - 13.9x: Show Country Names
    if (effective_zoom >= LOD_MIN_COUNTRY_NAMES && effective_zoom < LOD_MAX_COUNTRY_NAMES) {
        render_country_labels();
    }

    // Zoom 14.0x - 15.0x: Show State Borders + City Names & Dots
    if (effective_zoom >= 14.0f) {
        if (layer_visible[MAP_LAYER_STATES]) {
            render_layer(CULL_STATES, map_states, MAP_STATES_COUNT, border_u16);
        }
        if (layer_visible[MAP_LAYER_CITIES]) {
            render_city_markers();
        }
    }

    // Update the LVGL canvas
    lv_obj_invalidate(map_canvas);
}

void map_redraw() {
    // Choose rendering mode
    if (use_tile_mode && tiles_available) {
        map_tiles_render();
    } else {
        render_vector();
    }

    // Cache current view
    cached_center_lat = view_center_lat;
    cached_center_lon = view_center_lon;
    cached_zoom = view_zoom;
}

// === DOT POOL IMPLEMENTATION ===
// Initialize the dot pool with pre-allocated invisible dots
static void init_dot_pool(lv_obj_t* parent) {
    if (dot_pool_initialized) return;
    if (!parent) return;

    Serial.printf("[MAP] Initializing dot pool with %d objects\n", DOT_POOL_SIZE);
    unsigned long start = micros();

    dot_pool_parent = parent;

    for (int i = 0; i < DOT_POOL_SIZE; i++) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);

        // Set default style (will be updated when used)
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFDD00), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_50, 0);
        lv_obj_set_style_clip_corner(dot, false, 0);

        // Disable clicking
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

        // Start hidden
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

        dot_pool[i] = dot;
    }

    dot_pool_initialized = true;
    dot_pool_active_count = 0;

    unsigned long elapsed = micros() - start;
    Serial.printf("[MAP] Dot pool initialized in %lu us\n", elapsed);
}

// Get a dot from the pool (initializing pool if needed)
static lv_obj_t* get_pooled_dot(int index, lv_obj_t* parent) {
    if (!dot_pool_initialized) {
        init_dot_pool(parent);
    }

    if (index >= DOT_POOL_SIZE) {
        return NULL;  // Pool exhausted
    }

    return dot_pool[index];
}

// Hide all dots in the pool
static void hide_all_pooled_dots() {
    for (int i = 0; i < DOT_POOL_SIZE; i++) {
        if (dot_pool[i]) {
            lv_obj_add_flag(dot_pool[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    dot_pool_active_count = 0;
}

void map_render_earthquakes() {
    unsigned long start = micros();

    if (use_tile_mode) {
        // Tile mode: dots are drawn in tile_container's post-draw pass (dot layer). The layer starts
        // hidden (s_dots_visible), and only a touch release or zoom used to show it, so the first load
        // and every refresh stayed invisible until the map was touched. Show it here like the vector
        // path shows its dots, except mid-gesture (the gesture end shows it via map_process_pending_dots()).
        dots_layer_rebuild();
        if (!map_is_panning() && !map_is_pinching()) dots_layer_set_visible(true);
        return;
    }

    // Vector mode: pooled dot objects above the vector canvas
    lv_obj_t* parent = lv_obj_get_parent(map_canvas);

    // Initialize dot pool if needed (one-time cost)
    if (!dot_pool_initialized && parent) {
        init_dot_pool(parent);
    }

    // Hide all dots first (fast operation)
    hide_all_pooled_dots();

    const Earthquake* quakes = nullptr;
    size_t quake_count = 0;
    if (!quake_lock_read(&quakes, &quake_count)) {
        return;
    }

    // Get magnitude filter from user settings
    AppSettings* settings = settings_get();
    float mag_min = settings ? settings->mag_min : 0.0f;

    int dot_index = 0;
    for (int i = (int)quake_count - 1; i >= 0; --i) {
        const Earthquake& q = quakes[i];
        // Filter by magnitude based on user settings
        if (q.mag < mag_min) {
            continue;
        }

        // Pool exhausted
        if (dot_index >= DOT_POOL_SIZE) {
            break;
        }

        int16_t x, y;
        bool visible = map_latlon_to_pixel(q.lat, q.lon, &x, &y);

        // Check if on-screen
        bool on_screen = visible &&
                        (x >= 0 && x < MAP_CANVAS_WIDTH &&
                         y >= 0 && y < MAP_CANVAS_HEIGHT);

        // Skip off-screen dots entirely (no need to update hidden dots)
        if (!on_screen) {
            continue;
        }

        // Get dot from pool
        lv_obj_t* dot = get_pooled_dot(dot_index, parent);
        if (!dot) continue;

        // Fixed size based on magnitude only (no zoom scaling)
        int size;
        if (q.mag >= 6.0f) {
            size = 12;  // Major earthquakes
        } else if (q.mag >= 5.0f) {
            size = 10;  // Strong earthquakes
        } else if (q.mag >= 4.0f) {
            size = 8;   // Moderate earthquakes
        } else {
            size = 6;   // Light earthquakes (3.0-3.9)
        }

        lv_obj_set_size(dot, size, size);

        // Position relative to the vector canvas
        lv_coord_t canvas_x = lv_obj_get_x(map_canvas);
        lv_coord_t canvas_y = lv_obj_get_y(map_canvas);
        lv_obj_set_pos(dot, canvas_x + x - size/2, canvas_y + y - size/2);

        // Color based on magnitude
        lv_color_t dot_color;
        if (q.mag >= 6.0f) {
            dot_color = lv_color_hex(0xFF0000);  // Red for major
        } else if (q.mag >= 5.0f) {
            dot_color = lv_color_hex(0xFF6600);  // Orange for strong
        } else if (q.mag >= 4.0f) {
            dot_color = lv_color_hex(0xFFAA00);  // Yellow-orange for moderate
        } else {
            dot_color = lv_color_hex(0xFFDD00);  // Yellow for light
        }

        lv_obj_set_style_bg_color(dot, dot_color, 0);

        // Show the dot
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);

        dot_index++;
    }

    quake_unlock_read();
    dot_pool_active_count = dot_index;

    unsigned long elapsed = micros() - start;
    if (elapsed > 5000) {  // Only log if >5ms
        Serial.printf("[MAP] Render %d dots in %lu us\n", dot_index, elapsed);
    }
}

// Hide/show earthquake dots during interaction
void hide_earthquake_dots() {
    if (use_tile_mode) {
        dots_layer_set_visible(false);
        return;
    }
    // Use dot pool - hide only active dots for efficiency
    for (int i = 0; i < dot_pool_active_count && i < DOT_POOL_SIZE; i++) {
        if (dot_pool[i]) lv_obj_add_flag(dot_pool[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_earthquake_dots() {
    if (use_tile_mode) {
        dots_layer_set_visible(true);
        return;
    }
    // Use dot pool - show only active dots
    for (int i = 0; i < dot_pool_active_count && i < DOT_POOL_SIZE; i++) {
        if (dot_pool[i]) lv_obj_remove_flag(dot_pool[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Debounce for showing dots after interaction
unsigned long dots_show_scheduled = 0;
static bool dots_show_pending = false;

// FIX #5: Adaptive dots delay - reduced from 500ms to 200ms for faster feedback
// Can be further reduced when pan velocity is low (no momentum)
#define DOTS_SHOW_DELAY_MS 200

// --- DOT LAYER (tile mode) ---
// Dots are drawn by tile_container's LV_EVENT_DRAW_POST handler: after the
// tiles of every refreshed strip, straight into LVGL's draw buffer. This
// replaces a full-screen ARGB8888 canvas (921,600 B of PSRAM, cleared on each
// dot update and alpha-blended over the whole map on every refresh).
//
// LVGL task only: the map code writes this state, the draw callback reads it.
// The callback takes no lock, prints nothing and never invalidates.

// UI-task-owned copy of the hot quake records. Refreshed only when the loader's
// version changes, with quake_mutex held just for that copy, so drawing never
// blocks the fetch task (which previously waited out the whole LVGL draw).
static HotQuake s_dot_quakes[MAX_STORED_QUAKES];
static size_t s_dot_quake_count = 0;
static uint32_t s_dot_quake_version = UINT32_MAX;

// One drawable dot, in draw order (oldest first: fresh and major quakes on top)
struct DotSpec {
    uint32_t wx, wy;       // world position (HotQuake)
    int16_t x, y;          // centre in tile_container for s_dots_frame; x == kDotOff: not drawn
    lv_color_t fill;
    lv_color_t border;
    lv_opa_t border_opa;
    uint8_t border_w;
    uint8_t size;
    bool ring;             // radar ring around the newest M4+ quake under 6 h old
};
static constexpr int16_t kDotOff = INT16_MIN;
static constexpr int kRingExtra = 10;  // ring diameter = dot size + 10
static DotSpec s_dots[MAX_STORED_QUAKES];
static size_t s_dot_count = 0;
static ViewFrame s_dots_frame;         // frame the x/y above were projected with
static bool s_dots_placed = false;
static bool s_dots_visible = false;

// Dot diameter by magnitude in tile mode (dot layer and map_dot_on_screen()).
static uint8_t tile_dot_size(float mag) {
    if (mag >= 6.0f) return 14;  // Major earthquakes
    if (mag >= 5.0f) return 11;  // Strong earthquakes
    if (mag >= 4.0f) return 9;   // Moderate earthquakes
    return 6;                    // Light earthquakes (3.0-3.9)
}

// Any part of a size x size dot centred at (x, y) lies on the canvas.
static inline bool dot_bounds_on_canvas(int x, int y, int size) {
    const int x1 = x - size / 2;
    const int y1 = y - size / 2;
    return x1 + size > 0 && x1 < MAP_CANVAS_WIDTH && y1 + size > 0 && y1 < MAP_CANVAS_HEIGHT;
}

// Whether the dot at screen (x, y) is drawn, by the same rule as the drawing
// code: tile mode draws dots partly on screen, the vector dot pool needs the centre on it.
bool map_dot_on_screen(int16_t x, int16_t y, float mag) {
    if (use_tile_mode) return dot_bounds_on_canvas(x, y, tile_dot_size(mag));
    return x >= 0 && x < MAP_CANVAS_WIDTH && y >= 0 && y < MAP_CANVAS_HEIGHT;
}

static void dots_layer_set_visible(bool visible) {
    if (s_dots_visible == visible) return;
    s_dots_visible = visible;
    if (tile_container) lv_obj_invalidate(tile_container);
}

// Snapshot + per-dot style. Positions are projected lazily by the draw
// callback, with the frame the tiles were laid out with.
static void dots_layer_rebuild() {
    PERF_PROBE_SCOPE(dots, PP_DOTS);
    // Lock busy (fetch writing): keep the previous snapshot.
    quake_snapshot_hot(s_dot_quakes, MAX_STORED_QUAKES, &s_dot_quake_count, &s_dot_quake_version);

    // Get magnitude filter from user settings (mirrors dot pool path)
    AppSettings* settings = settings_get();
    const float mag_min = settings ? settings->mag_min : 0.0f;  // 0.0 = show all if settings unavailable
    const time_t now = time(nullptr);
    const bool clock_ok = hande_time_valid();

    size_t n = 0;
    for (int i = (int)s_dot_quake_count - 1; i >= 0; --i) {
        const HotQuake& q = s_dot_quakes[i];
        // Filter by magnitude based on user settings
        if (q.mag < mag_min) {
            continue;
        }
        DotSpec& d = s_dots[n++];
        d.wx = q.wx;
        d.wy = q.wy;
        d.x = kDotOff;
        d.y = 0;

        // Size and colour by magnitude
        d.size = tile_dot_size(q.mag);
        if (q.mag >= 6.0f) {
            d.fill = lv_color_hex(0xFF1E27);
        } else if (q.mag >= 5.0f) {
            d.fill = lv_color_hex(0xFF6B00);
        } else if (q.mag >= 4.0f) {
            d.fill = lv_color_hex(0xFFA500);
        } else {
            d.fill = lv_color_hex(0xFFD600);
        }

        // Anti-halation high-contrast border (AMOLED Performance Standard)
        d.border_w = 1;
        d.border = lv_color_hex(0xFFFFFF);
        d.border_opa = LV_OPA_70;

        // Freshness indicator: quakes under 2 hours old receive cyan aura
        long age_sec = (clock_ok && q.time_s > 1577836800u) ? (now - (time_t)q.time_s) : 999999;
        if (age_sec >= 0 && age_sec < 7200) {
            d.border = lv_color_hex(0x06B6D4);
            d.border_opa = LV_OPA_COVER;
            d.border_w = 2;
        }

        // The single most recent major earthquake (i == 0, M >= 4.0, < 6 hours old) gets a radar ring
        d.ring = (i == 0 && q.mag >= 4.0f && age_sec >= 0 && age_sec < 21600);
    }
    s_dot_count = n;
    s_dots_placed = false;

    Serial.printf("[MAP] Dot layer: %u quakes (mag >= %.1f)\n", static_cast<unsigned>(n), mag_min);
    if (s_dots_visible && tile_container) lv_obj_invalidate(tile_container);
}

// Project every dot with the tile layout frame (integer only, map_projection.h)
static void dots_layer_place(const ViewFrame& vf) {
    PERF_PROBE_SCOPE(dproj, PP_DOTS_PROJ);
    for (size_t i = 0; i < s_dot_count; ++i) {
        DotSpec& d = s_dots[i];
        int16_t x, y;
        if (view_frame_project(vf, WorldPoint{d.wx, d.wy}, MAP_CANVAS_WIDTH, &x, &y) &&
            dot_bounds_on_canvas(x, y, d.size)) {
            d.x = x;
            d.y = y;
        } else {
            d.x = kDotOff;
        }
    }
    s_dots_frame = vf;
    s_dots_placed = true;
}

static inline bool areas_overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

// LV_EVENT_DRAW_POST on tile_container: runs once per refreshed strip, after
// the tiles. Only dots overlapping the strip are submitted (LVGL allocates a
// draw task per lv_draw_rect and would only discard it at dispatch).
static void dots_draw_post_cb(lv_event_t* e) {
    CrashTraceScope trace(LVGL_PHASE_MAP_DOTS);
    if (!s_dots_visible || s_dot_count == 0) return;
    const ViewFrame* vf = map_tiles_layout_frame();
    if (!vf) return;
    if (!s_dots_placed || !view_frame_equal(*vf, s_dots_frame)) {
        dots_layer_place(*vf);
    }

    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    // Tiles are positioned relative to the container, which has no padding or
    // border (lv_obj_remove_style_all), so its coords are the dot origin too.
    lv_area_t origin;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_current_target(e)), &origin);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;

    for (size_t i = 0; i < s_dot_count; ++i) {
        const DotSpec& d = s_dots[i];
        if (d.x == kDotOff) continue;
        const int32_t cx = origin.x1 + d.x;
        const int32_t cy = origin.y1 + d.y;

        lv_area_t a;
        a.x1 = cx - d.size / 2;
        a.y1 = cy - d.size / 2;
        a.x2 = a.x1 + d.size - 1;
        a.y2 = a.y1 + d.size - 1;
        if (areas_overlap(a, clip)) {
            dsc.bg_color = d.fill;
            dsc.border_color = d.border;
            dsc.border_opa = d.border_opa;
            dsc.border_width = d.border_w;
            lv_draw_rect(layer, &dsc, &a);
        }

        if (d.ring) {
            const int ring = d.size + kRingExtra;
            lv_area_t r;
            r.x1 = cx - ring / 2;
            r.y1 = cy - ring / 2;
            r.x2 = r.x1 + ring - 1;
            r.y2 = r.y1 + ring - 1;
            if (areas_overlap(r, clip)) {
                lv_draw_rect_dsc_t ring_dsc;
                lv_draw_rect_dsc_init(&ring_dsc);
                ring_dsc.bg_opa = LV_OPA_TRANSP;
                ring_dsc.radius = LV_RADIUS_CIRCLE;
                ring_dsc.border_width = 2;
                ring_dsc.border_color = lv_color_hex(0x06B6D4);
                ring_dsc.border_opa = LV_OPA_80;
                lv_draw_rect(layer, &ring_dsc, &r);
            }
        }
    }
}

// Full recalculation path: used for zoom changes and touch release
// This function handles both rendering AND visibility, so callers
// don't need to call show_earthquake_dots() separately
void map_update_earthquake_positions() {
    if (use_tile_mode) {
        dots_layer_rebuild();
        dots_layer_set_visible(true);
        return;
    }

    // Using dot pool - map_render_earthquakes() handles both
    // rendering and visibility (shows dots as it positions them)
    map_render_earthquakes();
}

// --- INITIALIZATION ---

void map_renderer_init(lv_obj_t* parent) {
    map_parent = parent;

    // Initialize tile module (creates lightweight container and detects SD tiles)
    map_tiles_init(parent);
    // Earthquake dots: drawn over the tiles in the container's post-draw pass
    lv_obj_add_event_cb(tile_container, dots_draw_post_cb, LV_EVENT_DRAW_POST, nullptr);

    // Check if tiles are available after initialization
    if (tiles_available) {
        Serial.printf("[MAP] %s tiles detected - tile mode available\n",
                      tile_manager_using_flash_fallback() ? "Embedded flash fallback" : "SD card");
        use_tile_mode = true;
        lv_obj_remove_flag(tile_container, LV_OBJ_FLAG_HIDDEN);
        map_tiles_ensure_resources();
        // Protect initial zoom level from eviction
        tile_cache_set_priority(current_z, true);
        // Preload initial tiles
        tile_cache_preload(current_z, view_center_lat, view_center_lon);
    } else {
        Serial.println("[MAP] No SD or embedded flash tiles - using vector mode");
        ensure_vector_canvas();
    }

    // Initial render
    map_reset_view();
    map_redraw();

    // Pan buttons are created in ui_button_dock.cpp
    Serial.println("[MAP] Map renderer initialized");
}

// --- VIEW CONTROL ---

void map_set_view(float center_lat, float center_lon, float zoom) {
    // Clamp values
    if (center_lat > 85.0f) center_lat = 85.0f;
    if (center_lat < -85.0f) center_lat = -85.0f;
    while (center_lon > 180.0f) center_lon -= 360.0f;
    while (center_lon < -180.0f) center_lon += 360.0f;
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > MAX_ZOOM_LEVEL) zoom = MAX_ZOOM_LEVEL;

    view_center_lat = center_lat;
    view_center_lon = center_lon;
    view_zoom = zoom;

    // Apply Z1 vertical clamp (prevents blank space at tile edges)
    apply_z1_vertical_clamp();

    // Preload tiles at new view location (Issue #1 fix)
    if (tiles_available && use_tile_mode) {
        if (map_tiles_ensure_resources()) {
            tile_cache_preload(current_z, view_center_lat, view_center_lon);
        }
    }
}

float map_get_zoom() { return view_zoom; }
float map_get_center_lat() { return view_center_lat; }
float map_get_center_lon() { return view_center_lon; }

void map_zoom_by(float factor) {
    float new_zoom = view_zoom * factor;
    if (new_zoom < 1.0f) new_zoom = 1.0f;
    if (new_zoom > MAX_ZOOM_LEVEL) new_zoom = MAX_ZOOM_LEVEL;
    view_zoom = new_zoom;
}

// =============================================================================
// COORDINATE SYSTEM REFERENCE
// =============================================================================
// SCREEN COORDINATES:
//   - Origin (0,0) at top-left corner
//   - X increases rightward across MAP_CANVAS_WIDTH
//   - Y increases downward across MAP_CANVAS_HEIGHT
//
// GEOGRAPHIC COORDINATES:
//   - Latitude: -90 (South Pole) → +90 (North Pole)
//   - Longitude: -180 (West) → +180 (East)
//
// Y-AXIS INVERSION:
//   - Screen Y and Latitude have OPPOSITE directions
//   - Dragging down (dy > 0) moves the map down with the finger: the view centre moves north (latitude increases)
//   - Solution: Add dy when updating latitude (lat += dy); the tile path does the same in Mercator y
// =============================================================================

void map_pan_by(int16_t dx, int16_t dy) {
    PERF_PROBE_BEGIN(pan);
    if (is_panning) ui_hud_drag_begin();  // the HUD steps aside until the finger lifts
    // Apply pan based on current mode
    if (use_tile_mode) {
        float world_size = map_tiles_get_world_size(current_z);  // Cached to avoid powf() per frame
        float pixels_per_degree_x = world_size / 360.0f;
        view_center_lon -= (float)dx / pixels_per_degree_x;

        float center_y_mercator = lat_to_mercator_y(view_center_lat);
        float dy_mercator = (float)dy / world_size;
        view_center_lat = mercator_y_to_lat(center_y_mercator - dy_mercator);

        // Apply Z1 vertical clamp (uses helper for consistency with pinch/set_view)
        apply_z1_vertical_clamp();

        // Z1: Force full re-render (smooth pan disabled - ghost tiles need recalc)
        if (current_z == 1) {
            pan_cache.valid = false;
        }
    } else {
        float pixels_per_degree = (MAP_CANVAS_WIDTH / 180.0f) * map_effective_zoom();
        view_center_lon -= dx / pixels_per_degree;
        view_center_lat += dy / pixels_per_degree;  // finger down (dy > 0) shows land further north, like the tile path
    }

    // Clamp and wrap
    if (view_center_lat > 85.0f) view_center_lat = 85.0f;
    if (view_center_lat < -85.0f) view_center_lat = -85.0f;
    while (view_center_lon > 180.0f) view_center_lon -= 360.0f;
    while (view_center_lon < -180.0f) view_center_lon += 360.0f;

    // Update tile positions if in tile mode (skip Z1 - use full re-render for ghost tiles)
    if (use_tile_mode && rendered_tile_count > 0 && current_z > 1) {
        int16_t tile_dx = 0, tile_dy = 0;
        map_tiles_update_positions(&tile_dx, &tile_dy);

        const ViewFrame& vf_now = view_frame_get(current_z);  // cached: same frame the tiles used
        TileCoord new_center;
        new_center.z = vf_now.z;
        new_center.x = vf_now.ctx;
        new_center.y = vf_now.cty;

        // Prefetch at most every 50 ms or when the centre tile changes
        static unsigned long last_preload_time = 0;
        unsigned long now = millis();

        TileCoord* last_center = map_tiles_get_last_center_tile();
        bool tile_changed = (new_center.x != last_center->x || new_center.y != last_center->y);
        bool time_elapsed = (now - last_preload_time >= 50);

        if (tile_changed || time_elapsed) {
            // Direction of view travel in tile coordinates, from this drag step.
            // (The centre tile rarely changes between 50 ms calls, so deriving it
            // from tile deltas gave (0,0) for any drag inside a tile.) Finger right
            // (dx > 0) moves the view west: tiles with smaller x scroll into view.
            int8_t dir_x = (dx > 0) ? -1 : (dx < 0) ? 1 : 0;
            int8_t dir_y = (dy > 0) ? -1 : (dy < 0) ? 1 : 0;

            tile_cache_preload_direction(new_center, dir_x, dir_y);
            last_preload_time = now;
        }

        // Force visual refresh
        if (tile_container) {
            lv_obj_invalidate(tile_container);
        }
    } else if (use_tile_mode && current_z == 1) {
        // Z1 tile mode: full re-render for ghost tiles
        map_tiles_render();
    } else {
        // Vector mode - force canvas redraw
        map_redraw();
    }

    // Hide/show earthquake dots
    hide_earthquake_dots();
    dots_show_scheduled = millis() + DOTS_SHOW_DELAY_MS;
    PERF_PROBE_END(pan, PP_PAN_BY);
}

void map_reset_view() {
    // Reset to default base level: Center of z1 4-tile grid (1.0x user zoom)
    view_center_lat = 0.0f;
    view_center_lon = 0.0f;
    view_zoom = 1.0f;
    current_z = 1;     // Reset discrete zoom to z1

    // Preload tiles at reset location
    if (tiles_available && use_tile_mode) {
        if (map_tiles_ensure_resources()) {
            tile_cache_set_priority(current_z, true);  // Protect default zoom
            tile_cache_preload(current_z, view_center_lat, view_center_lon);
        }
    }
}


// --- GESTURE HANDLING ---
// Gesture handling is now in map_gestures module
// The state variables below are accessed by both modules

// --- LAYER VISIBILITY ---

void map_set_layer_visible(int layer, bool visible) {
    if (layer >= 0 && layer < 4) {
        layer_visible[layer] = visible;
    }
}

// --- COLORS ---

void map_set_ocean_color(uint32_t color) { color_ocean = lv_color_hex(color); }
void map_set_land_color(uint32_t color) { color_land = lv_color_hex(color); }
void map_set_border_color(uint32_t color) { color_border = lv_color_hex(color); }
void map_set_city_color(uint32_t color) { color_city = lv_color_hex(color); }

// render_new_tiles_only() now implemented in map_tiles.cpp as map_tiles_render_new_only()

// --- STATUS ---

bool map_needs_redraw() {
    // Check debounced dot show (called regularly from main loop)
    if (dots_show_scheduled > 0 && millis() >= dots_show_scheduled) {
        dots_show_scheduled = 0;
        dots_show_pending = true;
    }

    // Check if new tiles arrived from background loader
    if (tile_cache_was_updated()) {
        // In tile mode: insert newly-available tiles incrementally to avoid flash
        // (full redraw deletes+recreates ALL LVGL objects, causing visible flicker)
        if (use_tile_mode && rendered_tile_count > 0) {
            map_tiles_render_new_only();
            return false;  // Don't trigger full redraw
        }
        return true;
    }

    // === SMOOTH PAN: Don't trigger full redraw during tile panning ===
    // The fast path (update_tile_positions) handles movement
    if (use_tile_mode && is_panning && rendered_tile_count > 0) {
        return false;  // Fast path handles it
    }

    const float EPSILON = 0.001f;
    return (fabsf(view_center_lat - cached_center_lat) > EPSILON ||
            fabsf(view_center_lon - cached_center_lon) > EPSILON ||
            fabsf(view_zoom - cached_zoom) > EPSILON);
}

void map_process_pending_dots() {
    if (!dots_show_pending) return;
    if (map_is_panning() || map_is_pinching()) return;
    map_update_earthquake_positions();
    // Note: show_earthquake_dots() removed - map_update_earthquake_positions()
    // now handles visibility internally for both dot pool and canvas modes
    dots_show_pending = false;
}

lv_obj_t* map_get_canvas() {
    return map_canvas ? map_canvas : tile_container;
}

// --- TILE MODE ---

void map_set_tile_mode(bool enabled) {
    if (enabled == use_tile_mode) return;

    bool next_tile_mode = enabled && tiles_available;
    use_tile_mode = next_tile_mode;

    if (use_tile_mode) {
        release_vector_canvas();
        if (!map_tiles_ensure_resources()) {
            use_tile_mode = false;
            ensure_vector_canvas();
            map_redraw();
            return;
        }
        // Show tile container, hide canvas
        lv_obj_remove_flag(tile_container, LV_OBJ_FLAG_HIDDEN);
        if (map_canvas) lv_obj_add_flag(map_canvas, LV_OBJ_FLAG_HIDDEN);
        Serial.println("[MAP] Switched to tile mode");
    } else {
        map_tiles_release_resources();
        ensure_vector_canvas();
        // Show canvas, hide tile container
        if (tile_container) lv_obj_add_flag(tile_container, LV_OBJ_FLAG_HIDDEN);
        if (map_canvas) lv_obj_remove_flag(map_canvas, LV_OBJ_FLAG_HIDDEN);
        clear_tile_images();
        Serial.println("[MAP] Switched to vector mode");
    }

    // Force redraw with new mode
    map_redraw();
}

bool map_is_tile_mode() {
    return use_tile_mode;
}
