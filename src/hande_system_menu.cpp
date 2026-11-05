// The System Menu. See hande_system_menu.h for how it is used and hande_system_menu_policy.h for the button,
// timing and layout rules (host-tested).
#include "hande_system_menu.h"

#include "hande_system_menu_policy.h"

#include <Arduino.h>
#include <esp_system.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {
using namespace hande_system_menu;

constexpr uint32_t kBlack = 0x000000;
constexpr uint32_t kPrimary = 0xE0E0E0;    // AMOLED: no pure white on black
constexpr uint32_t kSecondary = 0xA0A0A0;
constexpr uint32_t kDisabled = 0x606060;
constexpr uint32_t kDanger = 0xFF7755;
constexpr uint32_t kStatusEveryMs = 250U;
constexpr uint32_t kWakeEveryMs = 500U;

HandeSystemMenuConfig s_cfg{};
ButtonHold s_hold{};
Page s_page = Page::HOLD;
bool s_active = false;
uint32_t s_shown_since = 0;
uint32_t s_status_checked = 0;
uint32_t s_woken = 0;
uint8_t s_last_progress = 255;
bool s_busy = false;
char s_confirm_title[48] = "";

lv_obj_t* s_overlay = nullptr;
lv_obj_t* s_pages[3] = {};
lv_obj_t* s_arc = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_restart_btn = nullptr;
lv_obj_t* s_reset_btn = nullptr;
lv_obj_t* s_confirm_btn = nullptr;

void handle(Action action);

lv_obj_t* make_page(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 480, 480);
    lv_obj_set_style_bg_color(page, lv_color_hex(kBlack), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

lv_obj_t* make_label(lv_obj_t* parent, const char* text, const lv_font_t* font, int16_t y, uint32_t rgb) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text_static(label, text);
    lv_obj_set_pos(label, 40, y);
    lv_obj_set_width(label, 400);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(rgb), 0);
    return label;
}

void on_button(lv_event_t* e) {
    handle(static_cast<Action>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
}

void on_outside(lv_event_t* e) {
    // A tap on the page itself (not on a button: buttons do not bubble their clicks) closes the menu.
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) handle(Action::OUTSIDE);
}

lv_obj_t* make_button(lv_obj_t* parent, const Rect& r, const char* text, uint32_t border, Action action) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, r.x, r.y);
    lv_obj_set_size(button, r.w, r.h);
    lv_obj_set_ext_click_area(button, kTouchSlop);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(button, lv_color_hex(kBlack), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(kDisabled), LV_STATE_DISABLED);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, on_button, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(action)));
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text_static(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kPrimary), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kDisabled), LV_STATE_DISABLED);
    lv_obj_center(label);
    return button;
}

void set_enabled(lv_obj_t* button, bool enabled) {
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
        lv_obj_remove_state(label, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_add_state(label, LV_STATE_DISABLED);
    }
}

void set_status(const char* text) { lv_label_set_text_static(s_status, text); }

