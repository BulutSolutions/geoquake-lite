#include "ui_button_dock.h"
#include "ui_display.h"
#include "ui_state.h"
#include "ui_sync.h"
#include "theme.h"
#include "hande_settings.h"
#include "icons/icons.h"
#include <Arduino.h>


/**
 * Button Dock for Circular 466x466 Display
 *
 * CRITICAL: This is a CIRCULAR AMOLED display, NOT rectangular!
 *
 * Display Geometry:
 * - Screen Size: 466x466 pixels
 * - Center Point: (233, 233)
 * - Visible Radius: ~233 pixels
 * - Safe Zone: 180-210 pixels from center (guaranteed visibility)
 * - CORNERS ARE NOT VISIBLE (>280px from center)
 *
 * Layout:
 *         ┌────────────┐
 *        ╱              ╲
 *       │                │
 *    🔔 │   [Map View]   │     ← Notifications LEFT; Settings = gear, top-right
 *       │                │
 *        ╲              ╱       ← Status label (tap to refresh)
 *         └────────────┘
 *
 * Button Positions (optimized for circular display visibility):
 * - Notifications: LEFT_MID (10, 0) → X 10-60, Y 208-258, distance from center ~185px ✅
 * - Settings:      gear, TOP_RIGHT (-16, 16), 56x56 + 16 px touch margin (hande_settings_gear_area)
 *
 * Vertical Spacing Guidelines (Bottom Section):
 * - Battery Container:  BOTTOM_MID (0, -5)   → Y ≈461
 * - Status Label:       BOTTOM_MID (0, -25)  → Y ≈433-461 (pill background, tappable for refresh)
 * - Minimum gap between elements: 60+ pixels to prevent visual overlap
 *
 * When positioning new elements:
 * 1. Calculate distance from center: sqrt((x-233)^2 + (y-233)^2)
 * 2. Ensure distance < 210px for guaranteed visibility
 * 3. Avoid corners (TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT)
 * 4. Prefer center alignments (TOP_MID, BOTTOM_MID, LEFT_MID, RIGHT_MID)
 * 5. Account for element size + padding when calculating gaps
 */

// Button dock structure
typedef struct {
    lv_obj_t *btn_settings;       // gear (top-right)
    lv_obj_t *btn_notifications;  // LEFT edge button
    lv_obj_t *icon_settings;
    lv_obj_t *icon_notifications;
} ButtonDock;

ButtonDock dock = {0};  // Global for test access

lv_obj_t* ui_dock_get_settings_button() {
    return dock.btn_settings;
}

lv_obj_t* ui_dock_get_notifications_button() {
    return dock.btn_notifications;
}

// Forward declarations
static void gear_clicked();
static void btn_notif_cb(lv_event_t *e);

// ===== DOCK CREATION =====

