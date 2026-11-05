#include "hande_rtc.h"

#include <Wire.h>

#include "hande_rtc_policy.h"
#include "i2c_mutex.h"

using namespace hande_rtc_policy;

HandeRtcStatus hande_rtc_read_utc(uint32_t* epoch, uint32_t lock_timeout_ms) {
    if (!epoch) return HandeRtcStatus::kInvalid;
    I2CMutexGuard lock(lock_timeout_ms);
    if (!lock.locked()) return HandeRtcStatus::kBusy;
    Wire.beginTransmission(kAddress);
    Wire.write(kTimeRegister);
    if (Wire.endTransmission() != 0 || Wire.requestFrom(static_cast<int>(kAddress), 7) != 7) {
        return HandeRtcStatus::kBusError;
    }
    uint8_t regs[7];
    for (uint8_t& value : regs) value = static_cast<uint8_t>(Wire.read());
    switch (registers_to_epoch(regs, epoch)) {
        case Decode::kOk: return HandeRtcStatus::kOk;
        case Decode::kClockStopped: return HandeRtcStatus::kClockStopped;
        default: return HandeRtcStatus::kInvalid;
    }
}

HandeRtcStatus hande_rtc_write_utc(uint32_t epoch, uint32_t lock_timeout_ms) {
    uint8_t regs[7];
    if (epoch < kValidEpoch || !epoch_to_registers(epoch, regs)) return HandeRtcStatus::kInvalid;
    I2CMutexGuard lock(lock_timeout_ms);
    if (!lock.locked()) return HandeRtcStatus::kBusy;
    Wire.beginTransmission(kAddress);
    Wire.write(kTimeRegister);
    Wire.write(regs, sizeof(regs));
    return Wire.endTransmission() == 0 ? HandeRtcStatus::kOk : HandeRtcStatus::kBusError;
}

const char* hande_rtc_status_text(HandeRtcStatus status) {
    switch (status) {
        case HandeRtcStatus::kOk: return "ok";
        case HandeRtcStatus::kBusy: return "I2C busy";
        case HandeRtcStatus::kBusError: return "no answer";
        case HandeRtcStatus::kClockStopped: return "oscillator stopped";
        default: return "not set";
    }
}
