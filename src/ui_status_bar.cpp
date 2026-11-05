#include "ui_status_bar.h"
#include "battery.h"
#include "config_constants.h"
#include "theme.h"
#include "quake_loader.h"
#include "settings_manager.h"
#include "wifi_manager.h"
#include "icons/icons.h"
#include "hande_time.h"
#include <time.h>
#include <vector>

// External UI objects from ui_display.cpp
extern lv_obj_t* ui_LblBattery;
extern lv_obj_t* ui_LblBatteryIcon;
extern lv_obj_t* ui_LblCharging;
extern lv_obj_t* ui_LblStatus;
extern lv_obj_t* ui_LblTime;
extern lv_obj_t* ui_LblDate;
extern lv_obj_t* ui_LblAmPm;
extern lv_obj_t* ui_OverlayPanel;
extern lv_obj_t* ui_TimeContainer;
extern lv_obj_t* ui_BatteryContainer;

// Custom fonts
LV_FONT_DECLARE(font_bruno_18);
LV_FONT_DECLARE(font_seg7_48);
LV_FONT_DECLARE(icon_material_24);

// Status override time
static unsigned long status_override_until_ms = 0;

// External sync time from quake_loader
extern time_t last_sync_epoch;

// Low battery (red) and the auto-hidden HUD: the battery stays on screen while the rest of the HUD is hidden
static bool s_battery_low = false;
static bool s_hud_hidden = false;
static bool s_offline = false;  // the status pill shows Offline: it stays on screen while the HUD is hidden

// While the HUD is hidden, show the battery only when it is low (and the user shows the battery at all).
// While the HUD is shown, the show/hide paths (ui_power.cpp, Settings) own the flag.
static void apply_hidden_hud_battery() {
    if (!ui_BatteryContainer || !s_hud_hidden) return;
    const AppSettings* settings = settings_get();
    const bool keep = s_battery_low && (!settings || settings->show_battery);
    if (keep == !lv_obj_has_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN)) return;  // no change: no redraw
    if (keep) lv_obj_remove_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui_BatteryContainer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_obj_get_parent(ui_BatteryContainer));  // the bottom HUD column re-lays out
}

// While the HUD is hidden, show the status pill only when it reads Offline (a stale map must not look empty).
static void apply_hidden_hud_status() {
    if (!ui_LblStatus || !s_hud_hidden) return;
    const bool keep = s_offline;
    if (keep == !lv_obj_has_flag(ui_LblStatus, LV_OBJ_FLAG_HIDDEN)) return;  // no change: no redraw
    if (keep) lv_obj_remove_flag(ui_LblStatus, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui_LblStatus, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_obj_get_parent(ui_LblStatus));  // the bottom HUD column re-lays out
}

void ui_battery_set_hud_hidden(bool hidden) {
    s_hud_hidden = hidden;
    apply_hidden_hud_battery();
    apply_hidden_hud_status();
}

// --- Public API Implementation ---

