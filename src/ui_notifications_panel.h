#ifndef UI_NOTIFICATIONS_PANEL_H
#define UI_NOTIFICATIONS_PANEL_H

#include <Arduino.h>
#include <lvgl.h>

// --- NOTIFICATIONS PANEL UI ---
// Handles notification panel display and earthquake alert list

// Toggle notifications panel visibility
void ui_toggle_notifications();

// Check if notification panel is currently open
bool ui_notification_panel_is_open();

#endif // UI_NOTIFICATIONS_PANEL_H
