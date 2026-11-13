/**
 * GeoQuake's settings rows on the Settings screen (hande_settings.cpp).
 *
 * This file only describes rows (static tables) and connects them to the storage GeoQuake already had:
 * - AppSettings via settings_get() + ui_settings_save_pending (deferred NVS save, 500 ms, namespace "gql_cfg")
 * - night mode via ui_auto_dim_*() (namespace "gql_auto_dim", its own deferred save in ui_power.cpp)
 * - brightness via ui_queue_set_brightness() (live preview, thread-safe queue)
 * - theme colour via settings_save_to_nvs() + settings_apply_theme() on release (as before)
 * Same keys, same ranges, same option values as the old ui_settings_{display,earthquake,sound,advanced,appearance}
 * pages. Hub tiles: Earthquakes, Sound, Theme, then Display / Clock & HUD (rows from here), Wi-Fi and About
 * (version, Factory reset).
 */

#include "ui_settings.h"
#include "hande_settings.h"
#include "ui_display.h"
#include "ui_power.h"
#include "ui_state.h"
#include "ui_sync.h"
#include "theme.h"
#include "config_constants.h"
#include "settings_manager.h"
#include "audio_manager.h"
#include "wifi_manager.h"
#include "hande_time.h"
#include "hande_tz_db.h"
#include "version.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>

bool ui_settings_save_pending = false;

namespace {

AppSettings* app() { return settings_get(); }
void mark_dirty() { ui_settings_save_pending = true; }

// ---- option tables (values identical to the old dropdowns) ------------------------------------------------

const int kTimeoutValues[] = {0, 30, 120};  // Screen timeout / Auto-hide: Off, 30 s, 2 min
const char* const kTimeoutLabels[] = {"Off", "30 s", "2 min"};
const int kWifiOffValues[] = {0, 10, 30, 60};  // Wi-Fi auto-off: Always on, 10 s, 30 s, 60 s
const char* const kWifiOffLabels[] = {"Never", "10 s", "30 s", "60 s"};
const int kSyncValues[] = {5, 10, 15, 30};  // Sync interval (minutes)
const char* const kSyncLabels[] = {"5 min", "10 min", "15 min", "30 min"};
const char* const kEffectNames[] = {
    "Beep",      "Double Beep", "Chime",      "Alert",      "Triple Beep", "Quad Pulse",    "Descending Chime",
    "Warble",    "Two-Tone",    "Long Alert", "Soft Ping",  "Deep Rumble", "Siren",         "Triplet Chime",
    "Urgent Buzz", "Fade Sweep", "Bell",      "Two-Step Rise", "Whistle"};
constexpr int32_t kEffectCount = sizeof(kEffectNames) / sizeof(kEffectNames[0]);

// Old dropdown index mapping (values in between snap the same way they did).
int32_t timeout_index(int seconds) { return seconds <= 0 ? 0 : (seconds <= 30 ? 1 : 2); }
int32_t wifi_off_index(int seconds) {
    if (seconds <= 0) return 0;
    if (seconds <= 10) return 1;
    return seconds <= 30 ? 2 : 3;
}
int32_t sync_index(int minutes) {
    if (minutes <= 5) return 0;
    if (minutes <= 10) return 1;
    return minutes <= 15 ? 2 : 3;
}
int32_t clamp_index(int32_t i, int32_t count) { return i < 0 ? 0 : (i >= count ? count - 1 : i); }

// ---- DISPLAY ------------------------------------------------------------------------------------------------

int32_t get_brightness() { return app() ? app()->brightness : 200; }
void set_brightness(int32_t v) {
    ui_queue_set_brightness(v);  // live preview via the thread-safe queue
    if (app()) {
        app()->brightness = v;
        mark_dirty();
    }
}
const char* brightness_text(int32_t v, char* buf, size_t len) {
    snprintf(buf, len, "%ld%%", static_cast<long>((v * 100 + 127) / 255));
    return buf;
}

bool get_night() { return ui_auto_dim_is_enabled(); }
void set_night(bool on) {
    ui_auto_dim_set_enabled(on);
    Serial.printf("[SETTINGS] Night mode: %s\n", on ? "ON" : "OFF");
}
int32_t get_night_start() { return ui_auto_dim_get_start_hour(); }
void set_night_start(int32_t h) { ui_auto_dim_set_schedule(h, ui_auto_dim_get_end_hour()); }
int32_t get_night_end() { return ui_auto_dim_get_end_hour(); }
void set_night_end(int32_t h) { ui_auto_dim_set_schedule(ui_auto_dim_get_start_hour(), h); }
const char* hour_text(int32_t h, char* buf, size_t len) {
    snprintf(buf, len, "%02ld:00", static_cast<long>(h));
    return buf;
}

int32_t get_screen_timeout() { return timeout_index(app() ? app()->auto_dim_timeout : 30); }
void set_screen_timeout(int32_t i) {
    if (!app()) return;
    app()->auto_dim_timeout = kTimeoutValues[clamp_index(i, 3)];
    mark_dirty();
    Serial.printf("[SETTINGS] Screen timeout: %d seconds\n", app()->auto_dim_timeout);
}

// ---- CLOCK & HUD --------------------------------------------------------------------------------------------

bool get_24h() { return app() && app()->clock_24h; }
void set_24h(bool on) {
    if (app()) {
        app()->clock_24h = on;
        mark_dirty();
    }
    ui_update_clock();
}
bool get_show_date() { return app() && app()->show_date; }
void set_show_date(bool on) {
    if (app()) {
        app()->show_date = on;
        mark_dirty();
    }
    ui_update_clock();
}
bool get_show_battery() { return app() && app()->show_battery; }
void set_show_battery(bool on) {
    if (app()) {
        app()->show_battery = on;
        mark_dirty();
    }
    if (ui_BatteryContainer) {
        if (on) lv_obj_remove_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_BatteryContainer);
    }
}
int32_t get_auto_hide() { return timeout_index(app() ? app()->auto_hide_timeout : 30); }
void set_auto_hide(int32_t i) {
    if (!app()) return;
    app()->auto_hide_timeout = kTimeoutValues[clamp_index(i, 3)];
    mark_dirty();
}
bool get_time_visible() { return app() && app()->time_always_visible; }
void set_time_visible(bool on) {
    if (app()) {
        app()->time_always_visible = on;
        mark_dirty();
    }
}

