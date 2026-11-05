#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <functional>

#include "wifi_scan_policy.h"

#define WIFI_CONNECT_TIMEOUT 10

/**
 * WiFi Manager - Unified WiFi Lifecycle Management
 *
 * Provides consistent WiFi power management across all features:
 * - The app's own saved network credentials
 * - Manual refresh button
 * - Settings filter changes
 * - Background auto-refresh (already working)
 *
 * Design: Turn WiFi ON → Execute operation → Always turn OFF (even on errors)
 */

// Callback type for WiFi operations (returns true on success, false on failure)
typedef std::function<bool(void)> WiFiOperationCallback;

// Result codes for WiFi operations
enum WiFiOperationResult {
    WIFI_OP_SUCCESS,           // Operation completed successfully
    WIFI_OP_WIFI_FAILED,       // WiFi connection failed
    WIFI_OP_OPERATION_FAILED,  // Operation failed (HTTP error, etc.)
    WIFI_OP_TIMEOUT            // Operation timed out
};

/**
 * Execute an operation with WiFi power management
 *
 * Lifecycle:
 * 1. Log operation start
 * 2. wifi_power_on() with retry logic
 * 3. Execute callback
 * 4. 500ms delay (prevent transfer corruption)
 * 5. wifi_power_off() (ALWAYS, even on errors)
 * 6. Log operation end with timing
 *
 * @param operation_name Name for logging (e.g., "Manual Refresh", "Background Refresh")
 * @param callback Function to execute while WiFi is ON
 * @return Result code
 */
WiFiOperationResult wifi_managed_operation(const char* operation_name, WiFiOperationCallback callback);

void wifi_init();
void wifi_process();

bool wifi_is_connected();
bool wifi_is_powered_off();
int32_t wifi_get_rssi();
const char* wifi_get_ssid();
const char* wifi_get_ip_str();
void wifi_get_mac(uint8_t mac[6]);
String wifi_get_mac_address();

bool wifi_has_saved_credentials();
bool wifi_try_saved_credentials();
const char* wifi_get_saved_ssid();

// Stores a new network as the app's own station config (the driver keeps it in NVS, like the config the app has
// always used) and connects to it, waiting up to timeout_ms. Blocks the calling task; never call it from LVGL.
// The caller must be a task that may own the radio for that long (Improv over USB serial does).
bool wifi_connect_new_credentials(const char* ssid, const char* password, uint32_t timeout_ms);

// Erases the saved station config (same sequence as factory_reset.cpp, under the Wi-Fi API lock), disconnects and
// stops the radio. Background reconnect stops until the next power-on/user action.
bool wifi_forget_network();

// One nearby network for the Improv serial scan: up to 20, strongest first, one entry per SSID.
using WifiScanEntry = wifi_scan_policy::Network;

// Scans for nearby networks, blocking the calling task for a few seconds (never call it from LVGL). Owns the radio
// the app's way: brings it up in STA mode when it was off and powers it off again afterwards unless a fetch worker
// started meanwhile. A connected link and a running fetch are left alone (a scan never breaks a fetch).
// Writes up to min(capacity, 20) networks into `out` and returns the count, or -1 when the scan failed.
int32_t wifi_scan_networks(WifiScanEntry* out, size_t capacity);

void wifi_power_off();
bool wifi_power_on(bool is_background_refresh = false);

// Radio lease for work that outlives its caller (the quake fetch worker). While
// a lease is held, no power-off happens; the worker releases it after it stops
// using Wi-Fi. The waiting caller never takes or releases a lease.
void wifi_worker_lease_acquire();
void wifi_worker_lease_release();

// Recursive global WiFi API lock for cross-core operations. Recursive locking
// allows task-level operations to call the guarded power/connect entry points.
bool wifi_api_lock(uint32_t timeout_ms);
void wifi_api_unlock();

/**
 * Get last WiFi operation duration (milliseconds)
 * For battery impact tracking
 */
unsigned long wifi_get_last_operation_duration();

/**
 * Get total WiFi ON time since boot (milliseconds)
 * For battery profiling
 */
unsigned long wifi_get_total_on_time();

#endif // WIFI_MANAGER_H
