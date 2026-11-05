#pragma once
#include <stdint.h>
#include "lvgl.h"
#include "config_constants.h"

// ==========================================
// THEME SYSTEM
// ==========================================
// Centralized color management for consistent UI theming.
// Change the primary accent color and the entire UI adapts.

// Available theme presets
enum ThemePreset {
    THEME_CYAN = 0,
    THEME_ORANGE,
    THEME_GREEN,
    THEME_PINK,
    THEME_YELLOW,
    THEME_PURPLE,
    THEME_BLUE,      // OLED-friendly deep blue
    THEME_RED,       // Vibrant red
    THEME_CUSTOM,    // User-defined custom color
    THEME_COUNT      // Number of themes (including custom)
};

// Number of preset themes (excluding custom, see config_constants.h for value)

// Theme color structure
struct ThemeColors {
    // Primary accent (selection, active states, main borders, buttons)
    uint32_t primary;           // Main accent color (e.g., 0x00FFFF cyan)
    uint32_t primary_dark;      // Dark tint for backgrounds (e.g., 0x002222)
    uint32_t primary_dim;       // Dimmed version for inactive borders (e.g., 0x006666)

    // Secondary accent (section headers, highlights)
    uint32_t secondary;         // Secondary color (e.g., 0xFF6600 orange)
    uint32_t secondary_dark;    // Dark tint (e.g., 0x1a0a00)

    // Background colors (AMOLED optimized - mostly black)
    uint32_t bg_black;          // True black (0x000000)
    uint32_t bg_dark;           // Near-black (0x0a0a0a)
    uint32_t bg_card;           // Card/container floor (0x0A0A0A)
    uint32_t bg_elevated;       // Slightly elevated surfaces (0x1a1a1a)

    // Text colors
    uint32_t text_primary;      // Primary text - white (0xFFFFFF)
    uint32_t text_secondary;    // Secondary text (0xC4C4C4)
    uint32_t text_tertiary;     // Muted readable floor (0xA0A0A0)
    uint32_t text_disabled;     // Disabled readable floor (0xA0A0A0)

    // Border colors
    uint32_t border_default;    // Default border (0x333333)
    uint32_t border_subtle;     // Shared readable border (0x333333)

    // Semantic colors (fixed across themes)
    uint32_t success;           // Success/positive (0x4ADE80)
    uint32_t warning;           // Warning (0xFACC15)
    uint32_t error;             // Error/critical (0xFF5C5C)
    uint32_t info;              // Info/neutral (0x60A5FA)
};

// ==========================================
// THEME API
// ==========================================

// Initialize theme system (call once at startup)
void theme_init();

// Get current theme colors
const ThemeColors* theme_get();

// Get current theme preset index
ThemePreset theme_get_preset();

// Set theme by preset (deferred NVS save)
void theme_set_preset(ThemePreset preset);

// Save pending theme to NVS (call from main loop when idle)
void theme_save_if_needed();

// Get theme name for display
const char* theme_get_name(ThemePreset preset);

// Get primary color for a specific preset (for theme selector UI)
uint32_t theme_get_primary_color(ThemePreset preset);

// Custom color functions
void theme_set_custom_color(uint32_t color);  // Set custom primary color
uint32_t theme_get_custom_color();             // Get current custom color

// ==========================================
// CONVENIENCE FUNCTIONS (return lv_color_t)
// ==========================================

// Primary colors
lv_color_t theme_primary();
lv_color_t theme_primary_dark();
lv_color_t theme_primary_dim();

// Secondary colors
lv_color_t theme_secondary();
lv_color_t theme_secondary_dark();

// Background colors
lv_color_t theme_bg_black();
lv_color_t theme_bg_dark();
lv_color_t theme_bg_card();
lv_color_t theme_bg_elevated();

// Text colors
lv_color_t theme_text_primary();
lv_color_t theme_text_secondary();
lv_color_t theme_text_tertiary();
lv_color_t theme_text_disabled();

// Border colors
lv_color_t theme_border_default();
lv_color_t theme_border_subtle();

// Semantic colors
lv_color_t theme_success();
lv_color_t theme_warning();
lv_color_t theme_error();
lv_color_t theme_info();
