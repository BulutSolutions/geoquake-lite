// GeoQuake's hooks for the System Menu (hold BOOT for 5 s).
#include "geoquake_system_menu.h"

#include "factory_reset.h"
#include "hande_system_menu.h"
#include "settings_manager.h"
#include "theme.h"
#include "ui_power.h"
#include "ui_sync.h"
#include "ui_touch.h"
#include "version.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {
uint32_t s_accent = 0;
bool s_night_lifted = false;      // night auto-dim brightness lifted while the menu is open

// Screen on: ends idle sleep (screen off) and resets the idle timers; night auto-dim is
// lifted to the user's brightness while the menu is open and restored on close.
void wake_display() {
    lv_display_trigger_activity(nullptr);
    ui_touch_note_physical_interaction();
    if (ui_auto_dim_is_active() && !s_night_lifted) {
        s_night_lifted = true;
        const int brightness = settings_get()->brightness;
        ui_queue_set_brightness(brightness < 50 ? 50 : brightness);
    }
}

void on_open() {
    // A pan or pinch in progress when BOOT went down ends here; touch_read_cb then routes all
    // touches to LVGL (the menu) because ui_is_panel_open() is true while the menu is active.
    ui_touch_end_gestures();
    ui_hud_drag_end();
}

void on_close() {
    if (s_night_lifted) {
        s_night_lifted = false;
        // Re-apply the night dim (ui_apply_auto_dim only acts on a schedule transition).
        if (ui_auto_dim_is_active()) ui_queue_set_brightness(NIGHT_BRIGHTNESS);
    }
    ui_reset_auto_hide_timer();
}
}  // namespace

void geoquake_system_menu_init() {
    s_accent = theme_get()->primary;
    HandeSystemMenuConfig config{};
    config.app_name = HANDE_APP_NAME;
    config.accent = s_accent;
    config.reset_text =
        "Clears GeoQuake Lite settings, time zone\nand the saved Wi-Fi network.\nMaps stay.";
    config.factory_reset = geoquake_factory_reset;
    config.wake_display = wake_display;
    config.on_open = on_open;
    config.on_close = on_close;
    hande_system_menu_init(config);
}

void geoquake_system_menu_tick() {
    const uint32_t accent = theme_get()->primary;
    if (accent != s_accent) {
        s_accent = accent;
        hande_system_menu_set_accent(accent);
    }
    hande_system_menu_tick();
}
