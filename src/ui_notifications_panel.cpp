#include "ui_notifications_panel.h"
#include "ui_button_dock.h"
#include "quake_loader.h"
#include "map_renderer.h"
#include "map_tiles.h"
#include "theme.h"
#include "icons/icons.h"
#include "config.h"
#include "board_profile.h"
#include "ui_helpers.h"
#include "settings_manager.h"
#include <algorithm>
#include <time.h>
#include <vector>

// External UI objects from ui_display.cpp
extern lv_obj_t* ui_ScreenMain;
extern lv_obj_t* ui_LblTime;
extern lv_obj_t* ui_LblDate;
extern lv_obj_t* ui_LblStatus;
extern lv_obj_t* ui_BatteryContainer;
extern lv_obj_t* ui_LblZoom;

// Custom fonts
LV_FONT_DECLARE(font_bruno_18);
LV_FONT_DECLARE(font_bruno_24);
LV_FONT_DECLARE(icon_material_32);
LV_FONT_DECLARE(lv_font_montserrat_18);

// Module state
static bool notification_mode = false;
static lv_obj_t* notification_panel = NULL;
static std::vector<Earthquake> panel_quakes;  // Thread-safe snapshot for zoom callbacks
static constexpr float EARTHQUAKE_FOCUS_VIEW_ZOOM = MAX_ZOOM_LEVEL;

// Forward declarations
static void close_notification_panel();
static void earthquake_zoom_cb(lv_event_t* e);
static void notification_close_cb(lv_event_t* e);

// --- Helper Functions ---

static lv_color_t get_magnitude_color(float mag) {
    if (mag >= 7.0f) return lv_color_hex(0xFF3333);  // Red
    if (mag >= 6.0f) return lv_color_hex(0xFF6600);  // Orange
    if (mag >= 5.0f) return lv_color_hex(0xFFCC00);  // Yellow
    if (mag >= 4.0f) return lv_color_hex(0x88CC00);  // Yellow-green
    return lv_color_hex(0x00CC44);                   // Green
}

// --- Public API Implementation ---

