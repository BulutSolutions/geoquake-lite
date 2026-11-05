#pragma once
#include <stdint.h>

/**
 * Settings Manager
 *
 * Centralized settings management with NVS (Non-Volatile Storage) persistence.
 * All app configuration is stored in the AppSettings structure and saved to flash.
 */

// Settings structure - all app configuration in one place
typedef struct {
    // ===== THEME =====
    int theme_preset;        // 0-5 for preset themes, -1 for custom
    uint32_t custom_color;   // RGB888 format (0xRRGGBB) for custom theme

    // ===== MAP OPTIONS =====
    int min_zoom;            // Minimum zoom level (1-10)
    int max_zoom;            // Maximum zoom level (1-32)
    float pan_sensitivity;   // Pan gesture sensitivity multiplier (0.5-2.0)
    bool pinch_zoom_enabled; // Enable/disable pinch-to-zoom gesture
    int label_detail;        // Label detail level: 0=low, 1=medium, 2=high

    // ===== EARTHQUAKE FILTERS =====
    float mag_min;           // Minimum magnitude to display (1.0-10.0)
    int time_range;          // Time range: 0=1hr, 1=24hr, 2=7days
    bool global_region;      // true=global, false=custom bounds

    // ===== DISPLAY SETTINGS =====
    int brightness;          // Display brightness (0-255)
    int auto_dim_timeout;    // Auto-dim timeout in seconds (0=never, 15, 30, 60, 300)
    bool clock_24h;          // Clock format: true=24h, false=12h
    bool show_battery;       // Show battery indicator on HUD
    bool show_date;          // Show date on HUD
    int auto_hide_timeout;   // Auto-hide HUD timeout: 0=never, 30, 60, 120, 300 (seconds)
    bool time_always_visible; // Keep time/date visible even when auto-hide triggers

    // ===== SOUND SETTINGS =====
    int sound_effect_id;     // SoundEffect enum value, 0-18
    int sound_volume;        // Sound volume (0-100)
    bool sound_muted;        // Silent mode toggle

    // ===== ADVANCED SETTINGS =====
    int wifi_auto_off_seconds;   // WiFi auto-off delay: 10, 30, 60 (default: 30)
    int sync_interval_minutes;   // Background sync interval: 5, 10, 15, 30 (default: 10)
} AppSettings;

// ===== INITIALIZATION =====

/**
 * Initialize settings system and load from NVS
 * Call this ONCE at startup, before ui_init()
 */
void settings_init();

/**
 * Load default settings values
 * Called automatically by settings_init() if NVS is empty
 */
void settings_load_defaults();

// ===== PERSISTENCE =====

/**
 * Load settings from NVS (Non-Volatile Storage)
 * Returns true if settings were loaded, false if using defaults
 */
bool settings_load_from_nvs();

/**
 * Save current settings to NVS
 * Call after any setting changes to persist across reboots
 */
void settings_save_to_nvs();

// ===== ACCESS =====

/**
 * Get pointer to current settings structure
 * Use this to read/modify settings
 *
 * Example:
 *   AppSettings* s = settings_get();
 *   s->brightness = 200;
 *   settings_save_to_nvs();
 */
AppSettings* settings_get();

// ===== THEME APPLICATION =====

/**
 * Apply current theme settings to the UI
 * Call after loading settings or changing theme
 * This will update the theme system and redraw the UI
 */
void settings_apply_theme();

// ===== UTILITY =====

/**
 * Reset all settings to factory defaults
 * This will clear NVS and reload defaults
 * @param apply_theme_now Whether to repaint the live LVGL theme immediately.
 */
void settings_reset_to_defaults(bool apply_theme_now = true);

/**
 * Print current settings to serial (for debugging)
 */
void settings_print_debug();
