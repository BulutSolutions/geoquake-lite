#pragma once

// Settings: the settings screen (shared words and row helpers below).
// Opened by the gear button.
//
// Structure:
//   Settings hub: a grid of category tiles, each with an icon, a name and a live one-line summary.
//     <APP NAME>  the app's own categories (HandeSettingsConfig::tiles), icons in the app's accent
//     DEVICE      the common categories, same names, icons and order:
//                 Display, Clock & HUD (the app's rows), Wi-Fi, About (built here; the app may add rows)
//   Category page: sections of rows; a link row opens a deeper page (at most 3 levels below the hub).
// Navigation: back button (top left) or a swipe to the right goes up one level (on the hub it closes Settings);
// the close button (top right) always leaves Settings. Pages slide in from the side they belong to.
//
// An app does not build any settings UI. It describes its pages as static tables of rows (label + getter +
// setter); this module draws them (black screen, rows on black, Montserrat 24/20/16, the accent colour) and calls
// the setters. The app keeps its own storage, so saved settings keep their values.
//
// Labels and section titles of the common pages are shared words (HANDE_SETTINGS_* below), so a setting has one
// name.
//
// Threads: everything on the app's LVGL thread. Fonts: LV_FONT_MONTSERRAT_16, _20, _24 (the System Menu's).

#include <stddef.h>
#include <stdint.h>
#include <lvgl.h>

enum class HandeSettingsRowType : uint8_t {
    kToggle,   // label (+ detail) and a switch: get_bool / set_bool
    kSlider,   // label, value text, slider min..max: get_int / set_int (called while dragging), format
    kChoice,   // label and 2..4 segments: get_int / set_int with the segment index, choices
    kStepper,  // label, [-] value [+]: get_int / set_int, min..max by step, format; holding repeats
    kInfo,     // label and a value: value_text
    kAction,   // a button: on_action; confirm = second tap within 3 s; danger = coral
    kLink,     // label, value, chevron: opens page (a deeper level of the current category)
    kCustom,   // the app draws inside the card: build(card)
};

struct HandeSettingsPage;

// Build rows with the hande_row_*() helpers below. They are constexpr, so a `const HandeSettingsRow rows[]` table
// of them is built at compile time and stays in flash (no RAM).
struct HandeSettingsRow {
    HandeSettingsRowType type;
    const char* label;
    const char* detail;     // optional second line under the label (static)
    bool (*visible)();      // nullptr = always shown; re-checked whenever a value changes

    bool (*get_bool)();
    void (*set_bool)(bool);

    int32_t (*get_int)();
    void (*set_int)(int32_t);     // kSlider: called while dragging (live preview)
    void (*commit_int)(int32_t);  // kSlider: optional, once when the finger lifts (save, heavy work)
    int32_t min;
    int32_t max;
    int32_t step;
    bool wrap;                    // kStepper: past max goes to min and back
    const char* const* choices;   // kChoice labels (2..6; more than 4 wrap onto two lines)
    uint8_t choice_count;
    // Value text for kSlider / kStepper (nullptr = the number); writes into buf, returns buf.
    const char* (*format)(int32_t value, char* buf, size_t len);

    // kInfo value and kLink value (nullptr = none); writes into buf, returns buf.
    const char* (*value_text)(char* buf, size_t len);

    void (*on_action)();
    const char* confirm;  // kAction: text shown after the first tap ("Tap again to reset"); nullptr = no confirm
    bool danger;

    const HandeSettingsPage* page;     // kLink
    void (*build)(lv_obj_t* card);     // kCustom
    bool repeat;                       // kStepper: holding [-]/[+] keeps stepping (default; hande_row_tap_only())

    constexpr HandeSettingsRow(HandeSettingsRowType type_, const char* label_, const char* detail_, bool (*visible_)(),
                               bool (*get_bool_)(), void (*set_bool_)(bool), int32_t (*get_int_)(),
                               void (*set_int_)(int32_t), void (*commit_int_)(int32_t), int32_t min_, int32_t max_,
                               int32_t step_, bool wrap_, const char* const* choices_, uint8_t choice_count_,
                               const char* (*format_)(int32_t, char*, size_t),
                               const char* (*value_text_)(char*, size_t), void (*on_action_)(), const char* confirm_,
                               bool danger_, const HandeSettingsPage* page_, void (*build_)(lv_obj_t*),
                               bool repeat_ = false)
        : type(type_), label(label_), detail(detail_), visible(visible_), get_bool(get_bool_), set_bool(set_bool_),
          get_int(get_int_), set_int(set_int_), commit_int(commit_int_), min(min_), max(max_), step(step_),
          wrap(wrap_), choices(choices_), choice_count(choice_count_), format(format_), value_text(value_text_),
          on_action(on_action_), confirm(confirm_), danger(danger_), page(page_), build(build_),
          repeat(repeat_) {}
};

