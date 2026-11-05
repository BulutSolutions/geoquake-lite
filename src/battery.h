#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>

// Initialize battery monitoring (call after Wire.begin)
bool battery_init();

// Get battery percentage (0-100, or -1 if no battery)
int battery_get_percent();

// Get battery voltage in mV
int battery_get_voltage();

// Check if battery is connected
bool battery_is_connected();

// Check if charging
bool battery_is_charging();

// Check if USB power connected
bool battery_is_usb_connected();

// --- Power Management ---

// Check battery health and return status
// Returns: 0=OK, 1=LOW_WARNING (10%), 2=CRITICAL (2%)
int battery_check_health();

// Check if battery is in low warning state
bool battery_is_low();

// Shutdown device via AXP2101 (call for critical battery)
void battery_shutdown();

#endif // BATTERY_H
