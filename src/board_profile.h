#pragma once

// The one board this firmware targets: the Waveshare ESP32-S3-Touch-AMOLED-2.16.
struct BoardProfile {
    int screen_size;
    int col_offset;
    bool touch_mirror_x;
    bool touch_mirror_y;
    bool is_round;
    const char* name;
};

extern BoardProfile g_board;

// Fills g_board with the fixed square216 profile. No NVS, no detection.
void board_profile_load();