struct HandeSettingsSection {
    const char* title;  // upper-case by convention ("NIGHT"); nullptr or "" = no title (a page's first section)
    const HandeSettingsRow* rows;
    uint8_t row_count;
};

struct HandeSettingsPage {
    const char* title;  // "Settings" for the first page; a sub-page's name
    const HandeSettingsSection* sections;
    uint8_t section_count;
};

// A category tile on the hub: opens `page`. summary: live state under the name ("M2.5+ · every 5 min"), written
// into buf; nullptr = no second line.
struct HandeSettingsTile {
    const char* icon;   // LV_SYMBOL_*
    const char* title;  // also the page header
    const HandeSettingsPage* page;
    const char* (*summary)(char* buf, size_t len);
};

struct HandeSettingsConfig {
    const char* app_name;   // under the title ("GeoQuake") and as the hub's first group title
    uint32_t accent;        // 0xRRGGBB
    // The app's own categories, first on the hub (1..4 tiles; Theme last by convention).
    const HandeSettingsTile* tiles;
    uint8_t tile_count;
    // Common categories: the app's rows for them (nullptr = the app has none: the tile is not shown).
    const HandeSettingsPage* display;
    const char* (*display_summary)(char* buf, size_t len);
    const HandeSettingsPage* clock;
    const char* (*clock_summary)(char* buf, size_t len);
    // Optional app rows on the shared pages: Wi-Fi (e.g. "Wi-Fi off when idle"), About (e.g. MAC address).
    const HandeSettingsSection* wifi_extra;
    const HandeSettingsSection* about_extra;
    void (*on_open)();              // optional: pause the app's own content
    void (*on_close)();             // optional: resume it; re-read anything the settings changed
    // Wi-Fi status for the shared Wi-Fi page, from the app's own wifi_manager (the only code that may call WiFi.*,
    // under its own locking). Any may be nullptr. state_text: optional override ("Connecting...", "Off").
    bool (*wifi_connected)();
    const char* (*wifi_ssid)();
    int32_t (*wifi_rssi)();
    const char* (*wifi_ip)();
    const char* (*wifi_state_text)();
};

// Shared words: the common pages use these section titles (and the row labels listed below), so a setting reads
// the same everywhere. Display page: an untitled first section
// (Brightness), NIGHT (Dim at night, Starts, Ends, ...), SLEEP (Screen off ...). Clock & HUD page: CLOCK, HUD.
#define HANDE_SETTINGS_NIGHT "NIGHT"
#define HANDE_SETTINGS_SLEEP "SLEEP"
#define HANDE_SETTINGS_CLOCK "CLOCK"
#define HANDE_SETTINGS_HUD "STATUS & HUD"

// The gear button: 56 x 56 (+16 px touch margin), top-right corner.
lv_obj_t* hande_settings_gear_create(lv_obj_t* parent, void (*on_click)());
// Rectangle of the gear, for callers that hit-test by hand.
void hande_settings_gear_area(lv_area_t* area);

void hande_settings_open(const HandeSettingsConfig& config);
void hande_settings_close();
bool hande_settings_is_open();
// Re-read every getter and tile summary on the visible page and which rows are shown (call after the app changed a
// value itself; Settings also does it every 2 s).
void hande_settings_refresh();
void hande_settings_set_accent(uint32_t accent);
// Touch rules for a slider an app builds itself in a custom row (core's slider rows already have them): horizontal
// drags stay in the slider, a vertical swipe over it scrolls the list and never sets it.
void hande_settings_slider_guard(lv_obj_t* slider);

// Where the user is (UI tests, logs): 0 = the hub, 1 = a category page, 2+ = deeper; the header title.
uint8_t hande_settings_depth();
const char* hande_settings_title();

// For builds whose LVGL has no input device: feed every touch sample while Settings is open; Settings has
// its own input device and reads each sample at once, so scrolling, sliders and quick taps work like everywhere else.
void hande_settings_touch(bool pressed, int16_t x, int16_t y);

