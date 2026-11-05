#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include <Arduino.h>
#include <lvgl.h>
#include "ui_state.h"

// --- UI STATE MACHINE ---
// Set UI state (must be called inside ui_sync_lock() or from ui_process_queue())
void ui_set_state(UiState state);

// Get current UI state (thread-safe read)
UiState ui_get_state();

// Request WiFi setup screen via state machine
// from_settings: true if coming from settings (shows back button)
void ui_request_wifi_setup(bool from_settings);

// --- INIT ---
void ui_display_create_gfx();
void ui_init_display_and_touch(); // panel, LVGL core, buffers, touch; builds no screens
void ui_init();
void ui_tick(); // Keep calling this in loop()

// --- HIGH LEVEL CONTROLS ---
// Switch to "Clean Mode" (Show map, hide overlays after delay)
void ui_enter_clean_mode();

// Show the spinner (Call this immediately when user swipes)
// with_timeout: true = start 90s network timeout, false = no timeout (for startup)
// icon: optional icon to show (default = earthquake icon, use ICON_WIFI for WiFi tasks)
void ui_show_loading(const char* status_text = "Connecting...", bool with_timeout = true, const char* icon = nullptr);

// Just update the text below the spinner (for progress)
void ui_update_loading_text(const char* text);

// Hide the loading panel
void ui_hide_loading();

// Update the bottom status bar (earthquake data status)
void ui_update_status(const char* text);

// Update the clock
void ui_update_clock();

// Update battery indicator
void ui_update_battery();

// Update bottom status label with connection/sync progress
void ui_update_status_text(const char* status_type);

// Zoom control
void ui_reset_zoom();
bool ui_is_zoomed();  // Returns true if image is zoomed in (above base zoom)

// Settings getters
int ui_get_anim_speed_ms();
int ui_get_refresh_interval_ms();
int ui_get_brightness();

// Auto-dim schedule
void ui_apply_auto_dim();  // Call from main loop (checks time and applies dim)
void ui_check_idle_sleep();  // Call from main loop (idle-based screen sleep)
void ui_check_deferred_saves();  // Call from main loop (handles pending NVS writes)

// Auto-hide system - clean map view with hidden overlays
void ui_check_auto_hide();  // Call from main loop (checks timer and hides components)
void ui_show_all_components();  // Show all HUD components (called on touch)
void ui_reset_auto_hide_timer();  // Reset timer without showing components

void ui_update_wifi_cache();  // Call from main loop (caches WiFi status safely)
bool ui_wifi_cached_is_connected();
const char* ui_wifi_cached_ssid();
int ui_wifi_cached_rssi();
bool ui_auto_dim_is_enabled();
void ui_auto_dim_set_enabled(bool enabled);
int ui_auto_dim_get_start_hour();
int ui_auto_dim_get_end_hour();
void ui_auto_dim_set_schedule(int start_hour, int end_hour);

// CPU Governor - Last user interaction time (for power management)
extern unsigned long last_interaction_millis;

// Check if any panel (settings, notifications) is open
bool ui_is_panel_open();

void ui_toggle_notifications();
void ui_update_status_sync();
void ui_apply_main_theme();

// Button Dock - Navigation buttons on left/right edges
void ui_dock_create_internal();  // Internal: called from create_main_screen()
void ui_dock_destroy_internal(); // Internal: called when leaving MAIN state

// Touch recovery (reinitialize touch controller if it stops reporting)
void ui_touch_reinit(const char* reason);
// Temporarily throttle touch polling to reduce I2C load during heavy UI creation
void ui_touch_throttle(uint32_t interval_ms, uint32_t duration_ms);

// Splash screen - shows logo for specified duration (blocking)
void ui_show_splash(uint32_t duration_ms = 2000);

// Access for image flipping
extern lv_obj_t *ui_ImgMap;

// UI object access for extracted modules
extern lv_obj_t *ui_TimeContainer;
extern lv_obj_t *ui_LblAmPm;
extern lv_obj_t *ui_LblCharging;
extern lv_obj_t *ui_BatteryContainer;
extern lv_obj_t *ui_LblStatus;
extern lv_obj_t *ui_LblBatteryIcon;
extern lv_obj_t *ui_LblBattery;
extern lv_obj_t *ui_LblZoom;
extern lv_obj_t *ui_LblTime;
extern lv_obj_t *ui_LblDate;
extern lv_obj_t *ui_OverlayPanel;
extern lv_obj_t *ui_ScreenMain;
extern lv_obj_t *ui_ImgContainer;
extern lv_obj_t *ui_LoaderPanel;

#endif // UI_DISPLAY_H