void show(Page page) {
    s_page = page;
    lv_obj_move_foreground(s_overlay);  // above anything the app put on lv_layer_top() after init
    for (uint8_t i = 0; i < 3; ++i) {
        if (i == static_cast<uint8_t>(page)) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool busy_now() { return s_cfg.busy != nullptr && s_cfg.busy(); }

void wake(uint32_t now) {
    if (s_cfg.wake_display != nullptr) s_cfg.wake_display();
    s_woken = now;
}

void open_hold(uint32_t now) {
    s_active = true;
    s_last_progress = 255;
    lv_arc_set_value(s_arc, 0);
    set_status("");
    if (s_cfg.on_open != nullptr) s_cfg.on_open();
    wake(now);
    show(Page::HOLD);
    Serial.println("[SYSTEM_MENU] BOOT held: hold ring shown");
}

void close_menu(const char* why) {
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_active = false;
    // A button still held now must be released before it can open the menu again.
    s_hold.reset(digitalRead(kButtonPin) == LOW, millis());
    if (s_cfg.on_close != nullptr) s_cfg.on_close();
    Serial.printf("[SYSTEM_MENU] closed (%s)\n", why);
}

void refresh_status(uint32_t now, bool force) {
    if (!force && now - s_status_checked < kStatusEveryMs) return;
    s_status_checked = now;
    const bool busy = busy_now();
    if (!force && busy == s_busy) return;
    s_busy = busy;
    set_enabled(s_restart_btn, !busy);
    set_enabled(s_reset_btn, !busy);
    set_enabled(s_confirm_btn, !busy);
    set_status(busy ? "Wait for the transfer to finish." : "");
}

// Leaves the app (does not return) or reports why it could not.
void leave(Action action) {
    if (s_cfg.begin_action != nullptr && !s_cfg.begin_action()) {
        set_status("Busy. Wait a moment and try again.");
        return;
    }
    if (action == Action::RESTART) {
        Serial.println("[SYSTEM_MENU] Restart");
        esp_restart();
    } else if (action == Action::RESET_CONFIRMED) {
        set_status("Resetting...");
        lv_refr_now(nullptr);
        if (s_cfg.factory_reset != nullptr && s_cfg.factory_reset()) {
            Serial.printf("[SYSTEM_MENU] %s reset; restarting\n", s_cfg.app_name);
            esp_restart();
        }
        set_status("Reset did not finish. Try again.");
    }
    if (s_cfg.end_action != nullptr) s_cfg.end_action();
}

void handle(Action action) {
    if (!s_active || s_page == Page::HOLD) return;
    switch (action) {
        case Action::CANCEL:
        case Action::OUTSIDE:
            close_menu(action == Action::CANCEL ? "cancel" : "tap outside");
            break;
        case Action::RESET_PROMPT:
            if (busy_now()) {
                refresh_status(millis(), true);
                break;
            }
            s_shown_since = millis();
            show(Page::RESET_CONFIRM);
            Serial.println("[SYSTEM_MENU] Factory Reset needs confirmation");
            break;
        case Action::RESTART:
        case Action::RESET_CONFIRMED:
            if (busy_now()) {
                refresh_status(millis(), true);
                break;
            }
            leave(action);
            break;
        default:
            break;
    }
}
}  // namespace

void hande_system_menu_init(const HandeSystemMenuConfig& config) {
    s_cfg = config;
    pinMode(kButtonPin, INPUT_PULLUP);
    s_hold.reset(digitalRead(kButtonPin) == LOW, millis());
    snprintf(s_confirm_title, sizeof(s_confirm_title), "Reset %s?", config.app_name ? config.app_name : "app");

    s_overlay = make_page(lv_layer_top());
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < 3; ++i) {
        s_pages[i] = make_page(s_overlay);
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* hold = s_pages[static_cast<uint8_t>(Page::HOLD)];
    s_arc = lv_arc_create(hold);
    lv_obj_set_size(s_arc, 210, 210);
    lv_obj_set_pos(s_arc, 135, 116);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x303030), LV_PART_MAIN);
    make_label(hold, "Hold for\nSystem Menu", &lv_font_montserrat_20, 193, kPrimary);
    make_label(hold, "Release to cancel", &lv_font_montserrat_16, 352, kSecondary);

    lv_obj_t* choices = s_pages[static_cast<uint8_t>(Page::CHOICES)];
    lv_obj_add_flag(choices, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(choices, on_outside, LV_EVENT_CLICKED, nullptr);
    make_label(choices, "System Menu", &lv_font_montserrat_24, 64, kPrimary);
    make_label(choices, config.app_name ? config.app_name : "", &lv_font_montserrat_16, 112, kSecondary);
    s_restart_btn = make_button(choices, kRestartButton, "Restart", config.accent, Action::RESTART);
    make_button(choices, kCancelButton, "Cancel", kSecondary, Action::CANCEL);
    s_reset_btn = make_button(choices, kResetButton, "Factory Reset", kDanger, Action::RESET_PROMPT);

    lv_obj_t* confirm = s_pages[static_cast<uint8_t>(Page::RESET_CONFIRM)];
    lv_obj_add_flag(confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(confirm, on_outside, LV_EVENT_CLICKED, nullptr);
    make_label(confirm, s_confirm_title, &lv_font_montserrat_24, 104, kPrimary);
    make_label(confirm, config.reset_text ? config.reset_text : "", &lv_font_montserrat_16, 160, kSecondary);
    make_button(confirm, kConfirmCancelButton, "Cancel", kSecondary, Action::CANCEL);
    s_confirm_btn = make_button(confirm, kConfirmResetButton, "Erase and reset", kDanger, Action::RESET_CONFIRMED);

    s_status = make_label(s_overlay, "", &lv_font_montserrat_16, 384, kSecondary);
    hande_system_menu_set_accent(config.accent);
    Serial.println("[SYSTEM_MENU] ready: hold BOOT for 5 seconds");
}

void hande_system_menu_set_accent(uint32_t accent) {
    s_cfg.accent = accent;
    if (s_overlay == nullptr) return;
    const lv_color_t color = lv_color_hex(accent);
    lv_obj_set_style_arc_color(s_arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_restart_btn, color, 0);
}

void hande_system_menu_tick() {
    if (s_overlay == nullptr) return;
    const uint32_t now = millis();
    const HoldEvent event = s_hold.update(digitalRead(kButtonPin) == LOW, now);

    if (!s_active) {
        if (event == HoldEvent::STARTED) open_hold(now);
        return;
    }
    if (now - s_woken >= kWakeEveryMs) wake(now);  // e.g. a remote brightness change must not hide the menu

    if (s_page == Page::HOLD) {
        if (event == HoldEvent::CANCELLED) {
            close_menu("released before 5 s");
            return;
        }
        if (s_hold.progress != s_last_progress) {
            s_last_progress = s_hold.progress;
            lv_arc_set_value(s_arc, s_last_progress);
        }
        if (event == HoldEvent::COMPLETED) {
            s_shown_since = now;
            refresh_status(now, true);
            show(Page::CHOICES);
            Serial.println("[SYSTEM_MENU] menu shown");
        }
        return;
    }
    if (choice_expired(now, s_shown_since)) {
        close_menu("15 s timeout");
        return;
    }
    refresh_status(now, false);
}

bool hande_system_menu_active() { return s_active; }

void hande_system_menu_open_reset() {
    if (s_overlay == nullptr) return;
    const uint32_t now = millis();
    if (!s_active) {
        s_active = true;
        if (s_cfg.on_open != nullptr) s_cfg.on_open();
        wake(now);
    }
    s_shown_since = now;
    refresh_status(now, true);
    show(Page::RESET_CONFIRM);
    Serial.println("[SYSTEM_MENU] Factory Reset opened from the app");
}

void hande_system_menu_tap(int16_t x, int16_t y) {
    if (!s_active) return;
    handle(hit_test(s_page, x, y));
}
