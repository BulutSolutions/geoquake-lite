#include "settings_manager.h"
#include "theme.h"
#include "ui_display.h"
#include "ui_button_dock.h"
#include "ui_settings.h"
#include "hande_settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>

// NVS namespace for settings storage
#define NVS_NAMESPACE "gql_cfg"

// NVS keys for each setting
#define KEY_THEME_PRESET     "theme_preset"
#define KEY_CUSTOM_COLOR     "custom_color"
#define KEY_MIN_ZOOM         "min_zoom"
#define KEY_MAX_ZOOM         "max_zoom"
#define KEY_PAN_SENS         "pan_sens"
#define KEY_PINCH_ZOOM       "pinch_zoom"
#define KEY_LABEL_DETAIL     "label_detail"
#define KEY_MAG_MIN          "mag_min"
#define KEY_TIME_RANGE       "time_range"
#define KEY_GLOBAL_REGION    "global_region"
#define KEY_BRIGHTNESS       "brightness"
#define KEY_AUTO_DIM         "auto_dim"
#define KEY_CLOCK_24H        "clock_24h"
#define KEY_SHOW_BATTERY     "show_battery"
#define KEY_SHOW_DATE        "show_date"
#define KEY_AUTO_HIDE        "auto_hide"
#define KEY_TIME_VISIBLE     "time_visible"
#define KEY_SOUND_EFFECT     "sound_effect"
#define KEY_SOUND_VOLUME     "sound_volume"
#define KEY_SOUND_MUTED      "sound_muted"
#define KEY_WIFI_AUTO_OFF    "wifi_off_sec"
#define KEY_SYNC_INTERVAL    "sync_interval"

// Global settings instance
static AppSettings g_settings;
static Preferences prefs;

// ===== INITIALIZATION =====

void settings_init() {
    Serial.println("[SETTINGS] Initializing settings manager");

    // Try to load from NVS first
    if (!settings_load_from_nvs()) {
        // NVS empty or corrupted, use defaults
        Serial.println("[SETTINGS] No saved settings found, loading defaults");
        settings_load_defaults();
        settings_save_to_nvs();  // Save defaults to NVS
    }

    // Apply theme from loaded settings
    settings_apply_theme();

    Serial.println("[SETTINGS] Settings manager initialized");
}

void settings_load_defaults() {
    Serial.println("[SETTINGS] Loading factory defaults");

    // Theme defaults
    g_settings.theme_preset = 0;  // THEME_CYAN (default)
    g_settings.custom_color = 0x00AFF0;  // Cyan

    // Map defaults
    g_settings.min_zoom = 2;
    g_settings.max_zoom = 10;
    g_settings.pan_sensitivity = 1.0f;
    g_settings.pinch_zoom_enabled = true;
    g_settings.label_detail = 1;  // Medium

    // Earthquake filter defaults
    g_settings.mag_min = 1.0f;
    g_settings.time_range = 1;  // 24 hours
    g_settings.global_region = true;

    // Display defaults
    g_settings.brightness = 200;  // ~78% brightness
    g_settings.auto_dim_timeout = 30;  // 30 seconds
    g_settings.clock_24h = true;  // 24-hour format
    g_settings.show_battery = true;
    g_settings.show_date = true;
    g_settings.auto_hide_timeout = 30;  // 30 seconds default
    g_settings.time_always_visible = false;  // Auto-hide by default

    // Sound defaults
    g_settings.sound_effect_id = 0;  // Beep
    g_settings.sound_volume = 70;  // Match codec output default intent
    g_settings.sound_muted = false;

    // Advanced defaults
    g_settings.wifi_auto_off_seconds = 30;  // 30 seconds
    g_settings.sync_interval_minutes = 10;  // 10 minutes
}

// ===== PERSISTENCE =====

