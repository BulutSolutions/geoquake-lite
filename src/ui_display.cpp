#include "ui_display.h"
#include "crash_trace.h"
#include "config.h"
#include "board_profile.h"
#include "esp_timer.h"  // For wrap-safe 64-bit microseconds
#include "hande_display.h"
#include "hande_orientation.h"
#include "quake_loader.h"
#include "wifi_manager.h"
#include "battery.h"
#include "icons/icons.h"
#include "theme.h"
#include "version.h"
#include "ui_sync.h"
#include "ui_touch.h"
#include "splash_screen.h"
#include "map_renderer.h"
#include "lv_fs_sd.h"
#include "lv_fs_flash.h"
#include "ui_button_dock.h"
#include "ui_error.h"
#include "ui_settings.h"
#include "hande_settings.h"
#include "quake_notifications.h"
#include "settings_manager.h"
#include "hande_system_menu.h"
#include "perf_probe.h"
// Extracted modules (Phase 1-3 optimization)
#include "ui_touch.h"
#include "ui_power.h"
#include "ui_status_bar.h"
#include "ui_notifications_panel.h"
#include "ui_quake_detail.h"
#include "ui_helpers.h"
#include "ui_memory.h"
#include "amoled_ui_tokens.h"
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>
#include "SensorQMI8658.hpp"
#include <freertos/semphr.h>
#include <math.h>
#include <time.h>
#include <vector>
#include <esp_heap_caps.h>
#include "i2c_mutex.h"

// Touch hardware now managed in ui_touch.cpp module

// --- DISPLAY (hande_display.cpp) ---
// Panel size and column offset from the board profile, one 40-line strip in internal DMA SRAM,
// synchronous flush on the LVGL task, software rotation, partial render mode.
#define LVGL_BUF_LINES 40

// Start of the flush callback (LVGL task): crash-trace phase and cycle probe.
static void display_flush_start() {
    crash_trace_lvgl_phase(LVGL_PHASE_FLUSH);
    PERF_FLUSH_CB_ENTER();
}

// End of each strip (sync flush: still inside the flush callback on the LVGL task).
static void display_flush_done(const HandeDisplayFlushTiming&) {
    crash_trace_lvgl_phase(LVGL_PHASE_TICK);
    PERF_FLUSH_CB_EXIT();
}

static HandeDisplayConfig display_config() {
    HandeDisplayConfig config;
    config.size = g_board.screen_size;
    config.col_offset = g_board.col_offset;
    config.buffer_lines = LVGL_BUF_LINES;
    config.double_buffer = false;
    config.buffers_in_psram = false;
    config.software_rotation = true;
    config.render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
    config.async_flush = false;
    config.on_flush_start = display_flush_start;
    config.on_flush_done = display_flush_done;
    return config;
}

// Bus + panel, delay(250) for the power rails and the CO5300, begin, black. Safe to call twice.
void ui_display_create_gfx() {
    hande_display_panel_begin(display_config());
}

// --- UI OBJECTS ---
lv_obj_t *ui_ScreenMain;
lv_obj_t *ui_ImgContainer;
lv_obj_t *ui_ImgMap;
lv_obj_t *ui_OverlayPanel;
lv_obj_t *ui_LblTime;
lv_obj_t *ui_LblDate;
lv_obj_t *ui_TimeContainer = NULL;  // Exported for ui_power module
lv_obj_t *ui_LblStatus;
lv_obj_t *ui_LblBatteryIcon = NULL;
lv_obj_t *ui_LblBattery = NULL;
lv_obj_t *ui_LblZoom = NULL;
lv_obj_t *ui_LblAmPm = NULL;  // Exported for ui_status_bar module
static lv_obj_t *ui_TimeRow = NULL;
lv_obj_t *ui_LblCharging = NULL;  // Exported for ui_status_bar module
lv_obj_t *ui_BatteryContainer = NULL;  // Exported for ui_power/status_bar modules
lv_obj_t *ui_LoaderPanel;
lv_obj_t *ui_Spinner;
lv_obj_t *ui_LblLoader;
static lv_obj_t *ui_SplashScreen = NULL;
static lv_obj_t *ui_SettingsPanel = NULL;

// Custom fonts
LV_FONT_DECLARE(font_bruno_18);
LV_FONT_DECLARE(font_bruno_24);
LV_FONT_DECLARE(font_bruno_32);
LV_FONT_DECLARE(font_seg7_48);
LV_FONT_DECLARE(icon_material_24);
LV_FONT_DECLARE(lv_font_montserrat_18);

// --- STATE ---
lv_timer_t *overlay_timer = NULL;
lv_timer_t *clock_timer = NULL;
static volatile UiState ui_state = UI_STATE_BOOT_SPLASH;
static bool wifi_cached_connected = false;
static String wifi_cached_ssid = "";
static int wifi_cached_rssi = 0;
static bool wifi_request_from_settings = false;
static lv_obj_t* wifi_help_screen = NULL;
static lv_obj_t* wifi_help_icon = NULL;
static lv_obj_t* wifi_help_title = NULL;
static lv_obj_t* wifi_help_ssid = NULL;
static lv_obj_t* wifi_help_detail = NULL;
static lv_obj_t* wifi_help_ip = NULL;
static lv_obj_t* wifi_help_continue_btn = NULL;
static lv_obj_t* wifi_help_continue_label = NULL;
static lv_obj_t* wifi_help_back_btn = NULL;
static lv_timer_t* wifi_help_timer = NULL;
static bool wifi_help_from_settings = false;
static bool auto_dim_enabled = false;
static int8_t auto_dim_start_hour = 22;
static int8_t auto_dim_end_hour = 7;
static bool auto_dim_active = false;
static bool auto_dim_save_pending = false;
// show_components_pending now managed in ui_touch.cpp (accessed via ui_touch_has_pending_show_components())
// Gesture state (is_pinching, is_single_touch_panning) now managed in ui_touch.cpp
static lv_indev_t *touch_indev = NULL;

static lv_obj_t* notification_panel = NULL;
static unsigned long status_override_until_ms = 0;
static bool pending_map_restore = false;
static float saved_center_lat = 0.0f;
static float saved_center_lon = 0.0f;
static float saved_zoom = 1.0f;

#define AUTO_DIM_NVS_NAMESPACE "gql_auto_dim"
#define NIGHT_BRIGHTNESS 30