void ui_toggle_notifications() {
    if (notification_mode) {
        close_notification_panel();
        return;
    }

    notification_mode = true;
    ui_dock_hide();

    if (ui_LblTime) lv_obj_add_flag(ui_LblTime, LV_OBJ_FLAG_HIDDEN);
    if (ui_LblDate) lv_obj_add_flag(ui_LblDate, LV_OBJ_FLAG_HIDDEN);
    if (ui_LblStatus) lv_obj_add_flag(ui_LblStatus, LV_OBJ_FLAG_HIDDEN);
    if (ui_BatteryContainer) lv_obj_add_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
    if (ui_LblZoom) lv_obj_add_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);

    notification_panel = lv_obj_create(ui_ScreenMain);
    lv_obj_set_size(notification_panel, g_board.screen_size, g_board.screen_size);
    lv_obj_center(notification_panel);
    lv_obj_set_style_bg_color(notification_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(notification_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(notification_panel, 0, 0);
    lv_obj_set_style_radius(notification_panel, 0, 0);
    lv_obj_set_style_pad_all(notification_panel, 16, 0);
    lv_obj_set_flex_flow(notification_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(notification_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title: "Earthquakes"
    lv_obj_t* title = lv_label_create(notification_panel);
    lv_label_set_text(title, "Earthquakes");
    lv_obj_set_style_text_font(title, &font_bruno_24, 0);
    lv_obj_set_style_text_color(title, theme_text_primary(), 0);
    lv_obj_invalidate(title);

    // Scrollable list container
    lv_obj_t* list = lv_obj_create(notification_panel);
    lv_obj_set_size(list, 340, g_board.screen_size - 180);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_anim_time(list, 450, 0);

    // Get thread-safe snapshot of earthquake list
    const bool copied_quakes = quake_copy_list(panel_quakes);
    if (!copied_quakes) {
        panel_quakes.clear();
        // Mutex timeout - show loading state
        lv_obj_t* loading = lv_label_create(list);
        lv_label_set_text(loading, "Loading...");
        lv_obj_set_style_text_font(loading, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(loading, theme_text_secondary(), 0);
        lv_obj_invalidate(loading);
        lv_obj_center(loading);
    } else {
        const AppSettings* settings = settings_get();
        const float mag_min = settings ? settings->mag_min : 0.0f;
        panel_quakes.erase(
            std::remove_if(panel_quakes.begin(), panel_quakes.end(),
                           [mag_min](const Earthquake& q) {
                               return q.mag < mag_min;
                           }),
            panel_quakes.end());
        std::sort(panel_quakes.begin(), panel_quakes.end(),
                  [](const Earthquake& a, const Earthquake& b) {
                      return a.time > b.time;
                  });
    }

    if (copied_quakes && panel_quakes.empty()) {
        lv_obj_t* empty = lv_label_create(list);
        lv_label_set_text(empty, "No earthquakes yet");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(empty, theme_text_secondary(), 0);
        lv_obj_invalidate(empty);
        lv_obj_center(empty);
    } else if (copied_quakes) {
        int count = panel_quakes.size();
        for (int i = 0; i < count; i++) {
            const Earthquake& q = panel_quakes[i];

            // Card container
            lv_obj_t* card = lv_obj_create(list);
            lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(card, theme_bg_elevated(), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_set_style_border_color(card, theme_border_default(), 0);
            lv_obj_set_style_radius(card, 12, 0);
            lv_obj_set_style_pad_all(card, 10, 0);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(card, 4, 0);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

            // Row 1: Magnitude + Time ago
            lv_obj_t* row1 = lv_obj_create(card);
            lv_obj_set_size(row1, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row1, 0, 0);
            lv_obj_set_style_pad_all(row1, 0, 0);
            lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_remove_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

            // Magnitude label (color-coded)
            char mag_buf[16];
            snprintf(mag_buf, sizeof(mag_buf), "M%.1f", q.mag);
            lv_obj_t* mag_lbl = lv_label_create(row1);
            lv_label_set_text(mag_lbl, mag_buf);
            lv_obj_set_style_text_font(mag_lbl, &font_bruno_18, 0);
            lv_obj_set_style_text_color(mag_lbl, get_magnitude_color(q.mag), 0);
            lv_obj_invalidate(mag_lbl);

            // Time ago label
            char time_buf[24];
            ui_format_time_ago(q.time, time_buf, sizeof(time_buf));
            lv_obj_t* time_lbl = lv_label_create(row1);
            lv_label_set_text(time_lbl, time_buf);
            lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(time_lbl, theme_text_secondary(), 0);
            lv_obj_invalidate(time_lbl);

            // Row 2: Location (truncated)
            lv_obj_t* place_lbl = lv_label_create(card);
            lv_label_set_text(place_lbl, q.place);
            lv_obj_set_width(place_lbl, lv_pct(100));
            // A panel may contain up to 100 cards. Circular scrolling on every
            // place label creates dozens of concurrent LVGL animations and
            // forces continuous full-list redraws. Static wrapping is both
            // readable and essentially idle once the panel has rendered.
            lv_label_set_long_mode(place_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(place_lbl, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(place_lbl, theme_text_primary(), 0);
            lv_obj_invalidate(place_lbl);

            // Row 3: Depth + Zoom button
            lv_obj_t* row3 = lv_obj_create(card);
            lv_obj_set_size(row3, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(row3, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row3, 0, 0);
            lv_obj_set_style_pad_all(row3, 0, 0);
            lv_obj_set_flex_flow(row3, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_remove_flag(row3, LV_OBJ_FLAG_SCROLLABLE);

            // Depth label
            char depth_buf[32];
            snprintf(depth_buf, sizeof(depth_buf), "Depth: %.1f km", q.depth);
            lv_obj_t* depth_lbl = lv_label_create(row3);
            lv_label_set_text(depth_lbl, depth_buf);
            lv_obj_set_style_text_font(depth_lbl, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(depth_lbl, theme_text_secondary(), 0);
            lv_obj_invalidate(depth_lbl);

            // Zoom button
            lv_obj_t* zoom_btn = lv_button_create(row3);
            lv_obj_set_size(zoom_btn, 70, 26);
            lv_obj_set_ext_click_area(zoom_btn, 11);
            lv_obj_set_style_bg_color(zoom_btn, theme_primary(), 0);
            lv_obj_set_style_bg_color(zoom_btn, theme_bg_card(), LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(zoom_btn, theme_primary_dim(), LV_STATE_PRESSED);
            lv_obj_set_style_radius(zoom_btn, 6, 0);
            lv_obj_add_event_cb(zoom_btn, earthquake_zoom_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

            lv_obj_t* zoom_lbl = lv_label_create(zoom_btn);
            lv_label_set_text(zoom_lbl, "Zoom");
            lv_obj_set_style_text_font(zoom_lbl, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(zoom_lbl, lv_color_black(), 0);
            lv_obj_invalidate(zoom_lbl);
            lv_obj_center(zoom_lbl);
        }
    }

    // Close button
    lv_obj_t* back_btn = lv_button_create(notification_panel);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_set_style_radius(back_btn, 32, 0);
    lv_obj_set_style_bg_color(back_btn, theme_bg_elevated(), 0);
    lv_obj_set_style_border_width(back_btn, 3, 0);
    lv_obj_set_style_border_color(back_btn, theme_primary(), 0);
    lv_obj_set_style_bg_color(back_btn, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(back_btn, theme_border_default(), LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(back_btn, theme_primary_dim(), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, notification_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* icon = lv_label_create(back_btn);
    lv_label_set_text(icon, ICON_CHECK);
    lv_obj_set_style_text_font(icon, &icon_material_32, 0);
    lv_obj_set_style_text_color(icon, theme_primary(), 0);
    lv_obj_center(icon);
}

bool ui_notification_panel_is_open() {
    return notification_mode;
}

// --- Private Implementation ---

static void close_notification_panel() {
    if (notification_panel) {
        lv_obj_delete(notification_panel);
        notification_panel = NULL;
    }
    notification_mode = false;
    panel_quakes.clear();  // Free snapshot memory

    if (ui_LblTime) lv_obj_remove_flag(ui_LblTime, LV_OBJ_FLAG_HIDDEN);
    if (ui_LblDate) lv_obj_remove_flag(ui_LblDate, LV_OBJ_FLAG_HIDDEN);
    if (ui_LblStatus) lv_obj_remove_flag(ui_LblStatus, LV_OBJ_FLAG_HIDDEN);
    if (ui_BatteryContainer) lv_obj_remove_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
    if (ui_LblZoom) lv_obj_remove_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);
    ui_dock_show();
}

static void earthquake_zoom_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    // Use module-level snapshot (already copied when panel opened)
    if (idx < 0 || idx >= (int)panel_quakes.size()) return;

    const Earthquake& q = panel_quakes[idx];
    // Use the active source's highest tile level, capped to z4 for flash fallback.
    map_tiles_set_zoom_level(tile_manager_get_max_tile_level());
    map_set_view(q.lat, q.lon, EARTHQUAKE_FOCUS_VIEW_ZOOM);
    close_notification_panel();
}

static void notification_close_cb(lv_event_t* e) {
    LV_UNUSED(e);
    close_notification_panel();
}