// ---- TIME ZONE (sub-page): a region roller and a city roller, applied through hande_time -----------------------
// The zone list is hande_tz_db (595 IANA names, sorted); the split is hande_tz_names::split_zone (host-tested).
// Only the regions the picker offers are shown (anchors like "US/Eastern" or "Etc/UTC" are left out), plus "UTC"
// as the last entry, which is the zone itself.

const char* const kTzRegions[] = {"Africa", "America", "Asia", "Atlantic",
                                  "Australia", "Europe", "Indian", "Pacific", "UTC"};
constexpr int32_t kTzRegionCount = sizeof(kTzRegions) / sizeof(kTzRegions[0]);
constexpr int32_t kTzUtcIndex = kTzRegionCount - 1;
constexpr int32_t kTzRowHeight = 56;  // a roller row is a touch target on its own (>= 56 px)

lv_obj_t* s_tz_region_roller = nullptr;
lv_obj_t* s_tz_city_roller = nullptr;
int32_t s_tz_region = kTzUtcIndex;
char s_tz_options[2048];  // city options of the selected region (the largest, America, needs ~1.8 KB)
char s_tz_zone[48] = "UTC";

const char* tz_text(char* buf, size_t len) { return hande_time_zone_name(buf, len); }

// The nth zone of a region, in table order (the table is sorted, so the cities read alphabetically).
bool zone_at(int32_t region_index, int32_t city_index, char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = '\0';
    if (region_index < 0 || region_index >= kTzRegionCount || city_index < 0) return false;
    if (region_index == kTzUtcIndex) {
        if (city_index != 0) return false;
        snprintf(out, len, "UTC");
        return true;
    }
    const char* wanted = kTzRegions[region_index];
    char zone_region[24];
    char zone_city[48];
    int32_t seen = 0;
    for (uint32_t i = 0; i < hande_tz_db::kZoneCount; ++i) {
        if (!hande_tz_names::split_zone(hande_tz_db::kZones[i].name, zone_region, sizeof(zone_region), zone_city,
                                        sizeof(zone_city))) {
            continue;
        }
        if (strcmp(zone_region, wanted) != 0) continue;
        if (seen++ != city_index) continue;
        snprintf(out, len, "%s", hande_tz_db::kZones[i].name);
        return true;
    }
    return false;
}

