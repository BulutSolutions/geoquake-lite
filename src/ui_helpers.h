#ifndef UI_HELPERS_H
#define UI_HELPERS_H

#include <Arduino.h>
#include <lvgl.h>

// --- UI HELPERS ---
// Reusable UI component creation functions to reduce code duplication

// =============================================================================
// LABELS
// =============================================================================

// Create a simple label with text, font, and color
lv_obj_t* ui_create_label(lv_obj_t* parent, const char* text,
                          const lv_font_t* font, lv_color_t color);

// Create a label aligned to parent
lv_obj_t* ui_create_label_aligned(lv_obj_t* parent, const char* text,
                                  const lv_font_t* font, lv_color_t color,
                                  lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs);

// Create an icon label (for icon fonts)
lv_obj_t* ui_create_icon_label(lv_obj_t* parent, const char* icon,
                               const lv_font_t* icon_font, lv_color_t color);

// =============================================================================
// CONTAINERS
// =============================================================================

// Create a container with size, radius, background color and opacity
lv_obj_t* ui_create_container(lv_obj_t* parent, lv_coord_t w, lv_coord_t h,
                              lv_coord_t radius, lv_color_t bg_color, lv_opa_t bg_opa);

// Create a simple panel (transparent background, no border)
lv_obj_t* ui_create_panel(lv_obj_t* parent, lv_coord_t w, lv_coord_t h);

// Create a row container (flex flow row)
lv_obj_t* ui_create_row(lv_obj_t* parent, lv_coord_t gap);

// Create a column container (flex flow column)
lv_obj_t* ui_create_column(lv_obj_t* parent, lv_coord_t gap);

// =============================================================================
// BUTTONS
// =============================================================================

// Create an icon button (circular or square)
lv_obj_t* ui_create_icon_button(lv_obj_t* parent, const char* icon,
                                const lv_font_t* icon_font, lv_coord_t size,
                                lv_event_cb_t cb, void* user_data);

// Create a text button with label
lv_obj_t* ui_create_text_button(lv_obj_t* parent, const char* text,
                                lv_coord_t w, lv_coord_t h,
                                lv_event_cb_t cb, void* user_data);

// Create a primary (filled) button
lv_obj_t* ui_create_primary_button(lv_obj_t* parent, const char* text,
                                   lv_coord_t w, lv_coord_t h,
                                   lv_event_cb_t cb, void* user_data);

// Create a secondary (outlined) button
lv_obj_t* ui_create_secondary_button(lv_obj_t* parent, const char* text,
                                     lv_coord_t w, lv_coord_t h,
                                     lv_event_cb_t cb, void* user_data);

// =============================================================================
// LIST ITEMS (for settings, menus)
// =============================================================================

// Create a list item with icon, title, and optional subtitle
// Returns the row container; use user_data to pass item index
lv_obj_t* ui_create_list_item(lv_obj_t* parent, const char* icon,
                              const char* title, const char* subtitle,
                              lv_event_cb_t cb, void* user_data);

// Create a switch/toggle item
// Returns the switch object (use lv_obj_add_state/clear_state to toggle)
lv_obj_t* ui_create_switch_item(lv_obj_t* parent, const char* title,
                                bool initial_state, lv_event_cb_t cb, void* user_data);

// Create a slider item
// Returns the slider object
lv_obj_t* ui_create_slider_item(lv_obj_t* parent, const char* title,
                                int32_t min_val, int32_t max_val, int32_t value,
                                lv_event_cb_t cb, void* user_data);

// =============================================================================
// DIALOGS
// =============================================================================

// Create a modal overlay (semi-transparent background)
lv_obj_t* ui_create_modal(lv_obj_t* parent);

// Create a dialog with title, message, and up to 2 buttons
// btn1 is primary action (left), btn2 is secondary (right)
// Pass NULL for btn1_text to skip button
lv_obj_t* ui_create_dialog(lv_obj_t* parent, const char* title, const char* message,
                           const char* btn1_text, lv_event_cb_t btn1_cb,
                           const char* btn2_text, lv_event_cb_t btn2_cb);

// =============================================================================
// STYLING HELPERS
// =============================================================================

// Apply pill background style (dark rounded background)
void ui_style_pill_background(lv_obj_t* obj, lv_coord_t radius);

// Apply card style (elevated container)
void ui_style_card(lv_obj_t* obj);

// Format a timestamp (ms) to a human-readable relative time string (e.g. "5m ago")
void ui_format_time_ago(long long quake_time_ms, char* buffer, size_t buf_size);

#endif // UI_HELPERS_H

