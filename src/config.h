#pragma once

// --- Waveshare ESP32-S3-Touch-AMOLED-1.75 CONFIG ---

// Power Management (AXP2101)
#define XPOWERS_CHIP_AXP2101

// 1. Display Pins (CO5300 Driver / QSPI)
#define TFT_CS    12
#define TFT_SCK   38
#define TFT_D0    4
#define TFT_D1    5
#define TFT_D2    6
#define TFT_D3    7
#define TFT_RES   39

// 2. Touch Pins (CST9217) - Waveshare 1.75" AMOLED
#define TOUCH_SDA 15
#define TOUCH_SCL 14
#define TOUCH_INT 11
#define TOUCH_RST 40

// 3. SD Card Pins (SPI mode)
#define SD_MOSI  1
#define SD_SCK   2
#define SD_MISO  3
#define SD_CS    41

// 4. GPS Module Pins (LC76GABMD)
#define GPS_RX    18
#define GPS_TX    17
#define GPS_BAUD  9600

// 5. Audio Pins (ES8311 Codec via I2S)
#define I2S_MCLK  42
#define I2S_SCLK  9
#define I2S_LRCK  45
// GPIO8 = I2S_DSDIN (ES8311 playback data input) = DOUT from ESP32's perspective
// GPIO10 = I2S_ASDOUT (ES7210 mic data output) = DIN from ESP32's perspective
// Confirmed against official Waveshare GPIO pin table -- previously swapped, causing total silence.
#define I2S_DOUT  8
#define I2S_DIN   10
#define PA_CTRL   46

// 6. Tile Loading Debug Level
// 0 = off (production), 1 = errors only, 2 = verbose
#define TILE_DEBUG_LEVEL 0

// 7. Tile Telemetry Level (diagnostic logging for tile loading issues)
// 0 = off (production)
// 1 = per-frame summary + decode timing
// 2 = verbose (cache dumps, grid details, queue flow)
#define TILE_TELEMETRY_LEVEL 1

// 8. Install page (ESP Web Tools: flash + Improv Wi-Fi over USB serial)
// Placeholder until the public address exists; shown on the Wi-Fi help screen.
#define INSTALL_PAGE_URL "https://bulutsolutions.github.io/geoquake-lite/"
