#ifndef UI_ERROR_H
#define UI_ERROR_H

#include <Arduino.h>

/**
 * Error Screen System
 *
 * Provides professional error handling UI with:
 * - Error type categorization
 * - User-friendly messages
 * - Retry/cancel actions
 * - Automatic state restoration
 */

// Error types for different failure scenarios
typedef enum {
    ERROR_NETWORK,       // WiFi or HTTP connection failures
    ERROR_DATA_LOAD,     // Earthquake data parsing failures
    ERROR_STORAGE,       // SD card or file system errors
    ERROR_MANIFEST,      // Region manifest loading failures
    ERROR_UNKNOWN        // Generic/unexpected errors
} ErrorType;

/**
 * Show error screen with message and actions
 *
 * @param type Error category (determines icon and title)
 * @param message Detailed error message to display
 * @param can_retry If true, shows Retry button; if false, only Cancel
 */
void ui_error_show(ErrorType type, const char *message, bool can_retry);

/**
 * Hide error screen and clean up resources
 */
void ui_error_hide();

/**
 * Check if error screen is currently visible
 *
 * @return true if error screen is showing
 */
bool ui_error_is_visible();

#endif // UI_ERROR_H
