#pragma once

// =============================================================================
// FIRMWARE VERSION
// =============================================================================
// Increment these values when releasing new firmware.
// =============================================================================

#define FIRMWARE_VERSION_MAJOR 1
#define FIRMWARE_VERSION_MINOR 0
#define FIRMWARE_VERSION_PATCH 1

// Version string for display (e.g., "1.0.0")
#define FIRMWARE_VERSION_STRING "1.0.1"

// Build number - increment with each build (optional, for tracking)
#define FIRMWARE_BUILD_NUMBER 1

// Version as integer for easy comparison (1.0.0 = 10000, 1.2.3 = 10203)
#define FIRMWARE_VERSION_INT ((FIRMWARE_VERSION_MAJOR * 10000) + (FIRMWARE_VERSION_MINOR * 100) + FIRMWARE_VERSION_PATCH)

// App identity and version text used by the UI.
#define HANDE_APP_NAME "GeoQuake Lite"
#define HANDE_APP_VERSION_STRING FIRMWARE_VERSION_STRING
#define HANDE_APP_BUILD_NUMBER FIRMWARE_BUILD_NUMBER

