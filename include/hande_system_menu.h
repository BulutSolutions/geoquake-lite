#pragma once
// The System Menu: hold BOOT for 5 s -> Restart App / Cancel / Factory Reset. The firmware supplies the name,
// accent colour, reset text and a few hooks; it keeps its own settings screens.
//
// Threading: call everything from the app's LVGL thread. Fonts: needs LV_FONT_MONTSERRAT_16, _20 and _24.

#include <stdint.h>

struct HandeSystemMenuConfig {
    const char* app_name;    // under the title, and in "Reset <app_name>?" (keep it short: "GeoQuake")
    uint32_t accent;         // 0xRRGGBB: the hold ring and the two main buttons
    const char* reset_text;  // what Factory Reset erases and what stays, a few short lines ("\n" between them)
    bool (*factory_reset)();  // erase this app's own data; true when done
    void (*wake_display)();   // optional: screen on at full brightness (on the first press and while open)
    bool (*busy)();           // optional: true while actions must wait (an upload, an update)
    bool (*begin_action)();   // optional: take exclusive access before leaving the app; false = busy right now
    void (*end_action)();     // optional: give it back when the action could not go ahead
    void (*on_open)();        // optional: pause the app's own content (the hold ring appears)
    void (*on_close)();       // optional: resume it (the menu closed and the app stays)
};

// Builds the (hidden) menu on lv_layer_top(). Call once, after LVGL and the display are up.
void hande_system_menu_init(const HandeSystemMenuConfig& config);
// Reads BOOT and runs the menu. Call at least every 20 ms from the LVGL thread, before lv_timer_handler().
void hande_system_menu_tick();
// True while the hold ring or the menu is on screen: the app must not act on touches or start new work.
bool hande_system_menu_active();
// For a build whose touch does not reach LVGL as an input device: report each new tap here.
void hande_system_menu_tap(int16_t x, int16_t y);
// Opens the same "Reset <app>?" confirmation directly, for an app's own Settings -> Factory Reset button, so a
// reset always looks and behaves the same wherever it starts. LVGL thread.
void hande_system_menu_open_reset();
// The app's theme changed.
void hande_system_menu_set_accent(uint32_t accent);