static void auto_dim_load();

UiState ui_get_state() { return ui_state; }

// Memory health check now provided by ui_memory module (ui_check_memory_health)

void ui_request_wifi_setup(bool from_settings) {
    wifi_request_from_settings = from_settings;
    ui_set_state(UI_STATE_WIFI_SETUP);
}

// Defer state transitions until input is released to avoid LVGL press/use-after-free
static lv_timer_t *pending_state_timer = NULL;
static UiState pending_state = UI_STATE_MAIN;
static bool pending_state_valid = false;

// Touch functions now provided by ui_touch module

static void deferred_state_transition_cb(lv_timer_t *timer) {
    if (ui_touch_is_pressed()) {
        return;
    }

    if (!pending_state_valid) {
        lv_timer_del(timer);
        pending_state_timer = NULL;
        return;
    }

    UiState target_state = pending_state;
    pending_state_valid = false;

    lv_timer_del(timer);
    pending_state_timer = NULL;

    ui_set_state(target_state);
}

// Deferred dock hide callback
// Runs AFTER the event that triggered it completes, making it safe
// to hide/delete dock buttons that may still be referenced by LVGL's event dispatcher
static void deferred_dock_hide_cb(lv_timer_t *timer) {
    Serial.println("[DOCK] Deferred hide timer fired");

    // Step 1: Reset input device BEFORE any deletion
    // CRITICAL FIX: Only reset if still on main screen where dock was visible
    // When transitioning to SETTINGS/WIFI_SETUP, LVGL screen context changes,
    // invalidating the input device's internal state. Calling lv_indev_reset()
    // on an invalidated indev causes LoadProhibited crash at 0x00000000.
    lv_indev_t *indev = touch_indev;
    if (indev && ui_state == UI_STATE_MAIN) {
        lv_indev_reset(indev, NULL);
        Serial.println("[DOCK] Input device reset (main screen active)");
    } else {
        Serial.println("[DOCK] Skipped input reset (screen changed)");
    }

    // Step 2: Now safe to hide dock buttons
    ui_dock_hide();

    Serial.println("[DOCK] Dock buttons hidden (deferred, input released)");
    // Timer auto-deletes (repeat_count=1)
}

static void ui_hide_core_layers() {
    if (ui_LoaderPanel) lv_obj_add_flag(ui_LoaderPanel, LV_OBJ_FLAG_HIDDEN);
    if (ui_ImgContainer) lv_obj_add_flag(ui_ImgContainer, LV_OBJ_FLAG_HIDDEN);
    if (ui_OverlayPanel) lv_obj_add_flag(ui_OverlayPanel, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_help_update_labels() {
    if (!wifi_help_screen || !wifi_help_title || !wifi_help_ssid || !wifi_help_detail || !wifi_help_ip) {
        return;
    }

    if (wifi_is_connected()) {
        char rssi_buf[32];
        char ip_buf[40];
        snprintf(rssi_buf, sizeof(rssi_buf), "Signal: %ld dBm", (long)wifi_get_rssi());
        snprintf(ip_buf, sizeof(ip_buf), "IP: %s", wifi_get_ip_str());

        lv_label_set_text(wifi_help_title, "Wi-Fi Connected");
        lv_label_set_text(wifi_help_ssid, wifi_get_ssid());
        lv_label_set_text(wifi_help_detail, rssi_buf);
        lv_label_set_text(wifi_help_ip, ip_buf);
        if (wifi_help_continue_label) lv_label_set_text(wifi_help_continue_label, "Done");
        lv_obj_set_style_text_color(wifi_help_detail, theme_text_secondary(), 0);
    } else {
        lv_label_set_text(wifi_help_title, "No Wi-Fi");
        lv_label_set_text(wifi_help_ssid, "No saved network.");
        lv_label_set_text(wifi_help_detail, "Set up Wi-Fi from the install page:");
        lv_obj_set_style_text_color(wifi_help_detail, theme_text_secondary(), 0);
        lv_label_set_text(wifi_help_ip, INSTALL_PAGE_URL);
        if (wifi_help_continue_label) lv_label_set_text(wifi_help_continue_label, "Use Offline");
    }
    // Connected: one filled "Done". No Wi-Fi: "Use Offline" is the only action.
    const bool connected = wifi_is_connected();
    if (wifi_help_continue_btn && wifi_help_continue_label) {
        lv_obj_set_style_bg_opa(wifi_help_continue_btn, connected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wifi_help_continue_btn, connected ? 0 : 1, 0);
        lv_obj_set_style_text_color(wifi_help_continue_label, connected ? lv_color_black() : theme_primary(), 0);
    }
}

static void wifi_help_timer_cb(lv_timer_t* timer) {
    (void)timer;
    wifi_help_update_labels();
}

static void wifi_help_continue_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_set_state(UI_STATE_MAIN);
}

static void wifi_help_back_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_set_state(UI_STATE_MAIN);
}

static void wifi_help_hide() {
    if (wifi_help_timer) {
        lv_timer_del(wifi_help_timer);
        wifi_help_timer = NULL;
    }
    if (wifi_help_screen) {
        lv_obj_delete(wifi_help_screen);
        wifi_help_screen = NULL;
    }

    wifi_help_icon = NULL;
    wifi_help_title = NULL;
    wifi_help_ssid = NULL;
    wifi_help_detail = NULL;
    wifi_help_ip = NULL;
    wifi_help_continue_btn = NULL;
    wifi_help_continue_label = NULL;
    wifi_help_back_btn = NULL;
    wifi_help_from_settings = false;
}

