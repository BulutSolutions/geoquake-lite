#pragma once

#include <lvgl.h>
#include <stdint.h>

/**
 * @brief Displays the AMOLED-optimized GeoQuake splash screen on lv_layer_top().
 * Follows pure-black (0x000000) canvas, Cyan/Amber seismic epicenter radar emblem,
 * high-contrast Bruno Ace typography, and live loading progress indicator.
 */
void splash_screen_show();

/**
 * @brief Updates the splash screen status label and progress bar.
 * @param status Status message to display below the progress bar.
 * @param progress Value between 0 and 100 (-1 to leave unchanged).
 */
void splash_screen_update(const char* status, int progress = -1);

/**
 * @brief Marks the background boot and quake data sync as ready, arming tap dismissal.
 */
void splash_screen_set_ready();

/**
 * @brief Drives the living cyclic animation loop and checks for tap dismissal.
 * Called each frame from ui_tick() on Core 1 when splash is active.
 */
void splash_screen_tick();

/**
 * @brief Dismisses the splash screen immediately (< 15ms) by deleting the root object.
 */
void splash_screen_dismiss();

/**
 * @brief Finishes loading and loops the living seismic emblem until the user taps to dismiss.
 * (Synchronous helper for backward compatibility or blocking setups).
 * @param wait_for_tap If true, holds indefinitely with cyclic living animation until user taps.
 *                     If false, holds for hold_ms then hands off.
 * @param hold_ms Minimum duration in milliseconds if wait_for_tap is false.
 */
void splash_screen_finish(bool wait_for_tap = true, uint32_t hold_ms = 600);

/**
 * @brief Returns true while the splash screen is visible on screen.
 */
bool splash_screen_is_active();