bool settings_load_from_nvs() {
    if (!prefs.begin(NVS_NAMESPACE, true)) {  // Read-only
        Serial.println("[SETTINGS] Failed to open NVS");
        return false;
    }

    // Check if settings exist by trying to read one key
    if (!prefs.isKey(KEY_THEME_PRESET)) {
        prefs.end();
        return false;  // No settings saved yet
    }

    // Load all settings from NVS
    g_settings.theme_preset = prefs.getInt(KEY_THEME_PRESET, 0);
    g_settings.custom_color = prefs.getUInt(KEY_CUSTOM_COLOR, 0x00AFF0);

    g_settings.min_zoom = prefs.getInt(KEY_MIN_ZOOM, 2);
    g_settings.max_zoom = prefs.getInt(KEY_MAX_ZOOM, 10);
    g_settings.pan_sensitivity = prefs.getFloat(KEY_PAN_SENS, 1.0f);
    g_settings.pinch_zoom_enabled = prefs.getBool(KEY_PINCH_ZOOM, true);
    g_settings.label_detail = prefs.getInt(KEY_LABEL_DETAIL, 1);

    g_settings.mag_min = prefs.getFloat(KEY_MAG_MIN, 1.0f);
    g_settings.time_range = prefs.getInt(KEY_TIME_RANGE, 1);
    g_settings.global_region = prefs.getBool(KEY_GLOBAL_REGION, true);

    g_settings.brightness = prefs.getInt(KEY_BRIGHTNESS, 200);
    g_settings.auto_dim_timeout = prefs.getInt(KEY_AUTO_DIM, 30);
    g_settings.clock_24h = prefs.getBool(KEY_CLOCK_24H, true);
    g_settings.show_battery = prefs.getBool(KEY_SHOW_BATTERY, true);
    g_settings.show_date = prefs.getBool(KEY_SHOW_DATE, true);
    g_settings.auto_hide_timeout = prefs.getInt(KEY_AUTO_HIDE, 30);
    g_settings.time_always_visible = prefs.getBool(KEY_TIME_VISIBLE, false);

    g_settings.sound_effect_id = prefs.getInt(KEY_SOUND_EFFECT, 0);
    g_settings.sound_volume = prefs.getInt(KEY_SOUND_VOLUME, 70);
    g_settings.sound_muted = prefs.getBool(KEY_SOUND_MUTED, false);

    g_settings.wifi_auto_off_seconds = prefs.getInt(KEY_WIFI_AUTO_OFF, 30);
    g_settings.sync_interval_minutes = prefs.getInt(KEY_SYNC_INTERVAL, 10);

    prefs.end();

    // WiFi auto-off: 0 (Always On), 10, 30, 60 seconds
    if (g_settings.wifi_auto_off_seconds != 0 &&
        g_settings.wifi_auto_off_seconds != 10 &&
        g_settings.wifi_auto_off_seconds != 30 &&
        g_settings.wifi_auto_off_seconds != 60) {
        Serial.printf("[SETTINGS] Invalid wifi_auto_off_seconds %d, defaulting to 30\n",
                      g_settings.wifi_auto_off_seconds);
        g_settings.wifi_auto_off_seconds = 30;
    }

    // Sync interval: 5, 10, 15, 30 minutes
    if (g_settings.sync_interval_minutes != 5 &&
        g_settings.sync_interval_minutes != 10 &&
        g_settings.sync_interval_minutes != 15 &&
        g_settings.sync_interval_minutes != 30) {
        Serial.printf("[SETTINGS] Invalid sync_interval_minutes %d, defaulting to 10\n",
                      g_settings.sync_interval_minutes);
        g_settings.sync_interval_minutes = 10;
    }

    if (g_settings.time_range < 0 || g_settings.time_range > 2) {
        Serial.printf("[SETTINGS] Invalid time_range %d, defaulting to 1\n",
                      g_settings.time_range);
        g_settings.time_range = 1;
    }

    if (g_settings.sound_effect_id < 0 || g_settings.sound_effect_id > 18) {
        Serial.printf("[SETTINGS] Invalid sound_effect_id %d, defaulting to 0\n",
                      g_settings.sound_effect_id);
        g_settings.sound_effect_id = 0;
    }
    if (g_settings.sound_volume < 0 || g_settings.sound_volume > 100) {
        Serial.printf("[SETTINGS] Invalid sound_volume %d, defaulting to 70\n",
                      g_settings.sound_volume);
        g_settings.sound_volume = 70;
    }

    Serial.println("[SETTINGS] Settings loaded from NVS");
    return true;
}

void settings_save_to_nvs() {
    if (!prefs.begin(NVS_NAMESPACE, false)) {  // Read-write
        Serial.println("[SETTINGS] Failed to open NVS for writing");
        return;
    }

    // Save all settings to NVS
    prefs.putInt(KEY_THEME_PRESET, g_settings.theme_preset);
    prefs.putUInt(KEY_CUSTOM_COLOR, g_settings.custom_color);

    prefs.putInt(KEY_MIN_ZOOM, g_settings.min_zoom);
    prefs.putInt(KEY_MAX_ZOOM, g_settings.max_zoom);
    prefs.putFloat(KEY_PAN_SENS, g_settings.pan_sensitivity);
    prefs.putBool(KEY_PINCH_ZOOM, g_settings.pinch_zoom_enabled);
    prefs.putInt(KEY_LABEL_DETAIL, g_settings.label_detail);

    prefs.putFloat(KEY_MAG_MIN, g_settings.mag_min);
    prefs.putInt(KEY_TIME_RANGE, g_settings.time_range);
    prefs.putBool(KEY_GLOBAL_REGION, g_settings.global_region);

    prefs.putInt(KEY_BRIGHTNESS, g_settings.brightness);
    prefs.putInt(KEY_AUTO_DIM, g_settings.auto_dim_timeout);
    prefs.putBool(KEY_CLOCK_24H, g_settings.clock_24h);
    prefs.putBool(KEY_SHOW_BATTERY, g_settings.show_battery);
    prefs.putBool(KEY_SHOW_DATE, g_settings.show_date);
    prefs.putInt(KEY_AUTO_HIDE, g_settings.auto_hide_timeout);
    prefs.putBool(KEY_TIME_VISIBLE, g_settings.time_always_visible);

    prefs.putInt(KEY_SOUND_EFFECT, g_settings.sound_effect_id);
    prefs.putInt(KEY_SOUND_VOLUME, g_settings.sound_volume);
    prefs.putBool(KEY_SOUND_MUTED, g_settings.sound_muted);

    prefs.putInt(KEY_WIFI_AUTO_OFF, g_settings.wifi_auto_off_seconds);
    prefs.putInt(KEY_SYNC_INTERVAL, g_settings.sync_interval_minutes);

    prefs.end();

    Serial.println("[SETTINGS] Settings saved to NVS");
}