static void wifi_help_show(bool from_settings) {
    wifi_help_hide();
    wifi_help_from_settings = from_settings;

    ui_hide_loading();

    wifi_help_screen = lv_obj_create(ui_ScreenMain);
    lv_obj_set_size(wifi_help_screen, g_board.screen_size, g_board.screen_size);
    lv_obj_set_pos(wifi_help_screen, 0, 0);
    lv_obj_set_style_bg_color(wifi_help_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_help_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_help_screen, 0, 0);
    lv_obj_set_style_radius(wifi_help_screen, 0, 0);
    lv_obj_remove_flag(wifi_help_screen, LV_OBJ_FLAG_SCROLLABLE);

    wifi_help_icon = lv_label_create(wifi_help_screen);
    lv_label_set_text(wifi_help_icon, ICON_WIFI);
    lv_obj_set_style_text_font(wifi_help_icon, &icon_material_24, 0);
    lv_obj_set_style_text_color(wifi_help_icon, theme_primary(), 0);
    lv_obj_align(wifi_help_icon, LV_ALIGN_TOP_MID, 0, 78);

    wifi_help_title = lv_label_create(wifi_help_screen);
    lv_obj_set_width(wifi_help_title, g_board.screen_size - 80);
    lv_obj_set_style_text_align(wifi_help_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_help_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(wifi_help_title, &font_orb_28, 0);
    lv_obj_set_style_text_color(wifi_help_title, lv_color_white(), 0);
    lv_obj_align(wifi_help_title, LV_ALIGN_TOP_MID, 0, 132);

    wifi_help_ssid = lv_label_create(wifi_help_screen);
    lv_obj_set_width(wifi_help_ssid, g_board.screen_size - 80);
    lv_obj_set_style_text_align(wifi_help_ssid, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_help_ssid, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(wifi_help_ssid, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(wifi_help_ssid, theme_text_primary(), 0);
    lv_obj_align(wifi_help_ssid, LV_ALIGN_TOP_MID, 0, 184);

    wifi_help_detail = lv_label_create(wifi_help_screen);
    lv_obj_set_width(wifi_help_detail, g_board.screen_size - 90);
    lv_obj_set_style_text_align(wifi_help_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_help_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(wifi_help_detail, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(wifi_help_detail, theme_text_secondary(), 0);
    lv_obj_align(wifi_help_detail, LV_ALIGN_TOP_MID, 0, 230);

    wifi_help_ip = lv_label_create(wifi_help_screen);
    lv_obj_set_width(wifi_help_ip, g_board.screen_size - 90);
    lv_obj_set_style_text_align(wifi_help_ip, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_help_ip, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(wifi_help_ip, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(wifi_help_ip, theme_text_secondary(), 0);
    lv_obj_align(wifi_help_ip, LV_ALIGN_TOP_MID, 0, 298);

    // One 260x56 button, centred, bottom edge at y 440: clear of the rounded corners.
    // Outline (1 px accent border + accent text) while there is no Wi-Fi; filled "Done" when connected
    wifi_help_continue_btn = lv_button_create(wifi_help_screen);
    lv_obj_set_size(wifi_help_continue_btn, 260, 56);
    lv_obj_align(wifi_help_continue_btn, LV_ALIGN_TOP_MID, 0, 384);
    lv_obj_set_style_bg_color(wifi_help_continue_btn, theme_primary(), 0);
    lv_obj_set_style_bg_opa(wifi_help_continue_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wifi_help_continue_btn, theme_primary(), 0);
    lv_obj_set_style_border_width(wifi_help_continue_btn, 0, 0);
    lv_obj_set_style_shadow_width(wifi_help_continue_btn, 0, 0);
    lv_obj_set_style_radius(wifi_help_continue_btn, 8, 0);
    lv_obj_set_style_bg_color(wifi_help_continue_btn, lv_color_hex(AMOLED_PRESSED_SURFACE_HEX), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(wifi_help_continue_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(wifi_help_continue_btn, 6);
    lv_obj_set_style_bg_color(wifi_help_continue_btn, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(wifi_help_continue_btn, theme_border_default(), LV_STATE_DISABLED);
    lv_obj_add_event_cb(wifi_help_continue_btn, wifi_help_continue_event, LV_EVENT_CLICKED, NULL);

    wifi_help_continue_label = lv_label_create(wifi_help_continue_btn);
    lv_obj_set_style_text_font(wifi_help_continue_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(wifi_help_continue_label, lv_color_black(), 0);
    lv_obj_center(wifi_help_continue_label);

    if (wifi_help_from_settings) {
        wifi_help_back_btn = lv_button_create(wifi_help_screen);
        lv_obj_set_size(wifi_help_back_btn, 64, 64);
        lv_obj_align(wifi_help_back_btn, LV_ALIGN_BOTTOM_MID, 0, -34);
        lv_obj_set_style_bg_color(wifi_help_back_btn, theme_primary_dark(), 0);
        lv_obj_set_style_border_width(wifi_help_back_btn, 2, 0);
        lv_obj_set_style_border_color(wifi_help_back_btn, theme_primary(), 0);
        lv_obj_set_style_shadow_width(wifi_help_back_btn, 0, 0);
        lv_obj_set_style_radius(wifi_help_back_btn, 32, 0);
        lv_obj_set_style_bg_color(wifi_help_back_btn, theme_primary_dim(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(wifi_help_back_btn, theme_bg_card(), LV_STATE_DISABLED);
        lv_obj_set_style_border_color(wifi_help_back_btn, theme_border_default(), LV_STATE_DISABLED);
        lv_obj_add_event_cb(wifi_help_back_btn, wifi_help_back_event, LV_EVENT_CLICKED, NULL);

        lv_obj_t* back_icon = lv_label_create(wifi_help_back_btn);
        lv_label_set_text(back_icon, ICON_CHECK);
        lv_obj_set_style_text_font(back_icon, &icon_material_24, 0);
        lv_obj_set_style_text_color(back_icon, theme_primary(), 0);
        lv_obj_center(back_icon);
    }

    wifi_help_update_labels();
    wifi_help_timer = lv_timer_create(wifi_help_timer_cb, 1000, NULL);
}

// Touch IRQ stuck detection now handled in ui_touch.cpp module

void ui_set_state(UiState state) {
    if (ui_state == state && !pending_state_valid) return;

    // If a press is active, defer the transition until the release is processed.
    if (ui_touch_is_pressed()) {
        pending_state = state;
        pending_state_valid = true;
        if (!pending_state_timer) {
            pending_state_timer = lv_timer_create(deferred_state_transition_cb, 5, NULL);
        }
        return;
    }

    pending_state_valid = false;

    // Temporarily disable input while screens are being swapped/hidden.
    ui_disable_input_temporarily(30);  // Reduced from 150ms to 30ms

    // === EXIT HANDLERS ===
    // Handle cleanup when LEAVING states
    if (ui_state == UI_STATE_WIFI_SETUP) {
        wifi_help_hide();
    }

    if (ui_state == UI_STATE_MAIN) {
        // FIX: Defer dock hiding until after event completes
        // This prevents use-after-free crash when LVGL tries to finish processing
        // the dock button click event that triggered this state transition.
        //
        // Pattern (per user guidance):
        // 1. Defer transition using lv_timer (150ms - v1.0.68 fix)
        // 2. Release/reset input before delete
        // 3. Hide-then-delete (hide first, delete in callback)
        //
        // FIX v1.0.68: Increased delay from 1ms to 150ms to allow LVGL's
        // periodic input device timer (fires every ~5-10ms) to complete
        // 15-30 cycles before hiding the dock. This prevents use-after-free
        // crash where LVGL tries to access the hidden dock button.
        Serial.println("[DOCK] Queuing deferred dock hide");
        lv_timer_t *timer = lv_timer_create(deferred_dock_hide_cb, 150, NULL);
        lv_timer_set_repeat_count(timer, 1);  // Run once and auto-delete

        // End any active gestures (panning/pinching) via ui_touch module
        ui_touch_end_gestures();
    }

    if (ui_state == UI_STATE_SETTINGS) {
        // CRITICAL: the main screen must be active before the settings screen goes away, otherwise the active
        // screen is deleted (LoadProhibited). The shared screen loads the previous screen (ui_ScreenMain) first and
        // deletes itself asynchronously; it is already closed when the user left through its close button.
        ui_settings_hide();
        if (lv_screen_active() != ui_ScreenMain) {
            lv_screen_load(ui_ScreenMain);
        }
        pending_map_restore = true;
    }

    if (ui_state == UI_STATE_ERROR) {
        ui_error_hide();  // Clean up error screen
    }

    ui_state = state;

    // Ensure touch input device is active before screen transitions
    if (state == UI_STATE_WIFI_SETUP || state == UI_STATE_MAIN) {
        extern void ui_touch_ensure_active();
        ui_touch_ensure_active();
    }

    if (state != UI_STATE_BOOT_SPLASH) {
        if (lv_screen_active() != ui_ScreenMain) lv_screen_load(ui_ScreenMain);
        if (ui_SplashScreen) { lv_obj_delete(ui_SplashScreen); ui_SplashScreen = NULL; }
    }

    switch (state) {
        case UI_STATE_STARTUP_LOADING:
            ui_show_loading("Starting...", false);
            break;
        case UI_STATE_WIFI_SETUP:
            if (splash_screen_is_active()) {
                splash_screen_dismiss();
            }
            ui_hide_core_layers();
            wifi_help_show(wifi_request_from_settings);
            wifi_request_from_settings = false;
            break;
        case UI_STATE_FETCH_MANIFEST:
            ui_hide_core_layers();
            ui_show_loading("Syncing Regions...", false);
            break;
        case UI_STATE_MAIN:
            if (ui_LoaderPanel) lv_obj_add_flag(ui_LoaderPanel, LV_OBJ_FLAG_HIDDEN);
            if (ui_ImgContainer) lv_obj_remove_flag(ui_ImgContainer, LV_OBJ_FLAG_HIDDEN);
            if (ui_OverlayPanel) lv_obj_remove_flag(ui_OverlayPanel, LV_OBJ_FLAG_HIDDEN);
            ui_dock_show();  // Show dock buttons on main screen
            if (pending_map_restore) {
                map_set_view(saved_center_lat, saved_center_lon, saved_zoom);
                map_redraw();
                map_render_earthquakes();
                pending_map_restore = false;
            }
            if (splash_screen_is_active()) {
                splash_screen_set_ready();
            }
            break;
        case UI_STATE_SETTINGS: {
            ui_hide_core_layers();
            saved_center_lat = map_get_center_lat();
            saved_center_lon = map_get_center_lon();
            saved_zoom = map_get_zoom();

            // FIX #11: Show loading indicator during heavy settings UI creation
            // This provides visual feedback since settings creation can take ~100-200ms
            ui_show_loading("Loading settings...", false);
            lv_refr_now(NULL);  // Force immediate render before heavy UI creation

            // Check memory health before creating Settings UI
            MemHealthStatus health = ui_check_memory_health();
            if (health == MEM_HEALTH_CRITICAL) {
                Serial.println("[UI] Memory critical! Clearing tile cache before Settings...");
                extern void tile_cache_clear();
                tile_cache_clear();  // Sacrifice tiles to ensure UI works
            }

            // The Settings screen (ui_settings.cpp describes GeoQuake's rows).
            ui_settings_show();

            // Hide loading indicator after settings UI is ready
            ui_hide_loading();
            break;
        }
        case UI_STATE_ERROR:
            // Error screen is managed by ui_error.cpp
            // Just ensure dock is hidden
            ui_dock_hide();
            break;
        default: break;
    }
}

// --- QUAKE RENDERING ---
// Now handled by map_renderer.cpp - see map_render_earthquakes()

// --- STATUS LABEL TAP-TO-REFRESH ---
static void status_label_click_cb(lv_event_t* e) {
    if (is_fetching_quakes) {
        Serial.println("[STATUS] Refresh already in progress, ignoring tap");
        return;
    }
    Serial.println("[STATUS] Status label tapped - triggering refresh");
    ui_queue_manual_refresh();
}

// --- MAP GESTURE HANDLING ---
// Forward declarations
static bool is_map_gesture_active = false;
static lv_point_t gesture_start_point;

static void map_gesture_event_cb(lv_event_t* e) {
    CrashTraceScope trace(LVGL_PHASE_GESTURE);
    // NOTE: Panning is handled directly by ui_touch.cpp (touch_read_cb).
    // This callback ONLY handles tap-to-select-earthquake detection.
    // DO NOT add map_on_touch_begin/move/end calls here - causes dual pipeline conflict.

    lv_event_code_t code = lv_event_get_code(e);
    // Registered for LV_EVENT_ALL: draw/refresh events arrive with no active indev and
    // would reset the tap state between PRESSED and RELEASED below.
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) {
        is_map_gesture_active = false;  // Reset to prevent stale state
        return;
    }
    if (ui_is_panel_open()) {
        is_map_gesture_active = false;  // Reset to prevent stale state
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Get absolute coordinates of the container to convert screen points to relative
    lv_obj_t* target = lv_event_get_current_target_obj(e);
    lv_area_t coords;
    lv_obj_get_coords(target, &coords);

    int16_t map_x = point.x - coords.x1;
    int16_t map_y = point.y - coords.y1;

    switch (code) {
        case LV_EVENT_PRESSED:
            // Check if touch is on dock button first
            if (ui_dock_is_touch_on_dock(point.x, point.y)) {
                is_map_gesture_active = false;  // Not a map gesture
                return;  // Let dock handle the event
            }
            // Record start point for tap detection (panning handled by ui_touch.cpp)
            is_map_gesture_active = true;
            gesture_start_point = point;
            break;

        case LV_EVENT_RELEASED:
            if (is_map_gesture_active) {
                // Detect tap (minimal movement from press to release)
                float dx = point.x - gesture_start_point.x;
                float dy = point.y - gesture_start_point.y;
                float move_dist = sqrtf(dx*dx + dy*dy);

                // quake_data_ready is a one-shot "new data" event the frame loop consumes;
                // ui_find_nearest_quake() returns -1 when no quakes are loaded.
                if (move_dist < 10.0f) {
                    // This was a tap, check if it hit an earthquake
                    int quake_idx = ui_find_nearest_quake(map_x, map_y);
                    if (quake_idx >= 0) {
                        ui_show_quake_detail_sheet(quake_idx);
                    }
                }
                is_map_gesture_active = false;
            }
            break;

        case LV_EVENT_PRESS_LOST:
            is_map_gesture_active = false;
            break;

        default:
            break;
    }
}

// --- PERFORMANCE TIMING (non-invasive) ---
// Accumulate timing data, report at 1Hz to avoid log spam in hot path
static uint32_t g_tick_total_us = 0;
static uint32_t g_tick_count = 0;
static uint32_t g_last_perf_report_ms = 0;

// MAIN-screen-only timing (separate from non-main frames)
static uint64_t g_main_tick_total_us = 0;
static uint32_t g_main_tick_count = 0;
static uint32_t g_main_tick_max_us = 0;  // Track worst frame
static uint32_t g_last_detail_report_ms = 0;

// Touch state variables now managed in ui_touch.cpp module
// Idle sleep state (idle_sleep_active, last_touch_millis, user_brightness_before_sleep) via ui_touch_*()
// Auto-hide state (auto_hide_active, last_ui_interaction_millis) via ui_touch_*()

// Touch recovery now handled by ui_touch_reinit() in ui_touch.cpp module

// Touch callback now provided by ui_touch.cpp module (touch_read_cb)
// Gesture handling (pan, pinch) now in ui_touch.cpp
// Show all components now in ui_power module



// Display bring-up shared by every start: panel, LVGL core, draw buffers, display driver and touch input.
// Builds no screens.
void ui_init_display_and_touch() {
    // Touch hardware initialization moved to ui_touch_init() to avoid duplicate I2C init
    Serial.println("[UI] Touch hardware will be initialized via ui_touch_init()");

    Serial.println("[UI_INIT] Allocating LVGL display buffers (DMA-capable)...");
    Serial.printf("[UI_INIT] DMA heap before: %lu KB (largest block: %lu KB)\n",
                 heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024,
                 heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024);

    // Panel (already begun by ui_display_create_gfx), lv_init + tick, draw buffer + rotation buffer,
    // LVGL display, rounder and flush.
    const size_t buf_size = (size_t)g_board.screen_size * LVGL_BUF_LINES * sizeof(lv_color16_t);
    if (!hande_display_init(display_config())) {
        Serial.printf("[UI_INIT] ❌ FATAL: Failed to allocate buf_1 (%lu KB) or create display!\n", buf_size / 1024);
        ESP.restart();  // Cannot continue without display buffer
    }
    Serial.printf("[UI_INIT] ✅ buf_1 allocated (%lu KB)\n", buf_size / 1024);

    lv_display_t *disp = hande_display_get();
    PERF_ATTACH_DISPLAY(disp);

    // The first frame that reaches the panel must already be in the orientation saved at the last commit: restore it
    // here, after the display exists and before the touch driver, any screen load or the splash's first flush, and
    // the filter is seeded so the first ticks do not flip back. Safe before orientation_init().
    hande_orientation_restore_saved();

    ui_touch_init(disp);
    touch_indev = ui_touch_get_indev();
    Serial.printf("[UI_INIT] Registered indev: %p\n", touch_indev);

    Serial.println("[UI_INIT] Touch input device initialized");
}

void ui_init() {
    ui_init_display_and_touch();

    // Initialize LVGL filesystem drivers (must be after lv_init)
    lv_fs_sd_init();
    lv_fs_flash_init();

    auto_dim_load();
    ui_touch_update_interaction_time();  // Initialize touch timing to prevent immediate sleep

    // Build UI
    ui_ScreenMain = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_ScreenMain, lv_color_black(), 0);

    ui_ImgContainer = lv_obj_create(ui_ScreenMain);
    lv_obj_set_size(ui_ImgContainer, g_board.screen_size, g_board.screen_size);
    lv_obj_center(ui_ImgContainer);
    if (g_board.is_round) {
        lv_obj_set_style_radius(ui_ImgContainer, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(ui_ImgContainer, true, 0);
    } else {
        lv_obj_set_style_radius(ui_ImgContainer, 0, 0);
        lv_obj_set_style_clip_corner(ui_ImgContainer, false, 0);
    }
    lv_obj_set_style_border_width(ui_ImgContainer, 0, 0);  // No border for full screen
    // LVGL's default theme is light: unstyled, this 480x480 container is white wherever the map tiles do not cover it
    lv_obj_set_style_bg_color(ui_ImgContainer, lv_color_black(), 0);
    lv_obj_set_style_pad_all(ui_ImgContainer, 0, 0);       // No padding

    // Initialize map renderer (supports both vector and tile modes)
    map_renderer_init(ui_ImgContainer);
    ui_ImgMap = map_get_canvas();
    lv_obj_center(ui_ImgMap);

    // Enable tile mode if SD card has tiles (auto-falls back to vector if unavailable)
    if (map_tiles_available()) {
        map_set_tile_mode(true);
        Serial.println("[UI] Tile mode enabled");
    }

    // Add gesture handling for pan/zoom - attach to container
    // This allows panning even when touching earthquake dots (which bubble events)
    lv_obj_add_flag(ui_ImgContainer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ui_ImgContainer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_ImgContainer, map_gesture_event_cb, LV_EVENT_ALL, NULL);

    // Make map canvas and other children transparent to events so they bubble to container
    lv_obj_remove_flag(ui_ImgMap, LV_OBJ_FLAG_CLICKABLE);

    // Container setup
    lv_obj_remove_flag(ui_ImgContainer, LV_OBJ_FLAG_SCROLLABLE);

    ui_OverlayPanel = lv_obj_create(ui_ScreenMain);
    lv_obj_set_size(ui_OverlayPanel, g_board.screen_size, g_board.screen_size);
    lv_obj_set_style_bg_opa(ui_OverlayPanel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_OverlayPanel, 0, 0);
    lv_obj_remove_flag(ui_OverlayPanel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_OverlayPanel, LV_OBJ_FLAG_IGNORE_LAYOUT);
    // Let touch events pass through to map below (overlay is visible for HUD)

    // Time container with dark pill background
    ui_TimeContainer = lv_obj_create(ui_OverlayPanel);
    lv_obj_set_size(ui_TimeContainer, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ui_TimeContainer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_TimeContainer, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(ui_TimeContainer, 16, 0);
    lv_obj_set_style_pad_ver(ui_TimeContainer, 6, 0);
    lv_obj_set_style_radius(ui_TimeContainer, 20, 0);
    lv_obj_set_style_border_width(ui_TimeContainer, 0, 0);
    lv_obj_set_flex_flow(ui_TimeContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_TimeContainer, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(ui_TimeContainer, LV_ALIGN_TOP_MID, 0, 0);

    // Date display (inside the same pill, above time)
    ui_LblDate = lv_label_create(ui_TimeContainer);
    lv_label_set_text(ui_LblDate, "");
    lv_obj_set_style_text_font(ui_LblDate, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ui_LblDate, theme_primary(), 0);
    lv_obj_set_style_text_align(ui_LblDate, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ui_LblDate, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_LblDate, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(ui_LblDate, LV_OBJ_FLAG_HIDDEN);

    // Time row (time + AM/PM) inside the same pill, below date
    ui_TimeRow = lv_obj_create(ui_TimeContainer);
    lv_obj_set_size(ui_TimeRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ui_TimeRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_TimeRow, 0, 0);
    lv_obj_set_style_pad_all(ui_TimeRow, 0, 0);
    lv_obj_set_flex_flow(ui_TimeRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_TimeRow, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Time display
    ui_LblTime = lv_label_create(ui_TimeRow);
    lv_label_set_text(ui_LblTime, "");  // filled once the local time is known
    lv_obj_set_style_text_font(ui_LblTime, &font_seg7_48, 0);
    lv_obj_set_style_text_color(ui_LblTime, theme_primary(), 0);
    lv_obj_set_style_text_align(ui_LblTime, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ui_LblTime, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_LblTime, LV_LABEL_LONG_WRAP);

    // AM/PM indicator (hidden unless 12h mode)
    ui_LblAmPm = lv_label_create(ui_TimeRow);
    lv_label_set_text(ui_LblAmPm, "");
    lv_obj_set_style_text_font(ui_LblAmPm, &font_bruno_18, 0);
    lv_obj_set_style_text_color(ui_LblAmPm, theme_primary(), 0);
    lv_obj_set_style_pad_left(ui_LblAmPm, 6, 0);
    lv_obj_add_flag(ui_LblAmPm, LV_OBJ_FLAG_HIDDEN);

    // Bottom HUD container (flex column, centered at bottom)
    // Holds battery indicator (top) and status label (bottom) for clean vertical stacking
    lv_obj_t* ui_BottomHUD = lv_obj_create(ui_OverlayPanel);
    lv_obj_remove_style_all(ui_BottomHUD);
    lv_obj_set_size(ui_BottomHUD, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(ui_BottomHUD, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_flex_flow(ui_BottomHUD, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_BottomHUD, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui_BottomHUD, 4, 0);  // 4px spacing between elements
    lv_obj_remove_flag(ui_BottomHUD, (lv_obj_flag_t)((lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE)));

    // Battery container (now child of ui_BottomHUD, appears FIRST = TOP)
    ui_BatteryContainer = lv_obj_create(ui_BottomHUD);
    lv_obj_remove_style_all(ui_BatteryContainer);
    lv_obj_set_size(ui_BatteryContainer, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui_BatteryContainer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_BatteryContainer, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(ui_BatteryContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Respect show_battery setting at startup
    AppSettings* settings = settings_get();
    if (settings && !settings->show_battery) {
        lv_obj_add_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
    }

    ui_LblBatteryIcon = lv_label_create(ui_BatteryContainer);
    lv_label_set_text(ui_LblBatteryIcon, ICON_BATTERY_FULL);
    lv_obj_set_style_text_font(ui_LblBatteryIcon, &icon_material_24, 0);
    lv_obj_set_style_text_color(ui_LblBatteryIcon, theme_primary(), 0);
    // Rotate battery icon 90 degrees (0.1 degree units)
    lv_obj_set_style_transform_pivot_x(ui_LblBatteryIcon, 12, 0);
    lv_obj_set_style_transform_pivot_y(ui_LblBatteryIcon, 12, 0);
    lv_obj_set_style_transform_angle(ui_LblBatteryIcon, 900, 0);
    lv_obj_add_flag(ui_LblBatteryIcon, LV_OBJ_FLAG_HIDDEN);

    ui_LblBattery = lv_label_create(ui_BatteryContainer);
    lv_label_set_text(ui_LblBattery, "");
    lv_obj_set_style_text_font(ui_LblBattery, &font_bruno_18, 0);
    lv_obj_set_style_text_color(ui_LblBattery, theme_primary(), 0);
    lv_obj_add_flag(ui_LblBattery, LV_OBJ_FLAG_HIDDEN);

    ui_LblCharging = lv_label_create(ui_BatteryContainer);
    lv_label_set_text(ui_LblCharging, ICON_BOLT);
    lv_obj_set_style_text_font(ui_LblCharging, &icon_material_24, 0);
    lv_obj_set_style_text_color(ui_LblCharging, theme_primary(), 0);
    lv_obj_add_flag(ui_LblCharging, LV_OBJ_FLAG_HIDDEN);

    // Status label (now child of ui_BottomHUD, appears SECOND = BOTTOM)
    ui_LblStatus = lv_label_create(ui_BottomHUD);
    lv_label_set_text(ui_LblStatus, "System Ready");
    lv_obj_set_style_text_font(ui_LblStatus, &font_bruno_18, 0);
    lv_obj_set_style_text_color(ui_LblStatus, theme_primary(), 0);
    lv_obj_set_style_text_align(ui_LblStatus, LV_TEXT_ALIGN_CENTER, 0);
    // At least 160 px for layout stability; grows for "Offline - updated 14:05" (max 340 px, clear of the corners)
    lv_obj_set_width(ui_LblStatus, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(ui_LblStatus, 160, 0);
    lv_obj_set_style_max_width(ui_LblStatus, 340, 0);
    lv_obj_set_ext_click_area(ui_LblStatus, 14);
    lv_label_set_long_mode(ui_LblStatus, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_bg_color(ui_LblStatus, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_LblStatus, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(ui_LblStatus, 14, 0);
    lv_obj_set_style_pad_ver(ui_LblStatus, 4, 0);
    lv_obj_set_style_radius(ui_LblStatus, 16, 0);
    // Make status label tappable for manual refresh
    lv_obj_add_flag(ui_LblStatus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_opa(ui_LblStatus, LV_OPA_70, LV_STATE_PRESSED);  // Visual feedback on press
    lv_obj_add_event_cb(ui_LblStatus, status_label_click_cb, LV_EVENT_CLICKED, NULL);

    // Zoom indicator (hidden by default)
    ui_LblZoom = lv_label_create(ui_OverlayPanel);
    lv_obj_set_style_text_color(ui_LblZoom, theme_text_primary(), 0);
    lv_obj_set_style_text_font(ui_LblZoom, &font_bruno_32, 0);
    lv_obj_align(ui_LblZoom, LV_ALIGN_CENTER, 0, 0);  // Always centered on screen
    lv_obj_set_style_bg_color(ui_LblZoom, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_LblZoom, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(ui_LblZoom, 10, 0);
    lv_obj_set_style_radius(ui_LblZoom, 8, 0);
    lv_obj_add_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);

    // Create button dock (shared Settings gear top-right, Notifications on LEFT)
    ui_dock_create(ui_ScreenMain);

    // Keep the loading overlay above every root screen; parenting it to
    // ui_ScreenMain would hide its text while another screen is active.
    ui_LoaderPanel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ui_LoaderPanel);
    lv_obj_set_size(ui_LoaderPanel, g_board.screen_size, g_board.screen_size);
    lv_obj_set_pos(ui_LoaderPanel, 0, 0);
    lv_obj_set_style_bg_opa(ui_LoaderPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui_LoaderPanel, lv_color_black(), 0);
    lv_obj_remove_flag(ui_LoaderPanel, LV_OBJ_FLAG_SCROLLABLE);
    ui_LblLoader = lv_label_create(ui_LoaderPanel);
    lv_obj_set_style_text_color(ui_LblLoader, lv_color_white(), 0);
    lv_obj_center(ui_LblLoader);

    lv_screen_load(ui_ScreenMain);

    quake_notifs_init();
}

// Track last zoom for label update
static float last_displayed_zoom = -1.0f;
static unsigned long last_zoom_label_ms = 0;
static const unsigned long ZOOM_LABEL_HIDE_MS = 1200;

void ui_tick() {
    int64_t t0 = esp_timer_get_time();

    lv_timer_handler();
    int64_t t_lvgl = esp_timer_get_time();

    if (ui_touch_has_pending_show_components()) {
        ui_touch_clear_pending_show_components();
        ui_show_all_components();
    }

    if (splash_screen_is_active()) {
        splash_screen_tick();
        g_tick_total_us += (uint32_t)(esp_timer_get_time() - t0);
        g_tick_count++;
        return;
    }

    if (ui_state != UI_STATE_MAIN) {
        g_tick_total_us += (uint32_t)(esp_timer_get_time() - t0);
        g_tick_count++;
        return;
    }

    // Check if map needs redraw (view changed)
    bool map_changed = map_needs_redraw();

    int64_t t_pre_map = esp_timer_get_time();

    if (map_changed) {
        map_redraw();
    }

    int64_t t_map = esp_timer_get_time();

    // Ensure deferred dot updates happen after redraw
    map_process_pending_dots();

    // Update zoom indicator whenever zoom changes (independent of redraw)
    float current_zoom = map_get_zoom();
    if (ui_LblZoom && fabsf(current_zoom - last_displayed_zoom) > 0.01f) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1fx", current_zoom);
        lv_label_set_text(ui_LblZoom, buf);
        if (current_zoom > 1.05f) {
            // Only show zoom label if auto-hide is not active
            if (!ui_touch_is_auto_hide_active()) {
                lv_obj_remove_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);
            }
            last_zoom_label_ms = millis();
        } else {
            lv_obj_add_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);
        }
        last_displayed_zoom = current_zoom;
    }
    if (ui_LblZoom && !lv_obj_has_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN)) {
        if (millis() - last_zoom_label_ms > ZOOM_LABEL_HIDE_MS) {
            lv_obj_add_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Render earthquake dots when map changes OR new data arrives
    // quake_data_ready: new data just loaded, need initial render
    // map_changed: view changed (zoom/pan), need to reposition dots
    static bool has_quake_data = false;
    bool need_render = false;

    if (quake_data_ready.exchange(false)) {
        has_quake_data = true;
        need_render = true;  // New data arrived, render immediately
    }

    // Only render on map change if NOT actively interacting
    if (has_quake_data && map_changed && !map_is_panning() && !map_is_pinching()) {
        need_render = true;  // Map changed, reposition dots
    }

    if (need_render) {
        map_render_earthquakes();
    }

    int64_t t_quake = esp_timer_get_time();

    // === MAIN-only detailed stats ===
    uint32_t frame_us = (uint32_t)(t_quake - t0);
    g_main_tick_total_us += frame_us;
    g_main_tick_count++;
    if (frame_us > g_main_tick_max_us) g_main_tick_max_us = frame_us;

    // Detailed breakdown every 5 seconds (for main screen only)
    uint32_t now = millis();
    if (now - g_last_detail_report_ms > 5000 && g_main_tick_count > 0) {
        Serial.printf("[PERF_MAIN] lvgl=%lld map=%lld quake=%lld TOTAL=%lu us (avg=%lu max=%lu n=%lu)\n",
            (long long)(t_lvgl - t0),
            (long long)(t_map - t_pre_map),
            (long long)(t_quake - t_map),
            frame_us,
            (uint32_t)(g_main_tick_total_us / g_main_tick_count),
            g_main_tick_max_us,
            g_main_tick_count);
        // Reset for next period
        g_main_tick_total_us = 0;
        g_main_tick_count = 0;
        g_main_tick_max_us = 0;
        g_last_detail_report_ms = now;
    }

    // Accumulate overall timing (no printing in hot path!)
    g_tick_total_us += frame_us;
    g_tick_count++;

    // Report performance at 1Hz (low-priority, non-blocking)
    if (now - g_last_perf_report_ms >= 1000) {
        const HandeDisplayFlushStats flush = hande_display_flush_stats(true);  // also resets the window
        if (g_tick_count > 0) {
            uint32_t avg_tick_us = g_tick_total_us / g_tick_count;
            uint32_t avg_flush_us = flush.average_us;
            uint32_t avg_pixels = flush.count ? (uint32_t)(flush.bytes / sizeof(lv_color16_t) / flush.count) : 0;
            Serial.printf("[PERF] tick=%lums flush=%lums (max=%lums) pixels=%lu flushes=%lu/s\n",
                avg_tick_us / 1000, avg_flush_us / 1000, flush.max_us / 1000, avg_pixels, flush.count);
        }
        // Reset accumulators
        g_tick_total_us = g_tick_count = 0;
        g_last_perf_report_ms = now;
    }
}

void ui_show_loading(const char* text, bool timeout, const char* icon) {
    if (ui_LoaderPanel) {
        lv_obj_remove_flag(ui_LoaderPanel, LV_OBJ_FLAG_HIDDEN);
        if (ui_LblLoader) {
            lv_obj_remove_flag(ui_LblLoader, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(ui_LblLoader, text);
            lv_obj_invalidate(ui_LblLoader);
        }
    }
}

void ui_hide_loading() {
    if (ui_LoaderPanel) {
        lv_obj_add_flag(ui_LoaderPanel, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_LblLoader) {
        lv_label_set_text(ui_LblLoader, "");  // Clear text to prevent ghost text
        lv_obj_add_flag(ui_LblLoader, LV_OBJ_FLAG_HIDDEN);  // Also hide the label itself
    }
}

void ui_update_loading_text(const char* text) {
    if (splash_screen_is_active() && text) {
        int prog = -1;
        if (strstr(text, "WiFi") != nullptr || strstr(text, "wifi") != nullptr) prog = 35;
        else if (strstr(text, "Syncing") != nullptr || strstr(text, "Regions") != nullptr) prog = 60;
        else if (strstr(text, "Earthquake") != nullptr || strstr(text, "quake") != nullptr) prog = 85;
        else if (strstr(text, "Ready") != nullptr) prog = 100;
        splash_screen_update(text, prog);
    }
    if (ui_LblLoader) {
        lv_label_set_text(ui_LblLoader, text);
        lv_obj_invalidate(ui_LblLoader);
    }
}

void ui_show_splash(uint32_t duration) {
    (void)duration;
    splash_screen_show();
}

// Status bar functions now in ui_status_bar module
// Stubs for compatibility (only for functions not defined elsewhere)
void update_earthquake_display() {}
int quake_current_result_count() { return 1; }
int get_available_earthquake_count() { return 1; }
void load_all_earthquakes() {}
bool is_earthquake_data_loading() { return false; }
bool all_earthquake_data_loaded() { return true; }
void ui_reset_zoom() { map_reset_view(); }
bool ui_is_zoomed() { return map_get_zoom() > 1.1f; }
unsigned long last_earthquake_update = 0;
unsigned long last_interaction_millis = 0;  // For CPU governor

int ui_get_refresh_interval_ms() { return 300000; }
int ui_get_anim_speed_ms() { return 200; }
bool ui_is_panel_open() {
    // The System Menu and the Settings screen count as open panels: map gestures, the dock,
    // auto-hide and idle sleep all stand down while they are on screen.
    return hande_system_menu_active() || hande_settings_is_open() ||
        ui_state != UI_STATE_MAIN || ui_notification_panel_is_open() || ui_quake_detail_is_open();
}
void ui_update_wifi_cache() {
    wifi_cached_connected = wifi_is_connected();
    if (wifi_cached_connected) {
        wifi_cached_ssid = wifi_get_ssid();
        wifi_cached_rssi = wifi_get_rssi();
    } else {
        wifi_cached_ssid = "";
        wifi_cached_rssi = 0;
    }
}

bool ui_wifi_cached_is_connected() {
    return wifi_cached_connected;
}

const char* ui_wifi_cached_ssid() {
    return wifi_cached_ssid.c_str();
}

int ui_wifi_cached_rssi() {
    return wifi_cached_rssi;
}

static void auto_dim_load() {
    Preferences prefs;
    prefs.begin(AUTO_DIM_NVS_NAMESPACE, true);
    auto_dim_enabled = prefs.getBool("enabled", false);
    auto_dim_start_hour = prefs.getChar("start", 22);
    auto_dim_end_hour = prefs.getChar("end", 7);
    prefs.end();
    Serial.printf("[AUTO_DIM] Loaded: enabled=%d, %d:00-%d:00\n",
                  auto_dim_enabled, auto_dim_start_hour, auto_dim_end_hour);
}

static void auto_dim_save() {
    Preferences prefs;
    prefs.begin(AUTO_DIM_NVS_NAMESPACE, false);
    prefs.putBool("enabled", auto_dim_enabled);
    prefs.putChar("start", auto_dim_start_hour);
    prefs.putChar("end", auto_dim_end_hour);
    prefs.end();
    Serial.printf("[AUTO_DIM] Saved: enabled=%d, %d:00-%d:00\n",
                  auto_dim_enabled, auto_dim_start_hour, auto_dim_end_hour);
}

// Power management functions now in ui_power module
// Notification functions now in ui_notifications_panel module