// ---- Row helpers (constexpr: tables of them live in flash) -------------------------------------------------
constexpr HandeSettingsRow hande_row_toggle(const char* label, bool (*get)(), void (*set)(bool),
                                            const char* detail = nullptr) {
    return HandeSettingsRow(HandeSettingsRowType::kToggle, label, detail, nullptr, get, set, nullptr, nullptr,
                            nullptr, 0, 1, 1, false, nullptr, 0, nullptr, nullptr, nullptr, nullptr, false, nullptr,
                            nullptr);
}
constexpr HandeSettingsRow hande_row_slider(const char* label, int32_t min, int32_t max, int32_t (*get)(),
                                            void (*set)(int32_t),
                                            const char* (*format)(int32_t, char*, size_t) = nullptr,
                                            void (*commit)(int32_t) = nullptr) {
    return HandeSettingsRow(HandeSettingsRowType::kSlider, label, nullptr, nullptr, nullptr, nullptr, get, set,
                            commit, min, max, 1, false, nullptr, 0, format, nullptr, nullptr, nullptr, false,
                            nullptr, nullptr);
}
constexpr HandeSettingsRow hande_row_choice(const char* label, const char* const* choices, uint8_t count,
                                            int32_t (*get)(), void (*set)(int32_t)) {
    return HandeSettingsRow(HandeSettingsRowType::kChoice, label, nullptr, nullptr, nullptr, nullptr, get, set,
                            nullptr, 0, count > 0 ? count - 1 : 0, 1, false, choices, count, nullptr, nullptr,
                            nullptr, nullptr, false, nullptr, nullptr);
}
constexpr HandeSettingsRow hande_row_stepper(const char* label, int32_t min, int32_t max, int32_t step,
                                             int32_t (*get)(), void (*set)(int32_t),
                                             const char* (*format)(int32_t, char*, size_t) = nullptr,
                                             bool wrap = false) {
    return HandeSettingsRow(HandeSettingsRowType::kStepper, label, nullptr, nullptr, nullptr, nullptr, get, set,
                            nullptr, min, max, step, wrap, nullptr, 0, format, nullptr, nullptr, nullptr, false,
                            nullptr, nullptr, true);
}
constexpr HandeSettingsRow hande_row_info(const char* label, const char* (*value)(char*, size_t)) {
    return HandeSettingsRow(HandeSettingsRowType::kInfo, label, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, 0, 1, 1, false, nullptr, 0, nullptr, value, nullptr, nullptr, false, nullptr,
                            nullptr);
}
constexpr HandeSettingsRow hande_row_action(const char* label, void (*action)(), const char* confirm = nullptr,
                                            bool danger = false) {
    return HandeSettingsRow(HandeSettingsRowType::kAction, label, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, 0, 1, 1, false, nullptr, 0, nullptr, nullptr, action, confirm, danger,
                            nullptr, nullptr);
}
constexpr HandeSettingsRow hande_row_link(const char* label, const HandeSettingsPage* page,
                                          const char* (*value)(char*, size_t) = nullptr) {
    return HandeSettingsRow(HandeSettingsRowType::kLink, label, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, 0, 1, 1, false, nullptr, 0, nullptr, value, nullptr, nullptr, false, page,
                            nullptr);
}
constexpr HandeSettingsRow hande_row_custom(const char* label, void (*build)(lv_obj_t*)) {
    return HandeSettingsRow(HandeSettingsRowType::kCustom, label, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, 0, 1, 1, false, nullptr, 0, nullptr, nullptr, nullptr, nullptr, false,
                            nullptr, build);
}
// Shown only while visible() is true (re-checked after every change, e.g. "Night starts" only with Night dim on).
constexpr HandeSettingsRow hande_row_when(HandeSettingsRow r, bool (*visible)()) {
    return HandeSettingsRow(r.type, r.label, r.detail, visible, r.get_bool, r.set_bool, r.get_int, r.set_int,
                            r.commit_int, r.min, r.max, r.step, r.wrap, r.choices, r.choice_count, r.format,
                            r.value_text, r.on_action, r.confirm, r.danger, r.page, r.build, r.repeat);
}
// A stepper that steps once per tap, also when held: for setters with a side effect per step (a sound preview).
constexpr HandeSettingsRow hande_row_tap_only(HandeSettingsRow r) {
    return HandeSettingsRow(r.type, r.label, r.detail, r.visible, r.get_bool, r.set_bool, r.get_int, r.set_int,
                            r.commit_int, r.min, r.max, r.step, r.wrap, r.choices, r.choice_count, r.format,
                            r.value_text, r.on_action, r.confirm, r.danger, r.page, r.build, false);
}