// ===== ACCESS =====

AppSettings* settings_get() {
    return &g_settings;
}

// ===== THEME APPLICATION =====

void settings_apply_theme() {
    Serial.printf("[SETTINGS] Applying theme: preset=%d, custom=0x%06X\n",
                  g_settings.theme_preset, g_settings.custom_color);

    if (g_settings.theme_preset >= 0 && g_settings.theme_preset < THEME_PRESET_COUNT) {
        // Apply preset theme
        theme_set_preset((ThemePreset)g_settings.theme_preset);
    } else {
        // Apply custom color theme
        uint8_t r = (g_settings.custom_color >> 16) & 0xFF;
        uint8_t g = (g_settings.custom_color >> 8) & 0xFF;
        uint8_t b = g_settings.custom_color & 0xFF;
        theme_set_custom_color(g_settings.custom_color);
        theme_set_preset(THEME_CUSTOM);
        Serial.printf("[SETTINGS] Custom color: R=%d G=%d B=%d\n", r, g, b);
    }

    // Force UI redraw if LVGL is initialized
    if (lv_is_initialized()) {
        ui_apply_main_theme();
        ui_dock_apply_theme();
        hande_settings_set_accent(ui_settings_theme_accent());  // Settings screen accent
        lv_obj_invalidate(lv_screen_active());
    }
}

// ===== UTILITY =====

void settings_reset_to_defaults(bool apply_theme_now) {
    Serial.println("[SETTINGS] Resetting to factory defaults");

    // Clear NVS
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
    }

    // Load defaults
    settings_load_defaults();

    // Apply theme only when caller is running on the LVGL task or needs live UI refresh
    if (apply_theme_now) {
        settings_apply_theme();
    }

    // Save defaults to NVS
    settings_save_to_nvs();

    Serial.println("[SETTINGS] Reset complete");
}

void settings_print_debug() {
    Serial.println("========== SETTINGS DEBUG ==========");

    Serial.printf("Theme Preset: %d\n", g_settings.theme_preset);
    Serial.printf("Custom Color: 0x%06X\n", g_settings.custom_color);

    Serial.printf("Min Zoom: %d\n", g_settings.min_zoom);
    Serial.printf("Max Zoom: %d\n", g_settings.max_zoom);
    Serial.printf("Pan Sensitivity: %.2f\n", g_settings.pan_sensitivity);
    Serial.printf("Pinch Zoom: %s\n", g_settings.pinch_zoom_enabled ? "ON" : "OFF");
    Serial.printf("Label Detail: %d\n", g_settings.label_detail);

    Serial.printf("Mag Min: %.1f\n", g_settings.mag_min);
    Serial.printf("Time Range: %d\n", g_settings.time_range);
    Serial.printf("Global Region: %s\n", g_settings.global_region ? "YES" : "NO");

    Serial.printf("Brightness: %d\n", g_settings.brightness);
    Serial.printf("Auto-dim Timeout: %d s\n", g_settings.auto_dim_timeout);
    Serial.printf("24h Clock: %s\n", g_settings.clock_24h ? "YES" : "NO");
    Serial.printf("Show Battery: %s\n", g_settings.show_battery ? "YES" : "NO");
    Serial.printf("Show Date: %s\n", g_settings.show_date ? "YES" : "NO");
    Serial.printf("Auto-hide Timeout: %d s\n", g_settings.auto_hide_timeout);
    Serial.printf("Time Always Visible: %s\n", g_settings.time_always_visible ? "YES" : "NO");

    Serial.printf("Sound Effect: %d\n", g_settings.sound_effect_id);
    Serial.printf("Sound Volume: %d\n", g_settings.sound_volume);
    Serial.printf("Sound Muted: %s\n", g_settings.sound_muted ? "YES" : "NO");

    Serial.printf("WiFi Auto-Off: %d s\n", g_settings.wifi_auto_off_seconds);
    Serial.printf("Sync Interval: %d min\n", g_settings.sync_interval_minutes);

    Serial.println("====================================");
}
