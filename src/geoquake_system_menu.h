#pragma once
// GeoQuake's side of the System Menu (hande_system_menu.h).
// Both functions: LVGL task only, with the UI lock held.

// Builds the menu. Call once, after the display and LVGL are up.
void geoquake_system_menu_init();
// Follows the theme accent and runs the menu. Every LVGL loop pass, before lv_timer_handler().
void geoquake_system_menu_tick();
