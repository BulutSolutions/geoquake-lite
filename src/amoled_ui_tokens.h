#pragma once

// Phase 0 AMOLED readability tokens. Values are RGB hex values for direct LVGL use.

// Core dark-theme palette.
#define AMOLED_BACKGROUND_HEX 0x000000
#define AMOLED_SURFACE_CARD_FLOOR_HEX 0x0A0A0A
#define AMOLED_BORDER_HEX 0x333333
#define APP_ACCENT_HEX 0xFF8A3D

// Readable text colors: never use a lower-contrast color for body copy.
#define AMOLED_PRIMARY_TEXT_HEX 0xFFFFFF
#define AMOLED_SECONDARY_TEXT_HEX 0xC4C4C4
#define AMOLED_MUTED_DISABLED_READABLE_FLOOR_HEX 0xA0A0A0

// Disabled controls use these explicit colors; never dim an entire widget with opacity.
#define AMOLED_DISABLED_TEXT_HEX 0xA0A0A0
#define AMOLED_DISABLED_SURFACE_HEX 0x0A0A0A
#define AMOLED_DISABLED_BORDER_HEX 0x333333

// Shared semantic status colors.
#define AMOLED_SUCCESS_HEX 0x4ADE80
#define AMOLED_WARNING_HEX 0xFACC15
#define AMOLED_ERROR_HEX 0xFF5C5C
#define AMOLED_INFO_HEX 0x60A5FA

// Interaction sizing floors in pixels.
#define AMOLED_MIN_TOUCH_TARGET_PX 44
#define AMOLED_PREFERRED_TOUCH_TARGET_PX 48

// Readable typography floors in pixels.
#define AMOLED_BODY_SECONDARY_FONT_FLOOR_PX 16
#define AMOLED_CAPTION_FONT_FLOOR_PX 14

// Pressed controls retain contrast against the black background.
#define AMOLED_PRESSED_SURFACE_HEX 0x1A1A1A

// Error text must wrap and must never be truncated.
#define AMOLED_ERROR_TEXT_MUST_WRAP 1
#define AMOLED_ERROR_TEXT_MUST_NOT_TRUNCATE 1

// Montserrat is for readable text. These display families are limited to titles or clock/display UI.
#define AMOLED_READABLE_TEXT_FONT_FAMILY "Montserrat"
#define AMOLED_TITLE_FONT_FAMILY "Bruno Ace SC"
#define AMOLED_TITLE_ALTERNATE_FONT_FAMILY "Orbitron"
#define AMOLED_CLOCK_DISPLAY_FONT_FAMILY "DSEG7"
