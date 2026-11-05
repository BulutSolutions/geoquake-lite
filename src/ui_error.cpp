#include "ui_error.h"
#include "ui_display.h"
#include "ui_state.h"
#include "ui_sync.h"
#include "theme.h"
#include <Arduino.h>

/**
 * Error Screen Implementation
 *
 * Displays professional error messages with retry/cancel actions.
 * Automatically manages state transitions and cleanup.
 */

// Error context structure
typedef struct {
    ErrorType type;
    char message[256];
    bool can_retry;
    lv_obj_t *screen;
    lv_obj_t *btn_retry;
    lv_obj_t *btn_cancel;
    UiState previous_state;
    bool visible;
} ErrorContext;

static ErrorContext error_ctx = {ERROR_UNKNOWN, "", false, NULL, NULL, NULL, UI_STATE_MAIN, false};

// Forward declarations
static void retry_cb(lv_event_t *e);
static void cancel_cb(lv_event_t *e);
static const char* get_error_title(ErrorType type);
static const char* get_error_icon(ErrorType type);

// ===== ERROR SCREEN CREATION =====

void ui_error_show(ErrorType type, const char *message, bool can_retry) {
    if (!message) {
        Serial.println("[ERROR] ui_error_show: message is NULL");
        return;
    }

    Serial.printf("[ERROR] Showing error screen: type=%d, retry=%d, msg=%s\n", type, can_retry, message);

    // Store error context
    error_ctx.type = type;
    strncpy(error_ctx.message, message, sizeof(error_ctx.message) - 1);
    error_ctx.message[sizeof(error_ctx.message) - 1] = '\0';  // Ensure null termination
    error_ctx.can_retry = can_retry;
    error_ctx.previous_state = ui_get_state();
    error_ctx.visible = true;

    // Get parent screen (ui_ScreenMain should always exist)
    lv_obj_t *parent = lv_screen_active();

    // Create error screen overlay (centered modal)
    error_ctx.screen = lv_obj_create(parent);
    lv_obj_set_size(error_ctx.screen, 400, 380);
    lv_obj_center(error_ctx.screen);
    lv_obj_set_style_bg_color(error_ctx.screen, theme_bg_card(), 0);
    lv_obj_set_style_bg_opa(error_ctx.screen, LV_OPA_90, 0);
    lv_obj_set_style_border_width(error_ctx.screen, 2, 0);
    lv_obj_set_style_border_color(error_ctx.screen, theme_error(), 0);
    lv_obj_set_style_radius(error_ctx.screen, 16, 0);
    lv_obj_set_style_shadow_width(error_ctx.screen, 20, 0);
    lv_obj_set_style_shadow_color(error_ctx.screen, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(error_ctx.screen, LV_OPA_50, 0);

    // Error icon (top center)
    lv_obj_t *icon = lv_label_create(error_ctx.screen);
    lv_label_set_text(icon, get_error_icon(type));
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, theme_error(), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 30);

    // Error title
    lv_obj_t *title = lv_label_create(error_ctx.screen);
    lv_label_set_text(title, get_error_title(type));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, theme_text_primary(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 110);

    // Error message lives in a bounded, vertically scrollable region so long
    // diagnostics remain accessible without colliding with the action buttons.
    lv_obj_t *msg_box = lv_obj_create(error_ctx.screen);
    lv_obj_set_size(msg_box, 360, 120);
    lv_obj_align(msg_box, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_opa(msg_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msg_box, 0, 0);
    lv_obj_set_style_pad_all(msg_box, 0, 0);
    lv_obj_set_scroll_dir(msg_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(msg_box, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *msg = lv_label_create(msg_box);
    lv_label_set_text(msg, error_ctx.message);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, 350);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(msg, theme_text_secondary(), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 0);

    // Buttons (bottom)
    if (can_retry) {
        // Retry button (left)
        error_ctx.btn_retry = lv_button_create(error_ctx.screen);
        lv_obj_set_size(error_ctx.btn_retry, 160, 50);
        lv_obj_align(error_ctx.btn_retry, LV_ALIGN_BOTTOM_LEFT, 20, -20);
        lv_obj_set_style_bg_color(error_ctx.btn_retry, theme_primary(), 0);
        lv_obj_set_style_bg_opa(error_ctx.btn_retry, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(error_ctx.btn_retry, theme_bg_card(), LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(error_ctx.btn_retry, theme_primary_dim(), LV_STATE_PRESSED);
        lv_obj_set_style_radius(error_ctx.btn_retry, 8, 0);
        lv_obj_set_style_shadow_width(error_ctx.btn_retry, 0, 0);  // OLED-optimized: no shadow
        lv_obj_add_event_cb(error_ctx.btn_retry, retry_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *retry_label = lv_label_create(error_ctx.btn_retry);
        lv_label_set_text(retry_label, "Retry");
        lv_obj_set_style_text_font(retry_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(retry_label, lv_color_white(), 0);
        lv_obj_center(retry_label);

        // Cancel button (right)
        error_ctx.btn_cancel = lv_button_create(error_ctx.screen);
        lv_obj_set_size(error_ctx.btn_cancel, 160, 50);
        lv_obj_align(error_ctx.btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
        lv_obj_set_style_bg_color(error_ctx.btn_cancel, theme_bg_elevated(), 0);
        lv_obj_set_style_bg_opa(error_ctx.btn_cancel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(error_ctx.btn_cancel, 2, 0);
        lv_obj_set_style_border_color(error_ctx.btn_cancel, theme_border_default(), 0);
        lv_obj_set_style_radius(error_ctx.btn_cancel, 8, 0);
        lv_obj_set_style_shadow_width(error_ctx.btn_cancel, 0, 0);  // OLED-optimized: no shadow
        lv_obj_set_style_bg_color(error_ctx.btn_cancel, theme_bg_card(), LV_STATE_DISABLED);
        lv_obj_set_style_border_color(error_ctx.btn_cancel, theme_border_default(), LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(error_ctx.btn_cancel, theme_primary_dim(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(error_ctx.btn_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *cancel_label = lv_label_create(error_ctx.btn_cancel);
        lv_label_set_text(cancel_label, "Cancel");
        lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(cancel_label, theme_text_primary(), 0);
        lv_obj_center(cancel_label);
    } else {
        // Only Cancel button (centered)
        error_ctx.btn_cancel = lv_button_create(error_ctx.screen);
        lv_obj_set_size(error_ctx.btn_cancel, 200, 50);
        lv_obj_align(error_ctx.btn_cancel, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_set_style_bg_color(error_ctx.btn_cancel, theme_primary(), 0);
        lv_obj_set_style_bg_opa(error_ctx.btn_cancel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(error_ctx.btn_cancel, 8, 0);
        lv_obj_set_style_shadow_width(error_ctx.btn_cancel, 0, 0);  // OLED-optimized: no shadow
        lv_obj_set_style_bg_color(error_ctx.btn_cancel, theme_bg_card(), LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(error_ctx.btn_cancel, theme_primary_dim(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(error_ctx.btn_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *cancel_label = lv_label_create(error_ctx.btn_cancel);
        lv_label_set_text(cancel_label, "OK");
        lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
        lv_obj_center(cancel_label);
    }

    Serial.println("[ERROR] Error screen created successfully");
}

// ===== ERROR SCREEN DESTRUCTION =====

void ui_error_hide() {
    if (!error_ctx.visible) {
        return;
    }

    Serial.println("[ERROR] Hiding error screen");

    if (error_ctx.screen) {
        lv_obj_delete(error_ctx.screen);
        error_ctx.screen = NULL;
    }

    error_ctx.btn_retry = NULL;
    error_ctx.btn_cancel = NULL;
    error_ctx.visible = false;

    Serial.println("[ERROR] Error screen destroyed");
}

// ===== STATE QUERY =====

bool ui_error_is_visible() {
    return error_ctx.visible;
}

// ===== EVENT HANDLERS =====

static void retry_cb(lv_event_t *e) {
    Serial.println("[ERROR] Retry button clicked");

    // Hide error screen
    ui_error_hide();

    // Retry logic based on error type
    switch (error_ctx.type) {
        case ERROR_NETWORK:
            // Retry network operation - return to previous state
            Serial.println("[ERROR] Retrying network operation");
            ui_queue_set_state(error_ctx.previous_state);
            break;

        case ERROR_MANIFEST:
            // Retry manifest fetch
            Serial.println("[ERROR] Retrying manifest fetch");
            ui_queue_set_state(UI_STATE_FETCH_MANIFEST);
            break;

        case ERROR_DATA_LOAD:
        case ERROR_STORAGE:
        case ERROR_UNKNOWN:
            // Return to main screen
            Serial.println("[ERROR] Returning to main screen");
            ui_queue_set_state(UI_STATE_MAIN);
            break;
    }
}

static void cancel_cb(lv_event_t *e) {
    Serial.println("[ERROR] Cancel/OK button clicked");

    // Hide error screen and return to main
    ui_error_hide();
    ui_queue_set_state(UI_STATE_MAIN);
}

// ===== HELPER FUNCTIONS =====

static const char* get_error_title(ErrorType type) {
    switch (type) {
        case ERROR_NETWORK:
            return "Network Error";
        case ERROR_DATA_LOAD:
            return "Data Load Failed";
        case ERROR_STORAGE:
            return "Storage Error";
        case ERROR_MANIFEST:
            return "Manifest Error";
        case ERROR_UNKNOWN:
        default:
            return "Error";
    }
}

static const char* get_error_icon(ErrorType type) {
    switch (type) {
        case ERROR_NETWORK:
            return LV_SYMBOL_WIFI;
        case ERROR_STORAGE:
            return LV_SYMBOL_SD_CARD;
        case ERROR_DATA_LOAD:
        case ERROR_MANIFEST:
        case ERROR_UNKNOWN:
        default:
            return LV_SYMBOL_WARNING;
    }
}
