#include "battery.h"
#include "config.h"
#include <Wire.h>
#include <atomic>
#include <esp_sleep.h>
#include "XPowersLib.h"
#include "hande_pmu.h"
#include "i2c_mutex.h"

static XPowersPMU power;
static bool pmu_initialized = false;
static bool low_battery_warning_shown = false;

// USB state caching to prevent I2C hangs during power transitions. Read from both cores (HUD on Core 1, managed
// refresh on Core 0): atomics, and usb_read_once is published last (release) so a reader that sees it also sees the
// state and time it covers. The first call reads the PMU (the cache starts empty, not "valid").
static std::atomic<bool> cached_usb_connected{false};
static std::atomic<uint32_t> last_usb_read_ms{0};
static std::atomic<bool> usb_read_once{false};
static std::atomic<uint32_t> usb_transition_cooldown_until{0};
static const uint32_t USB_CACHE_VALIDITY_MS = 2000;      // Cache valid for 2 seconds
static const uint32_t USB_TRANSITION_COOLDOWN_MS = 500;  // After change, wait 500ms

// Battery percentage smoothing (EMA filter)
static int smoothed_percent = -1;           // -1 = not initialized
static uint32_t last_percent_update_ms = 0;
static const float EMA_ALPHA = 0.3f;        // Smoothing factor (0.3 = moderate smoothing)
static const uint32_t PERCENT_UPDATE_INTERVAL_MS = 1000;  // Update at most once per second

// Forward declaration for smoothing reset
static void battery_reset_smoothing();

// Battery configuration
#define BATTERY_CAPACITY_MAH    2000    // 2000mAh battery
#define LOW_BATTERY_WARN_PCT    10      // Show warning at 10%
#define CRITICAL_BATTERY_PCT    2       // Shutdown at 2%
#define LOW_BAT_SHUTDOWN_PCT    1       // Hardware shutdown threshold (1%)

// Voltage-based percentage calculation (more reliable than fuel gauge)
// LiPo discharge curve: 4.2V (full) to 3.0V (empty)
#define BATTERY_FULL_MV     4200    // 100%
#define BATTERY_EMPTY_MV    3200    // 0% (safe cutoff, actual empty is ~3.0V)

// LiPo voltage to percentage lookup table (voltage in mV -> percentage)
// Based on typical LiPo discharge curve (non-linear)
static int voltage_to_percent(int voltage_mv, bool is_usb, bool is_charging) {
    if (is_usb && !is_charging && voltage_mv >= 4120) return 100;
    if (voltage_mv >= 4180) return 100;
    if (voltage_mv <= BATTERY_EMPTY_MV) return 0;

    // LiPo discharge curve approximation (more accurate than linear)
    if (voltage_mv >= 4120) {
        // 4120-4180mV = 92-100%
        return 92 + (voltage_mv - 4120) * 8 / 60;
    } else if (voltage_mv >= 4000) {
        // 4000-4120mV = 75-92%
        return 75 + (voltage_mv - 4000) * 17 / 120;
    } else if (voltage_mv >= 3900) {
        // 3900-4000mV = 55-75%
        return 55 + (voltage_mv - 3900) * 20 / 100;
    } else if (voltage_mv >= 3800) {
        // 3800-3900mV = 35-55%
        return 35 + (voltage_mv - 3800) * 20 / 100;
    } else if (voltage_mv >= 3700) {
        // 3700-3800mV = 20-35%
        return 20 + (voltage_mv - 3700) * 15 / 100;
    } else if (voltage_mv >= 3600) {
        // 3600-3700mV = 10-20%
        return 10 + (voltage_mv - 3600) * 10 / 100;
    } else if (voltage_mv >= 3400) {
        // 3400-3600mV = 2-10%
        return 2 + (voltage_mv - 3400) * 8 / 200;
    } else {
        // 3200-3400mV = 0-2%
        return (voltage_mv - BATTERY_EMPTY_MV) * 2 / 200;
    }
}

