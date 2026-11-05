#pragma once

// RTC: the board's PCF85063 holds UTC (hande_rtc_policy.h). All clock access goes through here.
// Uses the shared I2C lock (i2c_mutex); call from any task.

#include <stdint.h>

enum class HandeRtcStatus : uint8_t {
    kOk,
    kBusy,          // the I2C lock was not free within the timeout
    kBusError,      // the chip did not answer
    kClockStopped,  // the oscillator stopped (battery out): no valid time until it is set again
    kInvalid,       // registers hold no plausible date (never set)
};

HandeRtcStatus hande_rtc_read_utc(uint32_t* epoch, uint32_t lock_timeout_ms = 50);
HandeRtcStatus hande_rtc_write_utc(uint32_t epoch, uint32_t lock_timeout_ms = 50);
const char* hande_rtc_status_text(HandeRtcStatus status);
