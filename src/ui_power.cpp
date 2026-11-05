#include "ui_power.h"
#include "ui_touch.h"
#include "ui_sync.h"
#include "ui_state.h"
#include "ui_button_dock.h"
#include "settings_manager.h"
#include "ui_status_bar.h"
#include "hande_time.h"
#include <Preferences.h>
#include <time.h>
#include <atomic>

// External UI objects from ui_display.cpp
extern lv_obj_t* ui_LblStatus;
extern lv_obj_t* ui_BatteryContainer;
extern lv_obj_t* ui_LblZoom;
extern lv_obj_t* ui_TimeContainer;
extern lv_obj_t* ui_OverlayPanel;

// External functions
extern UiState ui_get_state();
extern bool ui_is_panel_open();

// Auto-dim NVS namespace
#define AUTO_DIM_NVS_NAMESPACE "gql_auto_dim"
#define NIGHT_BRIGHTNESS 30

// Auto-dim state
static bool auto_dim_enabled = false;
static int8_t auto_dim_start_hour = 22;
static int8_t auto_dim_end_hour = 7;
static bool auto_dim_active = false;
static bool auto_dim_save_pending = false;

// Auto-hide state (accessed from Core 0 and Core 1)
// Using std::atomic for proper thread-safe access across cores
static std::atomic<bool> auto_hide_active{false};

// Forward declarations
static void auto_dim_load();
static void auto_dim_save();

// --- Public API Implementation ---

void ui_power_init() {
    auto_dim_load();
}

void ui_apply_auto_dim() {
    if (!auto_dim_enabled) {
        if (auto_dim_active) {
            auto_dim_active = false;
            ui_queue_set_brightness(settings_get()->brightness);
            Serial.println("[AUTO_DIM] Disabled - restored user brightness");
        }
        return;
    }

    // Called every loop() pass and from settings callbacks on the LVGL task.
    struct tm timeinfo;
    if (!hande_time_local(&timeinfo)) {  // no local time yet: no night schedule
        return;
    }
    int current_hour = timeinfo.tm_hour;

    bool should_dim = false;
    if (auto_dim_start_hour > auto_dim_end_hour) {
        if (current_hour >= auto_dim_start_hour || current_hour < auto_dim_end_hour) {
            should_dim = true;
        }
    } else if (auto_dim_start_hour < auto_dim_end_hour) {
        if (current_hour >= auto_dim_start_hour && current_hour < auto_dim_end_hour) {
            should_dim = true;
        }
    }

    if (should_dim && !auto_dim_active) {
        auto_dim_active = true;
        ui_queue_set_brightness(NIGHT_BRIGHTNESS);
        Serial.printf("[AUTO_DIM] Night mode ON (hour=%d, dim=%d)\n", current_hour, NIGHT_BRIGHTNESS);
    } else if (!should_dim && auto_dim_active) {
        auto_dim_active = false;
        ui_queue_set_brightness(settings_get()->brightness);
        Serial.printf("[AUTO_DIM] Night mode OFF (hour=%d, brightness=%d)\n",
                      current_hour, settings_get()->brightness);
    }
}

bool ui_auto_dim_is_enabled() {
    return auto_dim_enabled;
}

bool ui_auto_dim_is_active() {
    return auto_dim_active;
}

int ui_auto_dim_get_start_hour() {
    return auto_dim_start_hour;
}

int ui_auto_dim_get_end_hour() {
    return auto_dim_end_hour;
}

void ui_auto_dim_set_enabled(bool enabled) {
    auto_dim_enabled = enabled;
    auto_dim_save_pending = true;
    ui_apply_auto_dim();
}

void ui_auto_dim_set_schedule(int start_hour, int end_hour) {
    auto_dim_start_hour = start_hour;
    auto_dim_end_hour = end_hour;
    auto_dim_save_pending = true;
    ui_apply_auto_dim();
}

void ui_check_idle_sleep() {
    AppSettings* settings = settings_get();
    int timeout_sec = settings->auto_dim_timeout;

    // Skip if disabled (0 = never sleep)
    if (timeout_sec == 0) {
        return;
    }

    // Don't sleep if night dimming is active (night mode takes priority)
    if (auto_dim_active) {
        return;
    }

    // Don't sleep if panel is open (settings, notifications)
    if (ui_is_panel_open()) {
        return;
    }

    uint32_t now = millis();
    uint32_t idle_time_ms = now - ui_touch_get_last_touch_millis();

    // Enter sleep after timeout
    if (!ui_touch_is_idle_sleep_active() && idle_time_ms > (timeout_sec * 1000)) {
        ui_touch_set_idle_sleep_active(true);
        ui_touch_set_brightness_before_sleep(settings->brightness);
        ui_queue_set_brightness(0);  // Screen off
        Serial.printf("[IDLE_SLEEP] Screen off after %d seconds idle\n", timeout_sec);
    }
}