// City roller options ("New York\nChicago\n...") of a region: the part after the first '/', '_' shown as a space.
void build_city_options(int32_t region_index) {
    s_tz_options[0] = '\0';
    if (region_index == kTzUtcIndex) {
        snprintf(s_tz_options, sizeof(s_tz_options), "UTC");
        return;
    }
    const char* wanted = kTzRegions[region_index];
    char zone_region[24];
    char zone_city[48];
    size_t used = 0;
    for (uint32_t i = 0; i < hande_tz_db::kZoneCount; ++i) {
        if (!hande_tz_names::split_zone(hande_tz_db::kZones[i].name, zone_region, sizeof(zone_region), zone_city,
                                        sizeof(zone_city))) {
            continue;
        }
        if (strcmp(zone_region, wanted) != 0) continue;
        const size_t n = strlen(zone_city);
        if (used + n + 2 > sizeof(s_tz_options)) break;  // cannot happen with this table; never overflow
        memcpy(s_tz_options + used, zone_city, n);
        used += n;
        s_tz_options[used++] = '\n';
        s_tz_options[used] = '\0';
    }
    if (used > 0) s_tz_options[used - 1] = '\0';  // the roller wants no trailing newline
    if (used == 0) snprintf(s_tz_options, sizeof(s_tz_options), "UTC");
}

void tz_remember_selection() {
    const int32_t city = s_tz_city_roller ? static_cast<int32_t>(lv_roller_get_selected(s_tz_city_roller)) : 0;
    if (!zone_at(s_tz_region, city, s_tz_zone, sizeof(s_tz_zone))) snprintf(s_tz_zone, sizeof(s_tz_zone), "UTC");
}

void tz_region_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    const int32_t region = static_cast<int32_t>(lv_roller_get_selected(lv_event_get_target_obj(e)));
    if (region == s_tz_region) return;
    s_tz_region = region;
    build_city_options(s_tz_region);
    if (s_tz_city_roller) {
        lv_roller_set_options(s_tz_city_roller, s_tz_options, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(s_tz_city_roller, 0, LV_ANIM_OFF);
    }
    tz_remember_selection();
}

void tz_city_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    tz_remember_selection();
}

void tz_set_event(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const bool applied = hande_time_set_zone(s_tz_zone);
    Serial.printf("[SETTINGS] Time zone: %s: %s\n", s_tz_zone, applied ? "applied" : "unknown zone");
    hande_settings_refresh();  // the "Time zone" row shows the new name
}

void tz_card_deleted(lv_event_t*) {
    s_tz_region_roller = nullptr;
    s_tz_city_roller = nullptr;
}

lv_obj_t* make_roller(lv_obj_t* parent, const char* options) {
    lv_obj_t* roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_bg_color(roller, lv_color_black(), LV_PART_MAIN);  // AMOLED black like the rest of Settings
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(roller, lv_color_hex(0x242A38), LV_PART_MAIN);
    lv_obj_set_style_radius(roller, 10, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, lv_color_hex(0x9DA7B5), LV_PART_MAIN);
    // LVGL spaces roller rows by font line height + text line space (not by padding): the row is exactly 56 px.
    lv_obj_set_style_text_line_space(roller, kTzRowHeight - lv_font_get_line_height(&lv_font_montserrat_20),
                                     LV_PART_MAIN);
    lv_obj_set_style_bg_color(roller, lv_color_hex(0x141720), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(0xF2F5F8), LV_PART_SELECTED);
    return roller;
}

