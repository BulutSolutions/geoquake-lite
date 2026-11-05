// Why the orientation is saved in NVS ("gql_ui"/"orient"): orientation is restored before the first draw after a
// reboot, so a settled position would otherwise be lost. A settled IMU commit writes the key back only when the value
// changes (never just because boot restored it).
#include "hande_orientation.h"

#include "hande_display.h"
#include "hande_orientation_policy.h"
#include "i2c_mutex.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

namespace op = hande_orientation_policy;

namespace {

constexpr int kPinSda = 15;
constexpr int kPinScl = 14;
constexpr uint32_t kPollIntervalMs = 50;

HandeOrientationConfig s_cfg;
SensorQMI8658 s_imu;
volatile bool s_ready = false;
volatile uint8_t s_current = 0;
op::Filter s_filter;
uint32_t s_last_poll_ms = 0;
uint8_t s_saved = 0xFF;  // last value written or restored (0xFF = unknown)
bool s_save_failed_logged = false;

}  // namespace

bool hande_orientation_init(const HandeOrientationConfig& config) {
    s_cfg = config;
    if (!i2c_mutex_lock(config.init_lock_timeout_ms)) {
        Serial.println("[ORIENTATION] I2C busy during IMU init: rotation off");
        return false;
    }
    bool ok = s_imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, kPinSda, kPinScl);
    if (!ok) ok = s_imu.begin(Wire, QMI8658_H_SLAVE_ADDRESS, kPinSda, kPinScl);
    if (ok) {
        s_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz);
        s_imu.enableAccelerometer();
    }
    i2c_mutex_unlock();
    s_ready = ok;
    Serial.println(ok ? "[ORIENTATION] QMI8658 ready" : "[ORIENTATION] QMI8658 not found: rotation off");
    return ok;
}

uint8_t hande_orientation_restore_saved() {
    uint8_t o = 0;
    bool saved = false;
    Preferences prefs;
    if (prefs.begin("gql_ui", true)) {
        const uint8_t raw = prefs.getUChar("orient", 0xFF);
        prefs.end();
        if (raw <= 3) {
            o = raw;
            saved = true;
        }
    }
    s_current = o;
    op::seed(s_filter, o);
    s_saved = o;
    if (hande_display_get() != nullptr) hande_display_set_orientation(o);
    Serial.printf("[ORIENTATION] restored %u degrees (%s)\n", static_cast<unsigned>(o) * 90U, saved ? "saved" : "default");
    return o;
}

void hande_orientation_tick() {
    if (!s_ready) return;
    if (s_cfg.skip_tick && s_cfg.skip_tick()) return;
    const uint32_t now = millis();
    if (now - s_last_poll_ms < kPollIntervalMs) return;
    s_last_poll_ms = now;

    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!i2c_mutex_lock(s_cfg.lock_timeout_ms)) return;
    const bool read_ok = s_imu.getAccelerometer(x, y, z);
    i2c_mutex_unlock();
    if (!read_ok || !op::filter_accel(s_filter, x, y, z)) return;

    if (s_cfg.on_reading) s_cfg.on_reading(x, y, z, s_filter.ax, s_filter.ay, s_filter.az);

    const bool vetoed = s_cfg.veto && s_cfg.veto();
    const op::Decision decision = op::decide(s_filter, now, vetoed);
    if (decision == op::kNone) return;

    if (decision == op::kFlatReset) {
        s_current = 0;
        hande_display_set_orientation(0);
        Serial.println("[ORIENTATION] lying flat: back to 0 degrees (saved value kept)");
        if (s_cfg.on_commit) s_cfg.on_commit(0);
        return;
    }

    s_current = s_filter.current;
    hande_display_set_orientation(s_filter.current);
    Serial.printf("[ORIENTATION] committed %u degrees\n", static_cast<unsigned>(s_filter.current) * 90U);
    if (s_cfg.on_commit) s_cfg.on_commit(s_filter.current);

    if (s_filter.current != s_saved) {
        Preferences prefs;
        if (prefs.begin("gql_ui", false)) {
            const size_t written = prefs.putUChar("orient", s_filter.current);
            prefs.end();
            if (written == 1) {
                s_saved = s_filter.current;
            } else if (!s_save_failed_logged) {
                s_save_failed_logged = true;
                Serial.println("[ORIENTATION] could not save the orientation to NVS");
            }
        } else if (!s_save_failed_logged) {
            s_save_failed_logged = true;
            Serial.println("[ORIENTATION] could not save the orientation to NVS");
        }
    }
}

bool hande_orientation_available() { return s_ready; }

uint8_t hande_orientation_current() { return s_current; }