static void update_battery_labels() {
    if (!ui_LblBattery || !ui_LblBatteryIcon) return;
    s_battery_low = false;  // set below when a present battery is low

    int battery_pct = battery_get_percent();
    bool battery_connected = battery_is_connected();
    bool charging = battery_is_charging();
    bool usb_connected = battery_is_usb_connected();
    int battery_mv = battery_get_voltage();

    // Heuristic: when on USB power with no battery installed, PMU may report a low voltage.
    bool battery_present = battery_connected;
    if (battery_present && usb_connected && !charging && battery_mv > 0 && battery_mv < 2600) {
        battery_present = false;
    }

    if (!battery_present && !usb_connected) {
        lv_obj_add_flag(ui_LblBatteryIcon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBatteryIcon);
        lv_obj_add_flag(ui_LblBattery, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBattery);
        if (ui_LblCharging) {
            lv_obj_add_flag(ui_LblCharging, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_LblCharging);
        }
        return;
    }

    if (!battery_present) {
        // USB-powered with no battery detected: show USB icon only.
        lv_obj_add_flag(ui_LblBatteryIcon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBatteryIcon);
        lv_obj_add_flag(ui_LblBattery, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBattery);
        if (ui_LblCharging) {
            if (usb_connected) {
                lv_label_set_text(ui_LblCharging, ICON_USB);
                lv_obj_set_style_text_color(ui_LblCharging, theme_primary(), 0);
                lv_obj_remove_flag(ui_LblCharging, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ui_LblCharging, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_invalidate(ui_LblCharging);
        }
        return;
    }

    if (battery_present && battery_pct >= 0) {
        lv_label_set_text_fmt(ui_LblBattery, "%d%%", battery_pct);
        lv_obj_remove_flag(ui_LblBatteryIcon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBatteryIcon);
        lv_obj_remove_flag(ui_LblBattery, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBattery);

        // Choose icon based on charging state and battery level
        // Color follows the active theme via ui_apply_main_theme()
        if (charging) {
            // WORKAROUND: Use bolt icon for charging (charging battery icons not in Material Symbols font)
            lv_label_set_text(ui_LblBatteryIcon, ICON_BOLT);
            lv_obj_invalidate(ui_LblBatteryIcon);
        } else {
            // Show normal battery icon when not charging
            if (battery_pct > 50) {
                lv_label_set_text(ui_LblBatteryIcon, ICON_BATTERY_FULL);
            } else if (battery_pct > 10) {
                lv_label_set_text(ui_LblBatteryIcon, ICON_BATTERY_50);
            } else {
                lv_label_set_text(ui_LblBatteryIcon, ICON_BATTERY_EMPTY);
            }
            lv_obj_invalidate(ui_LblBatteryIcon);
        }

        // Low battery (discharging, <= LOW_BATTERY_WARN_PCT) is drawn red.
        const bool low = !charging && !usb_connected && battery_pct <= LOW_BATTERY_WARN_PCT;
        // Set only on a change: a style write re-renders the label, and this runs every second.
        const lv_color_t color = low ? theme_error() : theme_primary();
        if (!lv_color_eq(lv_obj_get_style_text_color(ui_LblBattery, LV_PART_MAIN), color)) {
            lv_obj_set_style_text_color(ui_LblBatteryIcon, color, 0);
            lv_obj_set_style_text_color(ui_LblBattery, color, 0);
        }
        s_battery_low = low;
    } else {
        lv_obj_add_flag(ui_LblBatteryIcon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBatteryIcon);
        lv_obj_add_flag(ui_LblBattery, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui_LblBattery);
    }

    // Show USB icon when connected via USB but not charging
    if (ui_LblCharging) {
        if (usb_connected && !charging) {
            lv_label_set_text(ui_LblCharging, ICON_USB);
            lv_obj_remove_flag(ui_LblCharging, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_LblCharging);
        } else {
            lv_obj_add_flag(ui_LblCharging, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_LblCharging);
        }
    }
}

void ui_update_battery() {
    update_battery_labels();
    apply_hidden_hud_battery();
}

void ui_update_status(const char* t) {
    if (!ui_LblStatus || !t) return;
    s_offline = false;
    lv_label_set_text(ui_LblStatus, t);
    lv_obj_set_style_text_color(ui_LblStatus, theme_primary(), 0);
    lv_obj_invalidate(ui_LblStatus);
    status_override_until_ms = millis() + 5000;
    apply_hidden_hud_status();
}

void ui_update_status_sync() {
    if (!ui_LblStatus) return;
    if (millis() < status_override_until_ms) return;

    const AppSettings* settings = settings_get();

    const Earthquake* quakes = nullptr;
    size_t total_quakes = 0;
    if (!quake_lock_read(&quakes, &total_quakes)) {
        return;
    }
    const float mag_min = settings ? settings->mag_min : 0.0f;
    size_t quake_count = 0;
    for (size_t i = 0; i < total_quakes; ++i) {
        if (quakes[i].mag >= mag_min) {
            quake_count++;
        }
    }
    quake_unlock_read();
    const char* window_label = "24h";
    if (settings) {
        if (settings->time_range == 0) window_label = "1h";
        else if (settings->time_range == 2) window_label = "7d";
    }
    // The last download failed (no internet, server unreachable) or none can run (no Wi-Fi, e.g. after Use
    // Offline): say "Offline" in the warning colour, with the time of the last good update when there is one, so an
    // empty or stale map never reads as "no earthquakes". A tap on the pill retries; a good sync brings "13/24h" back.
    const bool fetching = is_fetching_quakes.load();
    const bool never_synced = (total_quakes == 0 && last_sync_epoch == 0);
    if (!fetching && (quake_last_fetch_result() != FETCH_SUCCESS || (never_synced && !wifi_is_connected()))) {
        char offline_buf[40] = "Offline";
        struct tm synced;
        if (hande_time_local_at(last_sync_epoch, &synced)) {  // only with a real clock and a known zone
            const bool use_24h = settings ? settings->clock_24h : true;
            char time_buf[12];
            strftime(time_buf, sizeof(time_buf), use_24h ? "%H:%M" : "%I:%M %p", &synced);
            snprintf(offline_buf, sizeof(offline_buf), "Offline - updated %s", time_buf);  // ASCII only: Bruno font
        }
        s_offline = true;
        lv_label_set_text(ui_LblStatus, offline_buf);
        lv_obj_set_style_text_color(ui_LblStatus, theme_warning(), 0);
        lv_obj_invalidate(ui_LblStatus);
        apply_hidden_hud_status();
        return;
    }
    if (never_synced) {
        // Nothing loaded yet and a download is running (or about to)
        s_offline = false;
        lv_label_set_text(ui_LblStatus, "Syncing...");
        lv_obj_set_style_text_color(ui_LblStatus, theme_primary(), 0);
        lv_obj_invalidate(ui_LblStatus);
        apply_hidden_hud_status();
        return;
    }

    char status_buf[64];
    snprintf(status_buf, sizeof(status_buf), "%u/%s",
             (unsigned)quake_count, window_label);
    s_offline = false;
    lv_label_set_text(ui_LblStatus, status_buf);
    lv_obj_set_style_text_color(ui_LblStatus, theme_primary(), 0);
    lv_obj_invalidate(ui_LblStatus);  // Force redraw with new text
    apply_hidden_hud_status();
}

void ui_update_status_text(const char* status_type) {
    if (!ui_LblStatus) return;

    if (strcmp(status_type, "connecting") == 0) {
        s_offline = false;
        lv_label_set_text(ui_LblStatus, "Connecting...");
    } else if (strcmp(status_type, "syncing") == 0) {
        s_offline = false;
        lv_label_set_text(ui_LblStatus, "Syncing...");
    } else if (strcmp(status_type, "restore") == 0) {
        // Restore normal "40/24h" display
        ui_update_status_sync();
        return;  // ui_update_status_sync handles invalidation
    }

    lv_obj_set_style_text_color(ui_LblStatus, theme_primary(), 0);
    lv_obj_invalidate(ui_LblStatus);
    apply_hidden_hud_status();
}

// Only on a change: this runs every second and a style write redraws the pill.
static void set_time_pill_opa(lv_opa_t opa) {
    if (ui_TimeContainer && lv_obj_get_style_bg_opa(ui_TimeContainer, LV_PART_MAIN) != opa) {
        lv_obj_set_style_bg_opa(ui_TimeContainer, opa, 0);
    }
}

void ui_update_clock() {
    if (!ui_LblTime) return;

    // NOTE: Removed lv_obj_invalidate(ui_OverlayPanel) - was causing full-screen
    // redraw every second on the full display. Individual labels are
    // invalidated below after their text changes, which is sufficient.

    struct tm timeinfo;
    if (!hande_time_local(&timeinfo)) {
        // No clock or no zone yet (hande_time.h): no clock at all rather than a wrong "00:00".
        if (lv_label_get_text(ui_LblTime)[0] != '\0') {
            lv_label_set_text(ui_LblTime, "");
            lv_obj_invalidate(ui_LblTime);
        }
        // No empty dark pill either (ui_power owns the container's HIDDEN flag for the HUD).
        set_time_pill_opa(LV_OPA_TRANSP);
        if (ui_LblAmPm) lv_obj_add_flag(ui_LblAmPm, LV_OBJ_FLAG_HIDDEN);
        if (ui_LblDate) lv_obj_add_flag(ui_LblDate, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const AppSettings* settings = settings_get();
    bool use_24h = settings ? settings->clock_24h : true;
    bool show_date = settings ? settings->show_date : true;  // Match settings_manager default

    char time_buf[16];
    if (use_24h) {
        strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
    } else {
        strftime(time_buf, sizeof(time_buf), "%I:%M", &timeinfo);
    }
    lv_label_set_text(ui_LblTime, time_buf);
    lv_obj_invalidate(ui_LblTime);
    set_time_pill_opa(LV_OPA_60);  // the pill from ui_display.cpp

    if (ui_LblAmPm) {
        if (use_24h) {
            lv_label_set_text(ui_LblAmPm, "");
            lv_obj_add_flag(ui_LblAmPm, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_LblAmPm);
        } else {
            char ampm_buf[4];
            strftime(ampm_buf, sizeof(ampm_buf), "%p", &timeinfo);
            lv_label_set_text(ui_LblAmPm, ampm_buf);
            lv_obj_remove_flag(ui_LblAmPm, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_LblAmPm);
        }
    }

    if (ui_LblDate) {
        if (show_date) {
            char date_buf[32];
            strftime(date_buf, sizeof(date_buf), "%b %d", &timeinfo);
            lv_label_set_text(ui_LblDate, date_buf);
            lv_obj_remove_flag(ui_LblDate, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_LblDate);
        } else {
            lv_obj_add_flag(ui_LblDate, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_apply_main_theme() {
    lv_color_t primary = theme_primary();

    if (ui_LblTime) {
        lv_obj_set_style_text_color(ui_LblTime, primary, 0);
        lv_obj_invalidate(ui_LblTime);
    }
    if (ui_LblDate) {
        lv_obj_set_style_text_color(ui_LblDate, primary, 0);
        lv_obj_invalidate(ui_LblDate);
    }
    if (ui_LblAmPm) {
        lv_obj_set_style_text_color(ui_LblAmPm, primary, 0);
        lv_obj_invalidate(ui_LblAmPm);
    }
    if (ui_LblStatus) {
        lv_obj_set_style_text_color(ui_LblStatus, primary, 0);
        lv_obj_invalidate(ui_LblStatus);
    }
    if (ui_LblBatteryIcon) {
        lv_obj_set_style_text_color(ui_LblBatteryIcon, primary, 0);
        lv_obj_invalidate(ui_LblBatteryIcon);
    }
    if (ui_LblBattery) {
        lv_obj_set_style_text_color(ui_LblBattery, primary, 0);
        lv_obj_invalidate(ui_LblBattery);
    }
    if (ui_LblCharging) {
        lv_obj_set_style_text_color(ui_LblCharging, primary, 0);
        lv_obj_invalidate(ui_LblCharging);
    }
}