void ui_dock_create(lv_obj_t *parent) {
    if (!parent) {
        Serial.println("[DOCK] Error: parent is NULL");
        return;
    }

    Serial.println("[DOCK] Creating button dock for circular display");

    // Settings: the gear (hande_settings.cpp), top-right corner.
    dock.btn_settings = hande_settings_gear_create(parent, gear_clicked);
    dock.icon_settings = NULL;  // the gear draws its own icon in the shared look

    // Notifications button - LEFT edge (vertical center)
    dock.btn_notifications = lv_button_create(parent);
    lv_obj_set_size(dock.btn_notifications, 50, 50);
    lv_obj_align(dock.btn_notifications, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(dock.btn_notifications, 25, 0);  // Circular button
    lv_obj_set_style_bg_color(dock.btn_notifications, theme_bg_card(), 0);
    lv_obj_set_style_bg_opa(dock.btn_notifications, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dock.btn_notifications, 2, 0);
    lv_obj_set_style_border_color(dock.btn_notifications, theme_primary(), 0);
    lv_obj_set_style_shadow_width(dock.btn_notifications, 0, 0);
    lv_obj_set_style_bg_color(dock.btn_notifications, theme_primary_dim(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(dock.btn_notifications, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(dock.btn_notifications, theme_border_default(), LV_STATE_DISABLED);

    dock.icon_notifications = lv_label_create(dock.btn_notifications);
    lv_label_set_text(dock.icon_notifications, ICON_EARTHQUAKE);
    lv_obj_set_style_text_font(dock.icon_notifications, &icon_material_24, 0);
    lv_obj_set_style_text_color(dock.icon_notifications, theme_primary(), 0);
    lv_obj_center(dock.icon_notifications);
    lv_obj_add_event_cb(dock.btn_notifications, btn_notif_cb, LV_EVENT_CLICKED, NULL);

    Serial.println("[DOCK] Button dock created successfully");
}

// ===== DOCK DESTRUCTION =====

void ui_dock_destroy() {
    Serial.println("[DOCK] Destroying button dock");

    if (dock.btn_settings) {
        lv_obj_delete(dock.btn_settings);
        dock.btn_settings = NULL;
    }

    if (dock.btn_notifications) {
        lv_obj_delete(dock.btn_notifications);
        dock.btn_notifications = NULL;
    }

    // Badge and icons will be deleted with parent buttons
    dock.icon_settings = NULL;
    dock.icon_notifications = NULL;

    Serial.println("[DOCK] Button dock destroyed");
}

// ===== EVENT HANDLERS =====

static void gear_clicked() {
    Serial.println("[DOCK] Settings gear clicked");
    ui_queue_set_state(UI_STATE_SETTINGS);
}

static void btn_notif_cb(lv_event_t *e) {
    Serial.println("[DOCK] Notifications button clicked");

    ui_toggle_notifications();
}



// ===== VISIBILITY =====

void ui_dock_show() {
    // Clear hidden flags - caller (ui_auto_hide_show_internal) handles screen invalidation
    if (dock.btn_settings) {
        lv_obj_remove_flag(dock.btn_settings, LV_OBJ_FLAG_HIDDEN);
    }

    if (dock.btn_notifications) {
        lv_obj_remove_flag(dock.btn_notifications, LV_OBJ_FLAG_HIDDEN);
    }

    Serial.println("[DOCK] Dock buttons shown");
}

void ui_dock_hide() {
    // Add hidden flags - caller (ui_auto_hide_hide_internal) handles screen invalidation
    if (dock.btn_settings) {
        lv_obj_add_flag(dock.btn_settings, LV_OBJ_FLAG_HIDDEN);
    }

    if (dock.btn_notifications) {
        lv_obj_add_flag(dock.btn_notifications, LV_OBJ_FLAG_HIDDEN);
    }

    Serial.println("[DOCK] Dock buttons hidden");
}

void ui_dock_set_drag_hidden(bool hidden) {
    lv_obj_t* buttons[] = {dock.btn_settings, dock.btn_notifications};
    for (lv_obj_t* b : buttons) {
        if (!b) continue;
        if (hidden) {
            lv_obj_set_style_opa_layered(b, LV_OPA_TRANSP, 0);
        } else {
            lv_obj_remove_local_style_prop(b, LV_STYLE_OPA_LAYERED, 0);
        }
    }
}

void ui_dock_apply_theme() {
    // The settings gear keeps its fixed look; only the notifications button follows the theme.
    if (dock.btn_notifications) {
        lv_obj_set_style_bg_color(dock.btn_notifications, theme_bg_card(), 0);
        lv_obj_set_style_border_color(dock.btn_notifications, theme_primary(), 0);
    }
    if (dock.icon_notifications) {
        lv_obj_set_style_text_color(dock.icon_notifications, theme_primary(), 0);
        lv_obj_invalidate(dock.icon_notifications);
    }

}

// ===== TOUCH DETECTION =====

bool ui_dock_is_touch_on_dock(int16_t x, int16_t y) {
    // Settings gear (top-right, shared rectangle incl. its 16 px touch margin): LVGL handles it, not the map
    lv_area_t gear;
    hande_settings_gear_area(&gear);
    if (x >= gear.x1 && x <= gear.x2 && y >= gear.y1 && y <= gear.y2) {
        return true;
    }

    // Check if touch is on notifications button (left edge)
    // Button at (10, 208) with size 50x50
    if (x >= 10 && x <= 60 && y >= 208 && y <= 258) {
        return true;
    }

    return false;
}
