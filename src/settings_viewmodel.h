#pragma once

#ifndef SETTINGS_VIEWMODEL_H
#define SETTINGS_VIEWMODEL_H

#include <cstdint>
#include <functional>
#include "settings_manager.h"

// SettingsViewModel for GeoQuake
// Separates settings data management from UI presentation
// Enables unit testing and cleaner UI code

class SettingsViewModel {
public:
    // Load from persistent storage (settings_manager)
    void load();
    
    // Save to persistent storage
    bool save();
    
    // Get current data
    const AppSettings& data() const { return data_; }
    AppSettings& data() { return data_; }
    
    // Update individual fields (with validation)
    bool set_brightness(int value);
    bool set_theme_preset(int preset);
    bool set_custom_color(uint32_t color);
    bool set_mag_min(float mag);
    bool set_time_range(int range);
    bool set_pan_sensitivity(float sensitivity);
    bool set_zoom_limits(int min_zoom, int max_zoom);
    bool set_auto_dim_timeout(int seconds);
    bool set_clock_format(bool use_24h);
    
    // Validation
    bool validate() const;
    
    // Check if modified from saved state
    bool is_modified() const;
    
    // Reset to defaults
    void reset_to_defaults();
    
    // Callbacks for data changes
    using ChangeCallback = std::function<void()>;
    void on_changed(ChangeCallback cb) { change_callback_ = cb; }
    void notify_changed() { if (change_callback_) change_callback_(); }
    
    // Validation helpers (static for testing)
    static bool is_valid_brightness(int value);
    static bool is_valid_zoom_range(int min_zoom, int max_zoom);
    static bool is_valid_magnitude(float mag);
    static bool is_valid_theme_preset(int preset);
    
private:
    AppSettings data_;
    AppSettings saved_data_;
    ChangeCallback change_callback_;
    
    void notify_if_changed();
};

// Global viewmodel instance (for now - can be injected later)
SettingsViewModel* settings_viewmodel_get();

#endif // SETTINGS_VIEWMODEL_H
