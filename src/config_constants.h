#pragma once

// GeoQuake Configuration Constants
// Extracted from scattered #defines to centralize configuration
// All constants are constexpr for type safety and compile-time evaluation

#include <cstdint>
#include <cstddef>
#include <cmath>

// ============================================================================
// TIMING CONSTANTS
// ============================================================================

// UI Timing
constexpr uint32_t DOCK_HIDE_DELAY_MS = 150;           // Button dock hide delay
constexpr uint32_t OVERLAY_THROTTLE_MS = 50;           // Throttle overlay invalidation during pan
constexpr uint32_t ZOOM_COOLDOWN_MS = 500;             // Minimum time between zoom level changes
constexpr uint32_t TOUCH_RELEASE_DEBOUNCE_MS = 30;     // Debounce time for touch release
constexpr uint32_t SETTINGS_IRQ_TIMEOUT_MS = 80;       // IRQ timeout for settings screen
constexpr uint32_t SETTINGS_DEBOUNCE_MS = 300;         // Settings UI debounce
constexpr uint32_t DOTS_SHOW_DELAY_MS = 200;           // Delay before showing earthquake dots

// WiFi Timing
constexpr uint32_t WIFI_RECONNECT_INITIAL_MS = 30000;  // Initial reconnect delay (30s)
constexpr uint32_t WIFI_RECONNECT_MAX_MS = 300000;     // Max reconnect delay (5min)

// System Timing
constexpr uint32_t REFRESH_INTERVAL_MS = 600000;       // Earthquake data refresh (10 minutes)
constexpr uint32_t APP_RESET_PRESS_MS = 1500;          // Button press for app reset
constexpr uint32_t FACTORY_RESET_PRESS_MS = 5000;      // Button press for factory reset
constexpr uint32_t BATTERY_FLUSH_INTERVAL_MS = 60000;  // Battery data flush interval

// ============================================================================
// DISTANCE & THRESHOLD CONSTANTS
// ============================================================================

// Touch/Pan Thresholds
constexpr float TAP_THRESHOLD_PX = 30.0f;              // Maximum distance for tap detection
constexpr float DOUBLE_TAP_TIME_MS = 300.0f;           // Maximum time between taps
constexpr float DOUBLE_TAP_DISTANCE_PX = 30.0f;        // Maximum distance between double taps
constexpr int16_t DELTA_CLAMP_THRESHOLD_PX = 200;      // Max delta before clamping (Anti-Pattern #16)
constexpr int16_t SCROLL_LIMIT_PX = 10;                // LVGL scroll limit (pixels before scrolling)
constexpr int16_t PAN_DIRECTION_DEAD_ZONE_PX = 5;      // Dead zone for direction detection

// Brightness Levels
constexpr uint8_t NIGHT_BRIGHTNESS = 30;               // Night mode brightness percentage

// ============================================================================
// ZOOM & MAP CONSTANTS
// ============================================================================

// Zoom Levels
constexpr float MAX_ZOOM_LEVEL = 32.0f;                // Maximum continuous zoom level
constexpr int8_t MAX_TILE_LEVEL = 6;                   // Highest tile zoom level available
constexpr float BASE_ZOOM = 2.0f;                      // Baseline zoom for z1 tiles
constexpr float ZOOM_THRESHOLD_RATIO = 1.4f;           // Zoom level transition threshold

// LOD (Level of Detail) Thresholds
constexpr float LOD_MIN_COUNTRY_NAMES = 4.0f;          // Show country names from 4x
constexpr float LOD_MAX_COUNTRY_NAMES = 14.0f;         // Hide country names at 14x
constexpr float LOD_SHOW_STATES = 14.0f;               // Show state borders from 14x
constexpr float LOD_SHOW_CITIES = 14.0f;               // Show city names from 14x

// Tile Dimensions
constexpr uint16_t TILE_SIZE = 256;                    // Tile size in pixels
constexpr size_t TILE_BUFFER_SIZE = 256 * 256 * 2;     // RGB565 buffer size (128KB)

// Screen dimensions are loaded at runtime from board_profile.h.

// ============================================================================
// MEMORY CONSTANTS (PSRAM - 8MB Total)
// ============================================================================

// Tile Cache (defined in tile_manager.h as #define TILE_CACHE_MAX 15)
// constexpr size_t TILE_CACHE_MAX = 15;               // DUPLICATE - DO NOT DEFINE HERE
constexpr size_t TILE_POOL_SIZE = 16;                  // Tile buffer pool size
constexpr size_t DOT_POOL_SIZE = 200;                  // Earthquake dot pool size

// Memory Thresholds (Anti-Pattern #5: Use largest_free_block)
constexpr size_t PSRAM_ALLOCATION_THRESHOLD = 4096;    // Threshold for PSRAM vs internal RAM
constexpr size_t PSRAM_LARGEST_BLOCK_MINIMUM = 768 * 1024;  // 768KB minimum for allocation
constexpr size_t FREE_PSRAM_WARNING = 2560 * 1024;     // 2.5MB warning threshold
constexpr size_t FREE_PSRAM_CRITICAL = 1536 * 1024;    // 1.5MB critical threshold

