#include "ui_quake_detail.h"
#include "quake_loader.h"
#include "map_renderer.h"
#include "theme.h"
#include "ui_helpers.h"
#include "settings_manager.h"
#include <math.h>
#include <time.h>
#include <vector>

// External UI objects from ui_display.cpp
extern lv_obj_t* ui_ScreenMain;

// Custom fonts
LV_FONT_DECLARE(lv_font_montserrat_18);

// Module state
static lv_obj_t* ui_QuakeDetailSheet = NULL;
static lv_obj_t* ui_DetailTitleLabel = NULL;
static lv_obj_t* ui_DetailInfoLabel = NULL;
static lv_obj_t* ui_DetailCloseBtn = NULL;
static int selected_quake_index = -1;

static void ui_create_quake_detail_sheet_if_needed() {
    if (ui_QuakeDetailSheet) return;

    ui_QuakeDetailSheet = lv_obj_create(ui_ScreenMain);
    lv_obj_set_size(ui_QuakeDetailSheet, 360, 220);
    lv_obj_set_style_bg_color(ui_QuakeDetailSheet, theme_bg_card(), 0);
    lv_obj_set_style_border_color(ui_QuakeDetailSheet, theme_primary(), 0);
    lv_obj_set_style_border_width(ui_QuakeDetailSheet, 2, 0);
    lv_obj_set_style_radius(ui_QuakeDetailSheet, 8, 0);
    lv_obj_align(ui_QuakeDetailSheet, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Title label
    ui_DetailTitleLabel = lv_label_create(ui_QuakeDetailSheet);
    lv_obj_set_style_text_color(ui_DetailTitleLabel, theme_text_primary(), 0);
    lv_obj_set_style_text_font(ui_DetailTitleLabel, &lv_font_montserrat_18, 0);
    lv_obj_set_width(ui_DetailTitleLabel, 340);
    lv_label_set_long_mode(ui_DetailTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui_DetailTitleLabel, LV_ALIGN_TOP_LEFT, 10, 10);

    // Info label
    ui_DetailInfoLabel = lv_label_create(ui_QuakeDetailSheet);
    lv_obj_set_style_text_color(ui_DetailInfoLabel, theme_text_secondary(), 0);
    lv_obj_set_style_text_font(ui_DetailInfoLabel, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(ui_DetailInfoLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui_DetailInfoLabel, 340);
    lv_obj_align(ui_DetailInfoLabel, LV_ALIGN_TOP_LEFT, 10, 35);

    // Close button
    ui_DetailCloseBtn = lv_button_create(ui_QuakeDetailSheet);
    lv_obj_set_size(ui_DetailCloseBtn, 100, 44);
    lv_obj_align(ui_DetailCloseBtn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_shadow_width(ui_DetailCloseBtn, 0, 0);  // OLED-optimized: no shadow
    lv_obj_set_style_bg_color(ui_DetailCloseBtn, theme_bg_elevated(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(ui_DetailCloseBtn, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(ui_DetailCloseBtn, theme_border_default(), LV_STATE_DISABLED);
    lv_obj_t* close_label = lv_label_create(ui_DetailCloseBtn);
    lv_label_set_text(close_label, "Close");
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_18, 0);
    lv_obj_center(close_label);

    lv_obj_add_event_cb(ui_DetailCloseBtn, [](lv_event_t* e) {
        ui_close_quake_detail_sheet();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(ui_QuakeDetailSheet, LV_OBJ_FLAG_HIDDEN);
}

// --- Public API Implementation ---

int ui_find_nearest_quake(int16_t tap_x, int16_t tap_y) {
    const Earthquake* quakes = nullptr;
    size_t quake_count = 0;
    if (!quake_lock_read(&quakes, &quake_count)) {
        return -1;
    }
    const AppSettings* settings = settings_get();
    const float mag_min = settings ? settings->mag_min : 0.0f;
    int nearest_idx = -1;
    float min_dist = 38.0f;  // Max tap distance threshold (38px for comfortable finger acquisition)

    for (size_t i = 0; i < quake_count; i++) {
        if (quakes[i].mag < mag_min) {
            continue;
        }

        int16_t dot_x, dot_y;
        if (!map_latlon_to_pixel(quakes[i].lat, quakes[i].lon, &dot_x, &dot_y) ||
            !map_dot_on_screen(dot_x, dot_y, quakes[i].mag)) {
            continue;  // Not drawn, so not tappable
        }

        // Calculate distance
        float dx = tap_x - dot_x;
        float dy = tap_y - dot_y;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist < min_dist) {
            min_dist = dist;
            nearest_idx = static_cast<int>(i);
        }
    }

    quake_unlock_read();
    return nearest_idx;
}

void ui_show_quake_detail_sheet(int quake_idx) {
    if (quake_idx < 0) return;
    Earthquake quake = {};
    if (!quake_read_item(static_cast<size_t>(quake_idx), quake)) return;

    const AppSettings* settings = settings_get();
    const float mag_min = settings ? settings->mag_min : 0.0f;
    if (quake.mag < mag_min) return;

    ui_create_quake_detail_sheet_if_needed();

    char title[160];
    snprintf(title, sizeof(title), "M%.1f - %s", quake.mag, quake.place);
    lv_label_set_text(ui_DetailTitleLabel, title);
    lv_obj_invalidate(ui_DetailTitleLabel);

    char time_ago[24];
    ui_format_time_ago(quake.time, time_ago, sizeof(time_ago));

    char info[128];
    snprintf(info, sizeof(info),
             "Depth: %.1f km\nTime: %s\nLat: %.2f, Lon: %.2f",
             quake.depth, time_ago, quake.lat, quake.lon);
    lv_label_set_text(ui_DetailInfoLabel, info);
    lv_obj_invalidate(ui_DetailInfoLabel);

    lv_obj_remove_flag(ui_QuakeDetailSheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(ui_QuakeDetailSheet, -1);
    selected_quake_index = quake_idx;
}

void ui_close_quake_detail_sheet() {
    if (ui_QuakeDetailSheet) {
        lv_obj_add_flag(ui_QuakeDetailSheet, LV_OBJ_FLAG_HIDDEN);
    }
    selected_quake_index = -1;
}

bool ui_quake_detail_is_open() {
    return ui_QuakeDetailSheet != NULL && !lv_obj_has_flag(ui_QuakeDetailSheet, LV_OBJ_FLAG_HIDDEN);
}