void build_tz_card(lv_obj_t* box) {
    lv_obj_add_event_cb(box, tz_card_deleted, LV_EVENT_DELETE, nullptr);

    // Region and city rollers side by side, three 56 px rows each; "Set" applies the pair below them.
    lv_obj_t* row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), 3 * kTzRowHeight);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char region_options[96];
    size_t used = 0;
    for (int32_t i = 0; i < kTzRegionCount; ++i) {
        const size_t n = strlen(kTzRegions[i]);
        if (used + n + 2 > sizeof(region_options)) break;
        memcpy(region_options + used, kTzRegions[i], n);
        used += n;
        region_options[used++] = '\n';
        region_options[used] = '\0';
    }
    if (used > 0) region_options[used - 1] = '\0';

    s_tz_region_roller = make_roller(row, region_options);
    lv_obj_set_size(s_tz_region_roller, LV_PCT(44), LV_PCT(100));
    lv_obj_add_event_cb(s_tz_region_roller, tz_region_event, LV_EVENT_VALUE_CHANGED, nullptr);

    s_tz_city_roller = make_roller(row, "UTC");
    lv_obj_set_size(s_tz_city_roller, LV_PCT(52), LV_PCT(100));
    lv_obj_add_event_cb(s_tz_city_roller, tz_city_event, LV_EVENT_VALUE_CHANGED, nullptr);

    // Start on the stored zone; a zone the picker does not offer (an anchor like "Etc/UTC") starts on "UTC".
    char stored[48];
    hande_time_zone_name(stored, sizeof(stored));
    char stored_region[24] = {};
    char stored_city[48] = {};
    s_tz_region = kTzUtcIndex;
    if (hande_tz_names::split_zone(stored, stored_region, sizeof(stored_region), stored_city, sizeof(stored_city)) &&
        stored_region[0]) {
        for (int32_t i = 0; i < kTzUtcIndex; ++i) {
            if (strcmp(kTzRegions[i], stored_region) == 0) {
                s_tz_region = i;
                break;
            }
        }
    }
    build_city_options(s_tz_region);
    lv_roller_set_options(s_tz_city_roller, s_tz_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_tz_region_roller, s_tz_region, LV_ANIM_OFF);

    int32_t city_index = 0;
    for (int32_t i = 0;; ++i) {
        char name[48];
        if (!zone_at(s_tz_region, i, name, sizeof(name))) break;
        if (strcmp(name, stored) == 0) {
            city_index = i;
            break;
        }
    }
    lv_roller_set_selected(s_tz_city_roller, city_index, LV_ANIM_OFF);
    tz_remember_selection();

    lv_obj_t* set_button = lv_button_create(box);
    lv_obj_set_size(set_button, LV_PCT(100), kTzRowHeight);
    lv_obj_set_style_bg_color(set_button, theme_primary(), 0);
    lv_obj_set_style_bg_opa(set_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(set_button, 0, 0);
    lv_obj_set_style_shadow_width(set_button, 0, 0);
    lv_obj_set_style_radius(set_button, 14, 0);
    lv_obj_set_style_bg_color(set_button, lv_color_hex(0x141720), LV_STATE_PRESSED);
    lv_obj_add_event_cb(set_button, tz_set_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* set_label = lv_label_create(set_button);
    lv_label_set_text(set_label, "Set");
    lv_obj_set_style_text_font(set_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(set_label, lv_color_black(), 0);
    lv_obj_center(set_label);
}

// ---- EARTHQUAKES ----------------------------------------------------------------------------------------------

// Minimum magnitude in tenths (0.0 .. 8.0 by 0.1, the old slider's range and precision).
int32_t get_mag() { return app() ? static_cast<int32_t>(lroundf(app()->mag_min * 10.0f)) : 10; }
void set_mag(int32_t tenths) {
    if (app()) {
        app()->mag_min = tenths / 10.0f;
        mark_dirty();
    }
}
const char* mag_text(int32_t tenths, char* buf, size_t len) {
    snprintf(buf, len, "M %ld.%ld", static_cast<long>(tenths / 10), static_cast<long>(tenths % 10));
    return buf;
}
int32_t get_sync() { return sync_index(app() ? app()->sync_interval_minutes : 10); }
void set_sync(int32_t i) {
    if (!app()) return;
    app()->sync_interval_minutes = kSyncValues[clamp_index(i, 4)];
    mark_dirty();
}
int32_t get_wifi_off() { return wifi_off_index(app() ? app()->wifi_auto_off_seconds : 30); }
void set_wifi_off(int32_t i) {
    if (!app()) return;
    app()->wifi_auto_off_seconds = kWifiOffValues[clamp_index(i, 4)];
    mark_dirty();
}

// ---- SOUND (sub-page) -----------------------------------------------------------------------------------------

int32_t get_effect() { return app() ? clamp_index(app()->sound_effect_id, kEffectCount) : 0; }
void set_effect(int32_t id) {
    if (app()) {
        app()->sound_effect_id = id;
        mark_dirty();
    }
    audio_play_test_tone();  // the old dropdown previewed the new effect too
}
const char* effect_text(int32_t id, char* buf, size_t len) {
    snprintf(buf, len, "%s", kEffectNames[clamp_index(id, kEffectCount)]);
    return buf;
}
int32_t get_volume() { return app() ? app()->sound_volume : 70; }
void set_volume(int32_t v) {
    if (app()) {
        app()->sound_volume = v;
        mark_dirty();
    }
}
const char* percent_text(int32_t v, char* buf, size_t len) {
    snprintf(buf, len, "%ld%%", static_cast<long>(v));
    return buf;
}
bool get_silent() { return app() && app()->sound_muted; }
void set_silent(bool on) {
    if (app()) {
        app()->sound_muted = on;
        mark_dirty();
    }
}
void test_sound() {
    const bool started = audio_play_test_tone();
    Serial.printf("[SETTINGS] Test Sound result: %s\n", started ? "started" : "failed");
}
const char* sound_link_text(char* buf, size_t len) {
    if (get_silent()) return "Silent";
    return effect_text(get_effect(), buf, len);
}

// ---- APPEARANCE (sub-page): theme colour, hue slider ----------------------------------------------------------
// A custom row: the shared slider only reports VALUE_CHANGED, but the theme is applied once on release (repainting
// the whole UI on every drag tick is too heavy). Events: VALUE_CHANGED, RELEASED, PRESS_LOST only
// (never LV_EVENT_PRESSING on a slider).

lv_obj_t* s_swatch = nullptr;
lv_obj_t* s_hex = nullptr;

uint32_t to_rgb888(lv_color_t c) {
    const lv_color32_t c32 = lv_color_to_32(c, 0xFF);
    return (static_cast<uint32_t>(c32.red) << 16) | (static_cast<uint32_t>(c32.green) << 8) | c32.blue;
}

int32_t rgb_to_hue(uint32_t rgb) {
    const float r = ((rgb >> 16) & 0xFF) / 255.0f;
    const float g = ((rgb >> 8) & 0xFF) / 255.0f;
    const float b = (rgb & 0xFF) / 255.0f;
    const float max_c = fmaxf(r, fmaxf(g, b));
    const float delta = max_c - fminf(r, fminf(g, b));
    if (delta < 0.001f) return 0;
    float hue;
    if (max_c == r) hue = 60.0f * fmodf((g - b) / delta, 6.0f);
    else if (max_c == g) hue = 60.0f * ((b - r) / delta + 2.0f);
    else hue = 60.0f * ((r - g) / delta + 4.0f);
    if (hue < 0.0f) hue += 360.0f;
    return static_cast<int32_t>(hue + 0.5f);
}

void show_preview(uint32_t rgb) {
    if (s_swatch) lv_obj_set_style_bg_color(s_swatch, lv_color_hex(rgb), 0);
    if (s_hex) {
        lv_label_set_text_fmt(s_hex, "#%06lX", static_cast<unsigned long>(rgb & 0xFFFFFF));
        lv_obj_set_style_text_color(s_hex, lv_color_hex(rgb), 0);
    }
}

void commit_custom_color(uint32_t rgb) {
    AppSettings* s = app();
    if (!s) return;
    rgb &= 0xFFFFFF;
    if (s->theme_preset < 0 && s->custom_color == rgb) return;
    Serial.printf("[SETTINGS] Custom theme color changed to 0x%06X\n", static_cast<unsigned int>(rgb));
    s->theme_preset = -1;
    s->custom_color = rgb;
    settings_save_to_nvs();
    settings_apply_theme();  // also passes the new accent to the shared screen
}

int32_t s_hue_at_press = -1;

void hue_event(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target_obj(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_hue_at_press = lv_slider_get_value(slider);
        return;
    }
    const lv_color_t c = lv_color_hsv_to_rgb(static_cast<uint16_t>(lv_slider_get_value(slider)), 100, 100);
    const uint32_t rgb = to_rgb888(c);
    show_preview(rgb);
    lv_obj_set_style_bg_color(slider, c, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, c, LV_PART_KNOB);
    const lv_event_code_t code = lv_event_get_code(e);
    // Commit only a hue the user picked: a swipe over the slider (value put back) must not turn a preset theme custom.
    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && lv_slider_get_value(slider) != s_hue_at_press) {
        commit_custom_color(rgb);
    }
}

void colour_row_deleted(lv_event_t*) {
    s_swatch = nullptr;
    s_hex = nullptr;
}

void build_colour_row(lv_obj_t* box) {
    // Preview the colour in use: the preset primary while a preset is active, else the custom colour.
    const AppSettings* s = app();
    const bool preset = s && s->theme_preset >= 0 && s->theme_preset < THEME_PRESET_COUNT;
    const uint32_t rgb = (preset ? theme_get_primary_color(static_cast<ThemePreset>(s->theme_preset))
                                 : (s ? s->custom_color : theme_get_custom_color())) & 0xFFFFFF;
    lv_obj_add_event_cb(box, colour_row_deleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t* row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);

    s_swatch = lv_obj_create(row);
    lv_obj_remove_style_all(s_swatch);
    lv_obj_remove_flag(s_swatch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_swatch, 44, 44);
    lv_obj_set_style_radius(s_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_swatch, 2, 0);
    lv_obj_set_style_border_color(s_swatch, lv_color_hex(0xD0D7DE), 0);

    s_hex = lv_label_create(row);
    lv_obj_set_style_text_font(s_hex, &lv_font_montserrat_20, 0);
    show_preview(rgb);

    // Show the whole hue spectrum on the track. The strip and the slider sit exactly on top of each other, centred
    // in a 56 px tall container (a child's click area cannot leave its parent: this is the slider's finger band);
    // the slider is created last so the knob draws over the strip.
    lv_obj_t* track = lv_obj_create(box);
    lv_obj_remove_style_all(track);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(track, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // the knob sticks out past the ends
    lv_obj_set_size(track, LV_PCT(100), 56);

    lv_obj_t* strip = lv_obj_create(track);
    lv_obj_remove_style_all(strip);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(strip, LV_PCT(100), 16);
    lv_obj_align(strip, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_radius(strip, 8, 0);
    lv_obj_set_style_clip_corner(strip, true, 0);
    for (int i = 0; i < 6; ++i) {
        lv_obj_t* seg = lv_obj_create(strip);
        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(seg, LV_PCT(100), LV_PCT(100));  // six equal LV_PCT slices, no gaps
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_bg_color(seg, lv_color_hsv_to_rgb(i * 60, 100, 100), 0);
        lv_obj_set_style_bg_grad_color(seg, lv_color_hsv_to_rgb((i + 1) * 60, 100, 100), 0);
        lv_obj_set_style_bg_grad_dir(seg, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }

    lv_obj_t* slider = lv_slider_create(track);
    lv_obj_set_size(slider, LV_PCT(100), 16);
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 0, 0);
    lv_slider_set_range(slider, 0, 360);
    lv_slider_set_value(slider, rgb_to_hue(rgb), LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);       // let the spectrum strip show through
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(rgb), LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(0xF2F5F8), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 20);  // 16 + 2 x 20 = the 56 px track
    hande_settings_slider_guard(slider);  // shared touch rules: drags stay in it, a vertical swipe never sets it
    lv_obj_add_event_cb(slider, hue_event, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(slider, hue_event, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, hue_event, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, hue_event, LV_EVENT_PRESS_LOST, nullptr);
}

const char* theme_link_text(char* buf, size_t len) {
    const int preset = app() ? app()->theme_preset : 0;
    if (preset >= 0 && preset < THEME_PRESET_COUNT) {
        snprintf(buf, len, "%s", theme_get_name(static_cast<ThemePreset>(preset)));
    } else {
        snprintf(buf, len, "#%06lX", static_cast<unsigned long>(theme_get_custom_color() & 0xFFFFFF));
    }
    return buf;
}

// ---- DEVICE ---------------------------------------------------------------------------------------------------

const char* mac_text(char* buf, size_t len) {
    snprintf(buf, len, "%s", wifi_get_mac_address().c_str());
    return buf;
}

// ---- summaries (second line of each hub tile; ASCII only: the built-in fonts have no other glyphs) ------------

const char* quake_summary(char* buf, size_t len) {
    char mag[12];
    snprintf(buf, len, "%s+, %s", mag_text(get_mag(), mag, sizeof(mag)), kSyncLabels[get_sync()]);
    return buf;
}
const char* display_summary(char* buf, size_t len) {
    char level[8];
    brightness_text(get_brightness(), level, sizeof(level));
    if (get_night()) {
        snprintf(buf, len, "%s, night %02ld-%02ld", level, static_cast<long>(get_night_start()),
                 static_cast<long>(get_night_end()));
    } else {
        snprintf(buf, len, "%s, night off", level);
    }
    return buf;
}
const char* clock_summary(char* buf, size_t len) {
    snprintf(buf, len, "%s%s", get_24h() ? "24 h" : "12 h", get_show_date() ? ", date" : "");
    return buf;
}

// ---- tables ---------------------------------------------------------------------------------------------------

const HandeSettingsRow kQuakeRows[] = {
    hande_row_slider("Minimum magnitude", 0, 80, get_mag, set_mag, mag_text),
};
const HandeSettingsRow kSyncRows[] = {
    hande_row_choice("Check every", kSyncLabels, 4, get_sync, set_sync),
};
const HandeSettingsSection kQuakeSections[] = {
    {nullptr, kQuakeRows, sizeof(kQuakeRows) / sizeof(kQuakeRows[0])},
    {"UPDATES", kSyncRows, sizeof(kSyncRows) / sizeof(kSyncRows[0])},
};
const HandeSettingsPage kQuakePage = {"Earthquakes", kQuakeSections, 2};

const HandeSettingsRow kSoundRows[] = {
    // Each step plays a preview of the effect: one step per tap, no burst while held.
    hande_row_tap_only(hande_row_stepper("Effect", 0, kEffectCount - 1, 1, get_effect, set_effect, effect_text)),
    hande_row_slider("Volume", 0, 100, get_volume, set_volume, percent_text),
    hande_row_toggle("Silent mode", get_silent, set_silent),
    hande_row_action("Test sound", test_sound),
};
const HandeSettingsSection kSoundSections[] = {
    {nullptr, kSoundRows, sizeof(kSoundRows) / sizeof(kSoundRows[0])},
};
const HandeSettingsPage kSoundPage = {"Sound", kSoundSections, 1};

const HandeSettingsRow kThemeRows[] = {
    hande_row_custom("Theme colour", build_colour_row),
};
const HandeSettingsSection kThemeSections[] = {
    {nullptr, kThemeRows, sizeof(kThemeRows) / sizeof(kThemeRows[0])},
};
const HandeSettingsPage kThemePage = {"Theme", kThemeSections, 1};

const HandeSettingsTile kTiles[] = {
    {LV_SYMBOL_BELL, "Earthquakes", &kQuakePage, quake_summary},
    {LV_SYMBOL_VOLUME_MAX, "Sound", &kSoundPage, sound_link_text},
    {LV_SYMBOL_TINT, "Theme", &kThemePage, theme_link_text},
};

const HandeSettingsRow kBrightnessRows[] = {
    hande_row_slider("Brightness", 10, 255, get_brightness, set_brightness, brightness_text),  // min 10: never black
};
const HandeSettingsRow kNightRows[] = {
    hande_row_toggle("Dim at night", get_night, set_night),
    // Hours go round: + at 23:00 gives 00:00, - at 00:00 gives 23:00.
    hande_row_when(hande_row_stepper("Starts", 0, 23, 1, get_night_start, set_night_start, hour_text, true),
                   get_night),
    hande_row_when(hande_row_stepper("Ends", 0, 23, 1, get_night_end, set_night_end, hour_text, true), get_night),
};
const HandeSettingsRow kSleepRows[] = {
    hande_row_choice("Screen off after", kTimeoutLabels, 3, get_screen_timeout, set_screen_timeout),
};
const HandeSettingsSection kDisplaySections[] = {
    {nullptr, kBrightnessRows, sizeof(kBrightnessRows) / sizeof(kBrightnessRows[0])},
    {HANDE_SETTINGS_NIGHT, kNightRows, sizeof(kNightRows) / sizeof(kNightRows[0])},
    {HANDE_SETTINGS_SLEEP, kSleepRows, sizeof(kSleepRows) / sizeof(kSleepRows[0])},
};
const HandeSettingsPage kDisplayPage = {"Display", kDisplaySections, 3};

// "Set automatically": the zone comes from the internet (hande_time's lookup). Off: the pickers appear and the
// zone in use stays until another one is set.
bool get_tz_auto() { return hande_time_zone_automatic(); }
void set_tz_auto(bool on) {
    hande_time_set_zone_automatic(on);
    Serial.printf("[SETTINGS] Time zone: set automatically %s\n", on ? "on" : "off");
    if (on) ui_queue_manual_refresh_kept();  // go online now: the lookup follows the earthquake download
}
bool tz_manual() { return !hande_time_zone_automatic(); }

const HandeSettingsRow kTzRows[] = {
    hande_row_toggle("Set automatically", get_tz_auto, set_tz_auto, "From your internet connection"),
    hande_row_when(hande_row_info("Zone", tz_text), get_tz_auto),
    hande_row_when(hande_row_custom("Time zone", build_tz_card), tz_manual),
};
const HandeSettingsSection kTzSections[] = {
    {nullptr, kTzRows, sizeof(kTzRows) / sizeof(kTzRows[0])},
};
const HandeSettingsPage kTzPage = {"Time zone", kTzSections, 1};

const HandeSettingsRow kClockRows[] = {
    hande_row_toggle("24-hour clock", get_24h, set_24h),
    hande_row_toggle("Show date", get_show_date, set_show_date),
    hande_row_link("Time zone", &kTzPage, tz_text),
};
const HandeSettingsRow kHudRows[] = {
    hande_row_toggle("Show battery", get_show_battery, set_show_battery),
    hande_row_choice("Auto-hide HUD", kTimeoutLabels, 3, get_auto_hide, set_auto_hide),
    hande_row_toggle("Keep time visible", get_time_visible, set_time_visible, "When the HUD hides"),
};
const HandeSettingsSection kClockSections[] = {
    {HANDE_SETTINGS_CLOCK, kClockRows, sizeof(kClockRows) / sizeof(kClockRows[0])},
    {HANDE_SETTINGS_HUD, kHudRows, sizeof(kHudRows) / sizeof(kHudRows[0])},
};
const HandeSettingsPage kClockPage = {"Clock & HUD", kClockSections, 2};

// ---- Wi-Fi (the shared page's own section) --------------------------------------------------------------------

// Forget Wi-Fi erases the driver's station config (NVS) and stops the radio: that must not run on the LVGL thread,
// so the button queues it to a small background task and the row under it shows the result when it is done.
enum class ForgetState : uint8_t { kIdle = 0, kPending = 1, kDone = 2, kFailed = 3 };

volatile ForgetState s_forget_state = ForgetState::kIdle;
volatile bool s_forget_requested = false;
TaskHandle_t s_forget_task = nullptr;

void forget_finished(void*) { hande_settings_refresh(); }  // LVGL thread: re-read the status row

void forget_task(void*) {
    while (true) {
        if (s_forget_requested) {
            s_forget_requested = false;
            const bool forgotten = wifi_forget_network();
            s_forget_state = forgotten ? ForgetState::kDone : ForgetState::kFailed;
            Serial.printf("[SETTINGS] Forget Wi-Fi: %s\n", forgotten ? "forgotten" : "not cleared");
            // Hand the refresh to the LVGL task: take the app's UI lock around the schedule call, as every other
            // background task does, and release it at once (the callback runs later, inside the UI task's lock).
            if (ui_sync_lock()) {
                lv_async_call(forget_finished, nullptr);
                ui_sync_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void forget_wifi() {  // kAction on the LVGL thread: queues the work, returns at once
    if (s_forget_state == ForgetState::kPending) return;
    if (!s_forget_task) {
        xTaskCreate(forget_task, "ForgetWifi", 6144, nullptr, 2, &s_forget_task);
        if (!s_forget_task) {
            Serial.println("[SETTINGS] Cannot start the Forget Wi-Fi task");
            return;
        }
    }
    s_forget_state = ForgetState::kPending;
    s_forget_requested = true;
    hande_settings_refresh();
}

bool forget_status_visible() { return s_forget_state != ForgetState::kIdle; }

const char* forget_status_text(char* buf, size_t len) {
    switch (s_forget_state) {
        case ForgetState::kPending: snprintf(buf, len, "Forgetting..."); break;
        case ForgetState::kDone: snprintf(buf, len, "Forgotten"); break;
        case ForgetState::kFailed: snprintf(buf, len, "Could not erase"); break;
        default: buf[0] = '\0'; break;
    }
    return buf;
}

const HandeSettingsRow kWifiRows[] = {
    hande_row_choice("Wi-Fi off when idle", kWifiOffLabels, 4, get_wifi_off, set_wifi_off),
    hande_row_when(hande_row_info("Saved network", forget_status_text), forget_status_visible),
    hande_row_action("Forget Wi-Fi", forget_wifi, "Tap again to forget"),
};
const HandeSettingsSection kWifiExtra = {"WI-FI", kWifiRows, sizeof(kWifiRows) / sizeof(kWifiRows[0])};
const HandeSettingsRow kAboutRows[] = {
    hande_row_info("MAC address", mac_text),
};
const HandeSettingsSection kAboutExtra = {"NETWORK", kAboutRows, sizeof(kAboutRows) / sizeof(kAboutRows[0])};

// ---- open / close ---------------------------------------------------------------------------------------------

bool s_closing_internally = false;

void return_to_main(void*) {
    // Leaving through the close button: same path as the old "back to app" (ui_set_state handles the indev, dock
    // and map restore; it also defers itself while a finger is still down).
    if (ui_get_state() == UI_STATE_SETTINGS) ui_set_state(UI_STATE_MAIN);
}

void on_settings_close() {
    if (s_closing_internally) return;  // ui_set_state is already leaving UI_STATE_SETTINGS
    lv_async_call(return_to_main, nullptr);
}

}  // namespace

uint32_t ui_settings_theme_accent() { return to_rgb888(theme_primary()); }

void ui_settings_show() {
    HandeSettingsConfig cfg{};
    cfg.app_name = HANDE_APP_NAME;
    cfg.accent = ui_settings_theme_accent();
    cfg.tiles = kTiles;
    cfg.tile_count = sizeof(kTiles) / sizeof(kTiles[0]);
    cfg.display = &kDisplayPage;
    cfg.display_summary = display_summary;
    cfg.clock = &kClockPage;
    cfg.clock_summary = clock_summary;
    cfg.wifi_extra = &kWifiExtra;
    cfg.about_extra = &kAboutExtra;
    cfg.wifi_connected = wifi_is_connected;
    cfg.wifi_ssid = wifi_get_ssid;
    cfg.wifi_rssi = wifi_get_rssi;
    cfg.wifi_ip = wifi_get_ip_str;
    cfg.on_close = on_settings_close;
    hande_settings_open(cfg);
}

void ui_settings_hide() {
    s_closing_internally = true;
    hande_settings_close();
    s_closing_internally = false;
}

bool ui_settings_is_visible() { return hande_settings_is_open(); }

void ui_settings_check_deferred_saves() {
    static uint32_t last_save_time = 0;
    uint32_t now = millis();

    if (ui_settings_save_pending && (now - last_save_time > 500)) {
        Serial.println("[SETTINGS] Deferred NVS save executing");
        settings_save_to_nvs();
        ui_settings_save_pending = false;
        last_save_time = now;
    }
}