bool battery_init() {
    // AXP2101 uses same I2C bus as touch (SDA=15, SCL=14)
    // Wire should already be initialized by touch driver

    bool result = power.begin(Wire, AXP2101_SLAVE_ADDRESS, TOUCH_SDA, TOUCH_SCL);

    if (!result) {
        Serial.println("[BATTERY] AXP2101 not found");
        return false;
    }

    Serial.println("[BATTERY] AXP2101 initialized");

    // PMU settings from hande_pmu.h: TS pin off, measurements and fuel gauge on, 4.2 V charge target.
    hande_pmu_apply_common(power);
    Serial.println("[BATTERY] Common PMU settings applied");

    // Set hardware low battery shutdown threshold (1% as safety net)
    power.setLowBatShutdownThreshold(LOW_BAT_SHUTDOWN_PCT);
    Serial.printf("[BATTERY] Hardware shutdown threshold: %d%%\n", LOW_BAT_SHUTDOWN_PCT);

    // Log battery capacity (for reference - AXP2101 doesn't have capacity register)
    Serial.printf("[BATTERY] Configured for %d mAh battery\n", BATTERY_CAPACITY_MAH);

    pmu_initialized = true;
    return true;
}

int battery_get_percent() {
    if (!pmu_initialized) return -1;

    // Rate limit: only read hardware once per second
    uint32_t now = millis();
    if (smoothed_percent >= 0 && (now - last_percent_update_ms) < PERCENT_UPDATE_INTERVAL_MS) {
        return smoothed_percent;  // Return cached smoothed value
    }

    // I2C mutex protection - battery shares bus with touch (CST9217)
    if (!i2c_mutex_lock(50)) {
        return smoothed_percent >= 0 ? smoothed_percent : -1;
    }

    if (!power.isBatteryConnect()) {
        i2c_mutex_unlock();
        return -1;
    }

    // Use voltage-based calculation with charge-state calibration
    int voltage_mv = power.getBattVoltage();
    bool is_usb = power.isVbusIn();
    bool is_charging = power.isCharging();
    int raw_percent = voltage_to_percent(voltage_mv, is_usb, is_charging);

    i2c_mutex_unlock();

    // Apply EMA smoothing
    if (smoothed_percent < 0) {
        // First reading - initialize directly
        smoothed_percent = raw_percent;
    } else {
        // EMA: new_value = alpha * raw + (1 - alpha) * old
        smoothed_percent = (int)(EMA_ALPHA * raw_percent + (1.0f - EMA_ALPHA) * smoothed_percent);
    }

    last_percent_update_ms = now;

    // Debug: log raw vs smoothed (every 30 seconds)
    static uint32_t last_debug_log = 0;
    if (now - last_debug_log > 30000) {
        Serial.printf("[BATTERY] Voltage: %dmV, Raw: %d%%, Smoothed: %d%%\n",
                      voltage_mv, raw_percent, smoothed_percent);
        last_debug_log = now;
    }

    return smoothed_percent;
}

int battery_get_voltage() {
    if (!pmu_initialized) return 0;

    // I2C mutex protection - battery shares bus with touch (CST9217)
    int result = 0;
    if (i2c_mutex_lock(50)) {
        result = power.getBattVoltage();
        i2c_mutex_unlock();
    }
    return result;
}

bool battery_is_connected() {
    if (!pmu_initialized) return false;

    // I2C mutex protection - battery shares bus with touch (CST9217)
    bool result = false;
    if (i2c_mutex_lock(50)) {
        result = power.isBatteryConnect();
        i2c_mutex_unlock();
    }
    return result;
}

bool battery_is_charging() {
    if (!pmu_initialized) return false;

    // I2C mutex protection - battery shares bus with touch (CST9217)
    bool result = false;
    if (i2c_mutex_lock(50)) {
        result = power.isCharging();
        i2c_mutex_unlock();
    }
    return result;
}

// Reset battery smoothing (call when USB state changes)
static void battery_reset_smoothing() {
    smoothed_percent = -1;  // Will re-initialize on next read
    Serial.println("[BATTERY] Smoothing reset due to USB state change");
}

