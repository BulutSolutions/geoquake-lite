#include "theme.h"
#include "amoled_ui_tokens.h"
#include <Preferences.h>

// ==========================================
// THEME DEFINITIONS
// ==========================================

// Helper macro to create dark tint from accent color
// Takes ~10% brightness of the original color
#define DARK_TINT(color) (((color & 0xFF0000) >> 4) | ((color & 0x00FF00) >> 4) | ((color & 0x0000FF) >> 4))

// Theme presets (fixed colors for preset themes)
static const ThemeColors theme_presets[THEME_PRESET_COUNT] = {
    // Default GeoQuake accent
    {
        .primary = APP_ACCENT_HEX,
        .primary_dark = 0x221208,
        .primary_dim = 0x663718,
        .secondary = 0xFF6600,
        .secondary_dark = 0x1a0a00,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
    // THEME_ORANGE
    {
        .primary = 0xFF6600,
        .primary_dark = 0x1a0a00,
        .primary_dim = 0x663300,
        .secondary = 0x00FFFF,
        .secondary_dark = 0x002222,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
    // THEME_GREEN
    {
        .primary = 0x00FF66,
        .primary_dark = 0x001a0a,
        .primary_dim = 0x006633,
        .secondary = 0xFF6600,
        .secondary_dark = 0x1a0a00,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
    // THEME_PINK
    {
        .primary = 0xFF00FF,
        .primary_dark = 0x1a001a,
        .primary_dim = 0x660066,
        .secondary = 0x00FFFF,
        .secondary_dark = 0x002222,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
    // THEME_YELLOW
    {
        .primary = 0xFFDD00,
        .primary_dark = 0x1a1500,
        .primary_dim = 0x665500,
        .secondary = 0x00AAFF,
        .secondary_dark = 0x001122,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
    // THEME_PURPLE
    {
        .primary = 0xAA55FF,
        .primary_dark = 0x110022,
        .primary_dim = 0x552288,
        .secondary = 0xFF6600,
        .secondary_dark = 0x1a0a00,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
    // THEME_BLUE (OLED-friendly deep blue)
    {
        .primary = 0x0088FF,
        .primary_dark = 0x001122,
        .primary_dim = 0x004488,
        .secondary = 0xFF6600,
        .secondary_dark = 0x1a0a00,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
    // THEME_RED
    {
        .primary = 0xFF3344,
        .primary_dark = 0x220011,
        .primary_dim = 0x882233,
        .secondary = 0x00FFFF,
        .secondary_dark = 0x002222,
        .bg_black = AMOLED_BACKGROUND_HEX,
        .bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX,
        .bg_elevated = 0x1a1a1a,
        .text_primary = AMOLED_PRIMARY_TEXT_HEX,
        .text_secondary = AMOLED_SECONDARY_TEXT_HEX,
        .text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX,
        .text_disabled = AMOLED_DISABLED_TEXT_HEX,
        .border_default = AMOLED_BORDER_HEX,
        .border_subtle = AMOLED_BORDER_HEX,
        .success = AMOLED_SUCCESS_HEX,
        .warning = AMOLED_WARNING_HEX,
        .error = AMOLED_ERROR_HEX,
        .info = AMOLED_INFO_HEX,
    },
};

// Theme names for display
static const char* theme_names[THEME_COUNT] = {
    "Classic",
    "Orange",
    "Green",
    "Pink",
    "Yellow",
    "Purple",
    "Blue",
    "Red",
    "Custom"
};

// NVS storage
#define NVS_NAMESPACE "gql_theme"
#define NVS_KEY_PRESET "preset"
#define NVS_KEY_CUSTOM_COLOR "custom"

// Current theme state
static ThemePreset current_preset = THEME_CYAN;
static Preferences theme_prefs;

// Custom theme (dynamically generated from user's color)
static uint32_t custom_primary_color = 0x00AFF0;  // Default cyan, as settings_manager.cpp
static ThemeColors custom_theme;
static bool custom_color_needs_save = false;

// Generate a full theme from just a primary color
static void generate_custom_theme(uint32_t primary) {
    custom_primary_color = primary;

    // Extract RGB components
    uint8_t r = (primary >> 16) & 0xFF;
    uint8_t g = (primary >> 8) & 0xFF;
    uint8_t b = primary & 0xFF;

    // Generate primary variants
    custom_theme.primary = primary;
    custom_theme.primary_dark = ((r / 8) << 16) | ((g / 8) << 8) | (b / 8);  // ~12% brightness
    custom_theme.primary_dim = ((r / 3) << 16) | ((g / 3) << 8) | (b / 3);   // ~33% brightness

    // Use complementary color for secondary (simple inversion)
    custom_theme.secondary = 0xFF6600;  // Keep orange as default secondary
    custom_theme.secondary_dark = 0x1a0a00;

    // Standard backgrounds (AMOLED optimized)
    custom_theme.bg_black = AMOLED_BACKGROUND_HEX;
    custom_theme.bg_dark = AMOLED_SURFACE_CARD_FLOOR_HEX;
    custom_theme.bg_card = AMOLED_SURFACE_CARD_FLOOR_HEX;
    custom_theme.bg_elevated = 0x1a1a1a;

    // Standard text colors
    custom_theme.text_primary = AMOLED_PRIMARY_TEXT_HEX;
    custom_theme.text_secondary = AMOLED_SECONDARY_TEXT_HEX;
    custom_theme.text_tertiary = AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX;
    custom_theme.text_disabled = AMOLED_DISABLED_TEXT_HEX;

    // Standard borders
    custom_theme.border_default = AMOLED_BORDER_HEX;
    custom_theme.border_subtle = AMOLED_BORDER_HEX;

    // Standard semantic colors
    custom_theme.success = AMOLED_SUCCESS_HEX;
    custom_theme.warning = AMOLED_WARNING_HEX;
    custom_theme.error = AMOLED_ERROR_HEX;
    custom_theme.info = AMOLED_INFO_HEX;
}

// ==========================================
// THEME API IMPLEMENTATION
// ==========================================

void theme_init() {
    theme_prefs.begin(NVS_NAMESPACE, true);  // Read-only first
    current_preset = (ThemePreset)theme_prefs.getInt(NVS_KEY_PRESET, THEME_CYAN);
    custom_primary_color = theme_prefs.getUInt(NVS_KEY_CUSTOM_COLOR, 0x00AFF0);  // Default cyan, as settings_manager.cpp
    theme_prefs.end();

    // Validate preset
    if (current_preset >= THEME_COUNT) {
        current_preset = THEME_CYAN;
    }

    // Initialize custom theme with saved color
    generate_custom_theme(custom_primary_color);

    Serial.printf("[THEME] Initialized with preset: %s\n", theme_names[current_preset]);
    if (current_preset == THEME_CUSTOM) {
        Serial.printf("[THEME] Custom color: 0x%06X\n", custom_primary_color);
    }
}

const ThemeColors* theme_get() {
    if (current_preset == THEME_CUSTOM) {
        return &custom_theme;
    }
    return &theme_presets[current_preset];
}

ThemePreset theme_get_preset() {
    return current_preset;
}

// Flag to defer NVS save (avoid blocking UI)
static bool theme_needs_save = false;
static ThemePreset theme_pending_preset = THEME_CYAN;

void theme_set_preset(ThemePreset preset) {
    if (preset >= THEME_COUNT) return;
    if (preset == current_preset) return;  // No change needed

    current_preset = preset;

    // Defer NVS save to avoid blocking UI
    theme_needs_save = true;
    theme_pending_preset = preset;
}

// Call this periodically (e.g., from main loop) to save pending theme
void theme_save_if_needed() {
    if (!theme_needs_save && !custom_color_needs_save) return;

    theme_prefs.begin(NVS_NAMESPACE, false);

    if (theme_needs_save) {
        theme_needs_save = false;
        theme_prefs.putInt(NVS_KEY_PRESET, (int)theme_pending_preset);
        Serial.printf("[THEME] Saved preset to NVS: %s\n", theme_names[theme_pending_preset]);
    }

    if (custom_color_needs_save) {
        custom_color_needs_save = false;
        theme_prefs.putUInt(NVS_KEY_CUSTOM_COLOR, custom_primary_color);
        Serial.printf("[THEME] Saved custom color to NVS: 0x%06X\n", custom_primary_color);
    }

    theme_prefs.end();
}

const char* theme_get_name(ThemePreset preset) {
    if (preset >= THEME_COUNT) return "Unknown";
    return theme_names[preset];
}

uint32_t theme_get_primary_color(ThemePreset preset) {
    if (preset >= THEME_COUNT) return 0x00FFFF;
    if (preset == THEME_CUSTOM) return custom_primary_color;
    return theme_presets[preset].primary;
}

void theme_set_custom_color(uint32_t color) {
    generate_custom_theme(color);
    custom_color_needs_save = true;

    // If custom theme is active, the UI will update on next refresh
    Serial.printf("[THEME] Custom color set to: 0x%06X\n", color);
}

uint32_t theme_get_custom_color() {
    return custom_primary_color;
}

// ==========================================
// CONVENIENCE FUNCTIONS
// ==========================================

// Primary colors
lv_color_t theme_primary() {
    return lv_color_hex(theme_get()->primary);
}

lv_color_t theme_primary_dark() {
    return lv_color_hex(theme_get()->primary_dark);
}

lv_color_t theme_primary_dim() {
    return lv_color_hex(theme_get()->primary_dim);
}

// Secondary colors
lv_color_t theme_secondary() {
    return lv_color_hex(theme_get()->secondary);
}

lv_color_t theme_secondary_dark() {
    return lv_color_hex(theme_get()->secondary_dark);
}

// Background colors
lv_color_t theme_bg_black() {
    return lv_color_hex(theme_get()->bg_black);
}

lv_color_t theme_bg_dark() {
    return lv_color_hex(theme_get()->bg_dark);
}

lv_color_t theme_bg_card() {
    return lv_color_hex(theme_get()->bg_card);
}

lv_color_t theme_bg_elevated() {
    return lv_color_hex(theme_get()->bg_elevated);
}

// Text colors
lv_color_t theme_text_primary() {
    return lv_color_hex(theme_get()->text_primary);
}

lv_color_t theme_text_secondary() {
    return lv_color_hex(theme_get()->text_secondary);
}

lv_color_t theme_text_tertiary() {
    return lv_color_hex(theme_get()->text_tertiary);
}

lv_color_t theme_text_disabled() {
    return lv_color_hex(theme_get()->text_disabled);
}

// Border colors
lv_color_t theme_border_default() {
    return lv_color_hex(theme_get()->border_default);
}

lv_color_t theme_border_subtle() {
    return lv_color_hex(theme_get()->border_subtle);
}

// Semantic colors
lv_color_t theme_success() {
    return lv_color_hex(theme_get()->success);
}

lv_color_t theme_warning() {
    return lv_color_hex(theme_get()->warning);
}

lv_color_t theme_error() {
    return lv_color_hex(theme_get()->error);
}

lv_color_t theme_info() {
    return lv_color_hex(theme_get()->info);
}
