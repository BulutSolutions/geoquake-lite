#include "board_profile.h"

#include <Arduino.h>

// GeoQuake Lite targets one board: the Waveshare ESP32-S3-Touch-AMOLED-2.16 (480x480, square).
BoardProfile g_board = {480, 0, false, false, false, "square216"};

void board_profile_load() {
    Serial.printf("[BOARD] Fixed profile: %s (%dx%d, col_offset=%d)\n", g_board.name,
                  g_board.screen_size, g_board.screen_size, g_board.col_offset);
}