bool battery_is_usb_connected() {
    if (!pmu_initialized) return false;

    uint32_t now = millis();

    // During transition cooldown, return cached value (no I2C)
    // This prevents blocking reads during unstable USB power transitions (~100ms)
    if (now < usb_transition_cooldown_until) {
        return cached_usb_connected;
    }

    // If cache still valid, return cached value
    if (usb_read_once.load(std::memory_order_acquire) && (now - last_usb_read_ms.load()) < USB_CACHE_VALIDITY_MS) {
        return cached_usb_connected.load();
    }

    // Try to read from PMU with timeout
    // I2C mutex protection - battery shares bus with touch (CST9217)
    if (!i2c_mutex_lock(30)) {
        return cached_usb_connected;  // Mutex timeout, use cache
    }

    bool new_state = power.isVbusIn();
    i2c_mutex_unlock();

    // Detect state change and start cooldown
    const bool old_state = cached_usb_connected.load();
    if (new_state != old_state) {
        Serial.printf("[BATTERY] USB: %s -> %s\n",
                     old_state ? "connected" : "disconnected",
                     new_state ? "connected" : "disconnected");
        usb_transition_cooldown_until = now + USB_TRANSITION_COOLDOWN_MS;
        // Reset smoothing for faster convergence to new voltage level
        battery_reset_smoothing();
    }

    cached_usb_connected.store(new_state);
    last_usb_read_ms.store(now);
    usb_read_once.store(true, std::memory_order_release);
    return new_state;
}

// Shutdown the device via AXP2101
void battery_shutdown() {
    if (!pmu_initialized) {
        // Fallback: ESP32 deep sleep
        esp_deep_sleep_start();
        return;
    }

    Serial.println("[BATTERY] Shutting down via AXP2101...");
    delay(100);  // Allow serial to flush

    // I2C mutex protection - battery shares bus with touch (CST9217)
    if (i2c_mutex_lock(100)) {
        power.shutdown();
        i2c_mutex_unlock();
    }

    // Fallback if shutdown() doesn't work
    delay(1000);
    esp_deep_sleep_start();
}

// Check battery health and return status
// Returns: 0=OK, 1=LOW_WARNING, 2=CRITICAL
int battery_check_health() {
    if (!pmu_initialized) return 0;

    // I2C mutex protection - battery shares bus with touch (CST9217)
    bool battery_connected = false;
    bool is_charging = false;
    bool has_usb_power = false;
    if (i2c_mutex_lock(50)) {
        battery_connected = power.isBatteryConnect();
        if (battery_connected) {
            is_charging = power.isCharging();
            has_usb_power = power.isVbusIn();
        }
        i2c_mutex_unlock();
    }

    if (!battery_connected) return 0;

    int percent = battery_get_percent();  // Use voltage-based calculation (has its own mutex)

    // External power is safe: VBUS can be valid before the PMU reports charging,
    // so a just-connected cable must already prevent a critical-battery shutdown.
    if (is_charging || has_usb_power) {
        low_battery_warning_shown = false;
        return 0;
    }

    // Critical level - needs shutdown
    if (percent <= CRITICAL_BATTERY_PCT) {
        Serial.printf("[BATTERY] CRITICAL: %d%% - shutdown required!\n", percent);
        return 2;
    }

    // Low battery warning
    if (percent <= LOW_BATTERY_WARN_PCT) {
        if (!low_battery_warning_shown) {
            Serial.printf("[BATTERY] WARNING: Low battery %d%%\n", percent);
            low_battery_warning_shown = true;
        }
        return 1;
    }

    // Battery OK
    low_battery_warning_shown = false;
    return 0;
}

// Check if in low power warning state
bool battery_is_low() {
    if (!pmu_initialized) return false;
    int percent = battery_get_percent();
    return (percent >= 0 && percent <= LOW_BATTERY_WARN_PCT && !battery_is_charging());
}
