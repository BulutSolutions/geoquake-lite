#include "settings_viewmodel.h"
#include <cmath>

// Global viewmodel instance
static SettingsViewModel s_viewmodel;

SettingsViewModel* settings_viewmodel_get() {
    return &s_viewmodel;
}

// ============================================================================
// Loading & Saving
// ============================================================================

void SettingsViewModel::load() {
    // Copy from settings_manager
    AppSettings* current = settings_get();
    if (current) {
        data_ = *current;
        saved_data_ = data_;
    }
}

bool SettingsViewModel::save() {
    if (!validate()) {
        return false;
    }
    
    // Copy to settings_manager and persist
    AppSettings* current = settings_get();
    if (current) {
        *current = data_;
        settings_save_to_nvs();
        saved_data_ = data_;
        return true;
    }
    return false;
}

// ============================================================================
// Setters with Validation
// ============================================================================

bool SettingsViewModel::set_brightness(int value) {
    if (!is_valid_brightness(value)) {
        return false;
    }
    data_.brightness = value;
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_theme_preset(int preset) {
    if (!is_valid_theme_preset(preset)) {
        return false;
    }
    data_.theme_preset = preset;
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_custom_color(uint32_t color) {
    data_.custom_color = color;
    data_.theme_preset = -1; // Switch to custom mode
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_mag_min(float mag) {
    if (!is_valid_magnitude(mag)) {
        return false;
    }
    data_.mag_min = mag;
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_time_range(int range) {
    if (range < 0 || range > 2) {
        return false;
    }
    data_.time_range = range;
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_pan_sensitivity(float sensitivity) {
    if (sensitivity < 0.1f || sensitivity > 5.0f) {
        return false;
    }
    data_.pan_sensitivity = sensitivity;
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_zoom_limits(int min_zoom, int max_zoom) {
    if (!is_valid_zoom_range(min_zoom, max_zoom)) {
        return false;
    }
    data_.min_zoom = min_zoom;
    data_.max_zoom = max_zoom;
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_auto_dim_timeout(int seconds) {
    // Valid: 0 (never), 15, 30, 60, 300
    const int valid_timeouts[] = {0, 15, 30, 60, 300};
    bool valid = false;
    for (int t : valid_timeouts) {
        if (seconds == t) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        return false;
    }
    data_.auto_dim_timeout = seconds;
    notify_if_changed();
    return true;
}

bool SettingsViewModel::set_clock_format(bool use_24h) {
    data_.clock_24h = use_24h;
    notify_if_changed();
    return true;
}

// ============================================================================
// Validation
// ============================================================================

bool SettingsViewModel::validate() const {
    return is_valid_brightness(data_.brightness) &&
           is_valid_zoom_range(data_.min_zoom, data_.max_zoom) &&
           is_valid_magnitude(data_.mag_min) &&
           is_valid_theme_preset(data_.theme_preset);
}

bool SettingsViewModel::is_modified() const {
    // Compare key fields
    return data_.brightness != saved_data_.brightness ||
           data_.theme_preset != saved_data_.theme_preset ||
           data_.custom_color != saved_data_.custom_color ||
           data_.mag_min != saved_data_.mag_min ||
           data_.time_range != saved_data_.time_range ||
           data_.min_zoom != saved_data_.min_zoom ||
           data_.max_zoom != saved_data_.max_zoom ||
           data_.pan_sensitivity != saved_data_.pan_sensitivity ||
           data_.auto_dim_timeout != saved_data_.auto_dim_timeout ||
           data_.clock_24h != saved_data_.clock_24h;
}

void SettingsViewModel::reset_to_defaults() {
    settings_load_defaults();
    load(); // Reload from defaults
    notify_if_changed();
}

// ============================================================================
// Static Validation Helpers
// ============================================================================

bool SettingsViewModel::is_valid_brightness(int value) {
    return value >= 0 && value <= 255;
}

bool SettingsViewModel::is_valid_zoom_range(int min_zoom, int max_zoom) {
    return min_zoom >= 1 && min_zoom <= 32 &&
           max_zoom >= 1 && max_zoom <= 32 &&
           min_zoom <= max_zoom;
}

bool SettingsViewModel::is_valid_magnitude(float mag) {
    return mag >= 1.0f && mag <= 10.0f;
}

bool SettingsViewModel::is_valid_theme_preset(int preset) {
    return preset >= -1 && preset <= 5; // -1 = custom, 0-5 = presets
}

// ============================================================================
// Private Helpers
// ============================================================================

void SettingsViewModel::notify_if_changed() {
    if (change_callback_) {
        change_callback_();
    }
}