void ui_check_auto_hide() {
    AppSettings* settings = settings_get();
    int timeout_sec = settings->auto_hide_timeout;

    // Skip if disabled (0 = never auto-hide)
    if (timeout_sec == 0) {
        return;
    }

    // Don't auto-hide if panel is open (settings, notifications)
    if (ui_is_panel_open()) {
        return;
    }

    // Don't auto-hide if idle sleep is active (screen already off)
    if (ui_touch_is_idle_sleep_active()) {
        return;
    }

    uint32_t now = millis();
    uint32_t idle_time_ms = now - ui_touch_get_last_ui_interaction_millis();

    // Hide components after timeout - queue event for LVGL task (thread-safe)
    if (!auto_hide_active.load() && idle_time_ms > (timeout_sec * 1000)) {
        auto_hide_active.store(true);
        ui_touch_set_auto_hide_active(true);
        ui_queue_auto_hide_hide();  // Queue event, process on LVGL task
        Serial.printf("[AUTO_HIDE] Components hidden after %d seconds idle\n", timeout_sec);
    }
}

void ui_show_all_components() {
    if (!auto_hide_active.load()) {
        return;  // Already visible
    }

    auto_hide_active.store(false);
    ui_touch_set_auto_hide_active(false);
    ui_queue_auto_hide_show();  // Queue event, process on LVGL task
    Serial.println("[AUTO_HIDE] All components shown");
}

void ui_reset_auto_hide_timer() {
    ui_touch_update_interaction_time();
}

void ui_check_deferred_saves() {
    static uint32_t last_save_time = 0;
    uint32_t now = millis();
    if (auto_dim_save_pending && (now - last_save_time > 500)) {
        auto_dim_save();
        auto_dim_save_pending = false;
        last_save_time = now;
    }

    // Check settings deferred saves (brightness slider)
    extern void ui_settings_check_deferred_saves();
    ui_settings_check_deferred_saves();
}

// --- Private Implementation ---

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

// ============================================================================
// INTERNAL LVGL FUNCTIONS (called from LVGL task via ui_sync queue)
// These functions perform actual LVGL operations and MUST only be called
// from the LVGL task (Core 1) to ensure thread safety.
// ============================================================================

void ui_auto_hide_hide_internal() {
    // Hide components WITHOUT individual invalidations to reduce rendering overhead
    // Single screen-wide invalidation at end is more efficient

    if (ui_LblStatus) {
        lv_obj_add_flag(ui_LblStatus, LV_OBJ_FLAG_HIDDEN);
    }

    if (ui_BatteryContainer) {
        lv_obj_add_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
    }
    ui_battery_set_hud_hidden(true);  // a low (red) battery stays visible

    if (ui_LblZoom) {
        lv_obj_add_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);
    }

    // Hide time/date ONLY if not set to "always visible"
    AppSettings* settings = settings_get();
    if (!settings->time_always_visible && ui_TimeContainer) {
        lv_obj_add_flag(ui_TimeContainer, LV_OBJ_FLAG_HIDDEN);
    }

    // Hide dock buttons (also modified to skip individual invalidations)
    ui_dock_hide();

    // Single screen-wide invalidation instead of per-element
    lv_obj_invalidate(lv_screen_active());
}

// --- HUD steps aside while a finger drags the map ---
// A pan redraws the whole screen every frame, and the HUD (clock, status pill,
// battery, dock) cost ~18 ms of each such frame (measured with the profiler's PAN_HUDOFF variant).
// opa_layered 0 makes LVGL skip the overlay panel with all its children, and the
// dock, without touching their HIDDEN flags (auto-hide and the zoom-label timer
// own those); removing the local style afterwards restores exactly what was there.
static bool s_hud_drag_hidden = false;

void ui_hud_drag_begin() {
    if (s_hud_drag_hidden) return;
    s_hud_drag_hidden = true;
    if (ui_OverlayPanel) lv_obj_set_style_opa_layered(ui_OverlayPanel, LV_OPA_TRANSP, 0);
    ui_dock_set_drag_hidden(true);
}

void ui_hud_drag_end() {
    if (!s_hud_drag_hidden) return;
    s_hud_drag_hidden = false;
    if (ui_OverlayPanel) lv_obj_remove_local_style_prop(ui_OverlayPanel, LV_STYLE_OPA_LAYERED, 0);
    ui_dock_set_drag_hidden(false);
}

void ui_auto_hide_show_internal() {
    AppSettings* settings = settings_get();

    // Show components WITHOUT individual invalidations to reduce rendering overhead
    // Single screen-wide invalidation at end is more efficient

    ui_battery_set_hud_hidden(false);
    if (ui_LblStatus) {
        lv_obj_remove_flag(ui_LblStatus, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_BatteryContainer && settings->show_battery) {
        lv_obj_remove_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_LblZoom) {
        lv_obj_remove_flag(ui_LblZoom, LV_OBJ_FLAG_HIDDEN);
    }

    // Show time/date if not set to "always visible" (if always visible, they never hid)
    if (!settings->time_always_visible && ui_TimeContainer) {
        lv_obj_remove_flag(ui_TimeContainer, LV_OBJ_FLAG_HIDDEN);
    }

    // Show dock buttons (also modified to skip individual invalidations)
    ui_dock_show();

    // Single screen-wide invalidation instead of per-element
    lv_obj_invalidate(lv_screen_active());
}