// Fragmentation Thresholds
constexpr float FRAGMENTATION_WARNING_PCT = 30.0f;     // Warning at 30% fragmentation
constexpr float FRAGMENTATION_CRITICAL_PCT = 50.0f;    // Critical at 50% fragmentation

// LVGL Buffer Configuration (40-line internal DMA buffer)
constexpr uint16_t LVGL_BUF_LINES = 40;                // LVGL buffer lines (matches display driver)

// ============================================================================
// STACK SIZES (Anti-Pattern #2: Task Stack Sizing)
// ============================================================================

constexpr uint32_t STACK_SIMPLE_TASK = 2048;           // Simple polling tasks
constexpr uint32_t STACK_MEDIUM_TASK = 4096;           // Tasks with some printf
constexpr uint32_t STACK_COMPLEX_TASK = 8192;          // Heavy logging tasks
constexpr uint32_t STACK_PARALLEL_TASK = 16384;        // Parallel decode tasks (Anti-Pattern #11)

// Other Stack Sizes
constexpr uint32_t UI_QUEUE_SIZE = 20;
constexpr uint32_t SERIAL_CMD_QUEUE_SIZE = 10;
constexpr uint32_t SERIAL_INPUT_BUFFER_SIZE = 256;
constexpr uint32_t WRITE_BUFFER_SIZE = 1024;

// ============================================================================
// LABEL & DISPLAY LIMITS
// ============================================================================

constexpr uint16_t MAX_CITY_LABELS = 200;              // Maximum city labels to render
constexpr uint16_t MAX_COUNTRY_LABELS = 100;           // Maximum country labels
constexpr uint32_t MIN_CITY_POPULATION = 300000;       // Minimum population for city label
constexpr size_t MAX_NETWORKS = 20;                    // Maximum WiFi networks to show
constexpr size_t UI_EVT_TEXT_MAX = 64;                 // Max UI event text length
constexpr size_t MAX_REGIONS = 10;                     // Maximum map regions

// ============================================================================
// SJPG FORMAT CONSTANTS
// ============================================================================

constexpr const char* SJPG_SIGNATURE = "_SJPG__";
constexpr size_t SJPG_HEADER_SIZE = 22;
constexpr size_t SJPG_X_RES_OFFSET = 14;
constexpr size_t SJPG_Y_RES_OFFSET = 16;
constexpr size_t SJPG_TOTAL_FRAMES_OFFSET = 18;
constexpr size_t SJPG_BLOCK_HEIGHT_OFFSET = 20;
constexpr size_t SJPG_FRAME_INFO_OFFSET = 22;
constexpr size_t TJPGD_WORK_SIZE = 4096;

// ============================================================================
// HARDWARE CONSTANTS
// ============================================================================

// SPI Frequencies
constexpr uint32_t SD_SPI_FREQUENCY = 40000000;        // 40MHz SD card SPI
// GPS_BAUD defined in config.h as #define GPS_BAUD 9600

// Battery Configuration
constexpr uint16_t BATTERY_CAPACITY_MAH = 2000;        // 2000mAh battery
constexpr uint8_t LOW_BATTERY_WARN_PCT = 10;           // Warning at 10%
constexpr uint8_t CRITICAL_BATTERY_PCT = 2;            // Shutdown at 2%
constexpr uint8_t LOW_BAT_SHUTDOWN_PCT = 1;            // Hardware shutdown at 1%
constexpr uint16_t BATTERY_FULL_MV = 4200;             // 100% voltage (4.2V)
constexpr uint16_t BATTERY_EMPTY_MV = 3200;            // 0% voltage (3.2V)

// Decode Tasks
constexpr uint8_t NUM_DECODE_TASKS = 2;                // Tested decode parallelism; 3 contends on SD mutex
constexpr uint8_t VISIBLE_QUEUE_SIZE = 16;             // Visible tile queue
constexpr uint8_t PREFETCH_QUEUE_SIZE = 48;            // Prefetch queue

// ============================================================================
// TYPE ALIASES
// ============================================================================

using ZoomLevel = float;
using TileZoomLevel = int8_t;
using PixelCoord = int16_t;
using GeoCoord = float;

// ============================================================================
// DEBUG & TELEMETRY LEVELS
// ============================================================================

// TILE_DEBUG_LEVEL and TILE_TELEMETRY_LEVEL defined in config.h (user-configurable)

// ============================================================================
// MAP DATA COUNTS (Auto-generated from map data files)
// ============================================================================

constexpr size_t COUNTRY_LABEL_COUNT = 258;
constexpr size_t CITY_LABEL_COUNT = 2572;
constexpr size_t MAP_STATES_COUNT = 4941;
constexpr size_t MAP_COASTLINES_COUNT = 725;
constexpr size_t MAP_COUNTRIES_COUNT = 804;

// Map Layer Indices: Defined in map_renderer.h (module-specific)
// MAP_LAYER_COASTLINES, MAP_LAYER_COUNTRIES, MAP_LAYER_STATES, MAP_LAYER_CITIES

// Theme Configuration
constexpr uint8_t THEME_PRESET_COUNT = 8;
