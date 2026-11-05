#include "ui_helpers.h"
#include "theme.h"
#include "icons/icons.h"
#include "board_profile.h"
#include "hande_time.h"

// Custom fonts

LV_FONT_DECLARE(font_bruno_18);
LV_FONT_DECLARE(icon_material_24);

// =============================================================================
// LABELS
// =============================================================================

lv_obj_t* ui_create_label(lv_obj_t* parent, const char* text,
                          const lv_font_t* font, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

lv_obj_t* ui_create_label_aligned(lv_obj_t* parent, const char* text,
                                  const lv_font_t* font, lv_color_t color,
                                  lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs) {
    lv_obj_t* label = ui_create_label(parent, text, font, color);
    lv_obj_align(label, align, x_ofs, y_ofs);
    return label;
}

lv_obj_t* ui_create_icon_label(lv_obj_t* parent, const char* icon,
                               const lv_font_t* icon_font, lv_color_t color) {
    return ui_create_label(parent, icon, icon_font, color);
}

// =============================================================================
// CONTAINERS
// =============================================================================

lv_obj_t* ui_create_container(lv_obj_t* parent, lv_coord_t w, lv_coord_t h,
                              lv_coord_t radius, lv_color_t bg_color, lv_opa_t bg_opa) {
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, w, h);
    lv_obj_set_style_radius(cont, radius, 0);
    lv_obj_set_style_bg_color(cont, bg_color, 0);
    lv_obj_set_style_bg_opa(cont, bg_opa, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    return cont;
}

lv_obj_t* ui_create_panel(lv_obj_t* parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t* ui_create_row(lv_obj_t* parent, lv_coord_t gap) {
    lv_obj_t* row = ui_create_panel(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    return row;
}

lv_obj_t* ui_create_column(lv_obj_t* parent, lv_coord_t gap) {
    lv_obj_t* col = ui_create_panel(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, gap, 0);
    return col;
}

// =============================================================================
// BUTTONS
// =============================================================================

lv_obj_t* ui_create_icon_button(lv_obj_t* parent, const char* icon,
                                const lv_font_t* icon_font, lv_coord_t size,
                                lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);  // Circular
    lv_obj_set_style_bg_color(btn, theme_bg_elevated(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, theme_primary(), 0);
    lv_obj_set_style_bg_color(btn, theme_primary_dim(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, theme_border_default(), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t* icon_lbl = lv_label_create(btn);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, icon_font, 0);
    lv_obj_set_style_text_color(icon_lbl, theme_primary(), 0);
    lv_obj_center(icon_lbl);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    return btn;
}

lv_obj_t* ui_create_text_button(lv_obj_t* parent, const char* text,
                                lv_coord_t w, lv_coord_t h,
                                lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, theme_bg_elevated(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, theme_border_default(), 0);
    lv_obj_set_style_bg_color(btn, theme_primary_dim(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, theme_border_default(), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(btn, 0, 0);  // OLED-optimized: no shadow

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, theme_text_primary(), 0);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    return btn;
}

lv_obj_t* ui_create_primary_button(lv_obj_t* parent, const char* text,
                                   lv_coord_t w, lv_coord_t h,
                                   lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, theme_primary(), 0);
    lv_obj_set_style_bg_color(btn, theme_text_primary(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_black(), 0);
    lv_obj_set_style_text_color(btn, theme_text_disabled(), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);  // OLED-optimized: no shadow

    lv_obj_set_style_bg_color(btn, theme_bg_card(), LV_STATE_DISABLED);

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    return btn;
}

lv_obj_t* ui_create_secondary_button(lv_obj_t* parent, const char* text,
                                     lv_coord_t w, lv_coord_t h,
                                     lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, theme_bg_elevated(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, theme_border_default(), 0);
    lv_obj_set_style_bg_color(btn, theme_bg_elevated(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, theme_text_primary(), 0);
    lv_obj_set_style_text_color(btn, theme_text_disabled(), LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(btn, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(btn, theme_border_default(), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(btn, 0, 0);  // OLED-optimized: no shadow

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    return btn;
}

// =============================================================================
// LIST ITEMS
// =============================================================================

lv_obj_t* ui_create_list_item(lv_obj_t* parent, const char* icon,
                              const char* title, const char* subtitle,
                              lv_event_cb_t cb, void* user_data) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, theme_bg_elevated(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(row, theme_primary_dim(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, theme_bg_card(), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(row, theme_border_default(), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, theme_border_default(), 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (icon && strlen(icon) > 0) {
        lv_obj_t* icon_lbl = lv_label_create(row);
        lv_label_set_text(icon_lbl, icon);
        lv_obj_set_style_text_font(icon_lbl, &icon_material_24, 0);
        lv_obj_set_style_text_color(icon_lbl, theme_primary(), 0);
        lv_obj_set_style_pad_right(icon_lbl, 12, 0);
    }

    lv_obj_t* text_col = ui_create_column(row, 2);
    lv_obj_set_flex_grow(text_col, 1);

    lv_obj_t* title_lbl = lv_label_create(text_col);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title_lbl, theme_text_primary(), 0);

    if (subtitle && strlen(subtitle) > 0) {
        lv_obj_t* sub_lbl = lv_label_create(text_col);
        lv_label_set_text(sub_lbl, subtitle);
        lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(sub_lbl, theme_text_secondary(), 0);
    }

    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }

    return row;
}

lv_obj_t* ui_create_switch_item(lv_obj_t* parent, const char* title,
                                bool initial_state, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, theme_text_primary(), 0);

    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_size(sw, 50, 26);
    lv_obj_set_style_bg_color(sw, theme_bg_elevated(), 0);
    lv_obj_set_style_bg_color(sw, theme_primary(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, theme_primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, theme_primary_dim(), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(sw, 11);
    lv_obj_set_style_bg_color(sw, theme_bg_card(), LV_STATE_DISABLED);

    if (initial_state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    if (cb) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    }

    return sw;
}

lv_obj_t* ui_create_slider_item(lv_obj_t* parent, const char* title,
                                int32_t min_val, int32_t max_val, int32_t value,
                                lv_event_cb_t cb, void* user_data) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(col);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, theme_text_primary(), 0);
    lv_obj_set_style_pad_bottom(label, 8, 0);

    lv_obj_t* slider = lv_slider_create(col);
    lv_obj_set_width(slider, lv_pct(100));
    lv_obj_set_height(slider, 20);
    lv_slider_set_range(slider, min_val, max_val);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, theme_bg_elevated(), 0);
    lv_obj_set_style_bg_color(slider, theme_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, theme_primary(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, theme_text_primary(), LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(slider, theme_bg_card(), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(slider, theme_text_disabled(), LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(slider, 12);

    if (cb) {
        lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, user_data);
    }

    return slider;
}

// =============================================================================
// DIALOGS
// =============================================================================

lv_obj_t* ui_create_modal(lv_obj_t* parent) {
    lv_obj_t* modal = lv_obj_create(parent);
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_center(modal);
    return modal;
}

lv_obj_t* ui_create_dialog(lv_obj_t* parent, const char* title, const char* message,
                           const char* btn1_text, lv_event_cb_t btn1_cb,
                           const char* btn2_text, lv_event_cb_t btn2_cb) {
    // Create modal background
    lv_obj_t* modal = ui_create_modal(parent);

    // Create dialog card
    lv_obj_t* card = lv_obj_create(modal);
    lv_obj_set_size(card, 340, LV_SIZE_CONTENT);
    lv_obj_center(card);
    ui_style_card(card);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title
    if (title && strlen(title) > 0) {
        lv_obj_t* title_lbl = lv_label_create(card);
        lv_label_set_text(title_lbl, title);
        lv_obj_set_style_text_font(title_lbl, &font_bruno_18, 0);
        lv_obj_set_style_text_color(title_lbl, theme_text_primary(), 0);
        lv_obj_set_style_pad_bottom(title_lbl, 16, 0);
    }

    // Message
    if (message && strlen(message) > 0) {
        lv_obj_t* msg_lbl = lv_label_create(card);
        lv_label_set_text(msg_lbl, message);
        lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg_lbl, 300);
        lv_obj_set_style_text_font(msg_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(msg_lbl, theme_text_secondary(), 0);
        lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_bottom(msg_lbl, 24, 0);
    }

    // Buttons row
    lv_obj_t* btn_row = ui_create_row(card, 16);

    if (btn1_text && strlen(btn1_text) > 0) {
        lv_obj_t* btn1 = ui_create_primary_button(btn_row, btn1_text, 130, 44, btn1_cb, modal);
    }

    if (btn2_text && strlen(btn2_text) > 0) {
        lv_obj_t* btn2 = ui_create_secondary_button(btn_row, btn2_text, 130, 44, btn2_cb, modal);
    }

    return modal;
}

// =============================================================================
// STYLING HELPERS
// =============================================================================

void ui_style_pill_background(lv_obj_t* obj, lv_coord_t radius) {
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_60, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
}

void ui_style_card(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, theme_bg_card(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, theme_border_default(), 0);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_shadow_width(obj, 20, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_40, 0);
}

void ui_style_transparent_container(lv_obj_t* obj) {
    if (!obj) return;
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
}

void ui_format_time_ago(long long quake_time_ms, char* buffer, size_t buf_size) {
    if (!buffer || buf_size == 0) return;
    time_t now = time(nullptr);
    time_t quake_time = quake_time_ms / 1000;

    if (!hande_time_valid() || quake_time < 1577836800) {  // no clock yet, or no quake time
        snprintf(buffer, buf_size, "Time unknown");
        return;
    }

    long diff_sec = now - quake_time;
    if (diff_sec < 0) {
        snprintf(buffer, buf_size, "Just now");  // Future timestamp edge case
        return;
    }

    if (diff_sec < 60) snprintf(buffer, buf_size, "%lds ago", diff_sec);
    else if (diff_sec < 3600) snprintf(buffer, buf_size, "%ldm ago", diff_sec / 60);
    else if (diff_sec < 86400) snprintf(buffer, buf_size, "%ldh ago", diff_sec / 3600);
    else snprintf(buffer, buf_size, "%ldd ago", diff_sec / 86400);
}

