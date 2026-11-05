#pragma once

// Pure PCF85063 time-register math (no Arduino): host-tested on the host.
// Contract: the board's RTC holds UTC. The time zone lives in software (TZ / settings), so the clock reads
// correctly whenever it was set.

#include <stdint.h>
#include <time.h>

namespace hande_rtc_policy {

constexpr uint8_t kAddress = 0x51;
constexpr uint8_t kTimeRegister = 0x04;      // seconds; 7 registers: sec, min, hour, day, weekday, month, year
constexpr uint8_t kOscillatorStopped = 0x80;  // bit 7 of the seconds register: time is not valid
constexpr uint32_t kValidEpoch = 1700000000U;  // 2023-11-14: anything earlier is an unset clock

inline uint8_t bcd_to_dec(uint8_t value) { return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0F)); }
inline uint8_t dec_to_bcd(uint8_t value) { return static_cast<uint8_t>(((value / 10) << 4) | (value % 10)); }

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm): no time zone involved.
inline int64_t days_from_civil(int32_t year, uint32_t month, uint32_t day) {
    year -= month <= 2;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t year_of_era = static_cast<uint32_t>(year - era * 400);
    const uint32_t adjusted_month = static_cast<uint32_t>(static_cast<int32_t>(month) + (month > 2 ? -3 : 9));
    const uint32_t day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const uint32_t day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
}

enum class Decode : uint8_t { kOk, kClockStopped, kInvalid };

// Seven time registers -> UTC epoch seconds.
inline Decode registers_to_epoch(const uint8_t regs[7], uint32_t* epoch) {
    if (regs[0] & kOscillatorStopped) return Decode::kClockStopped;
    const uint32_t sec = bcd_to_dec(regs[0] & 0x7F);
    const uint32_t min = bcd_to_dec(regs[1] & 0x7F);
    const uint32_t hour = bcd_to_dec(regs[2] & 0x3F);
    const uint32_t day = bcd_to_dec(regs[3] & 0x3F);
    const uint32_t month = bcd_to_dec(regs[5] & 0x1F);
    const int32_t year = 2000 + bcd_to_dec(regs[6]);
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
        return Decode::kInvalid;
    }
    const int64_t value = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
    if (value < kValidEpoch || value > UINT32_MAX) return Decode::kInvalid;
    *epoch = static_cast<uint32_t>(value);
    return Decode::kOk;
}

// UTC epoch seconds -> seven time registers (oscillator-stopped flag cleared).
inline bool epoch_to_registers(uint32_t epoch, uint8_t regs[7]) {
    const time_t raw = static_cast<time_t>(epoch);
    struct tm utc {};
    if (!gmtime_r(&raw, &utc)) return false;
    regs[0] = dec_to_bcd(static_cast<uint8_t>(utc.tm_sec)) & 0x7F;
    regs[1] = dec_to_bcd(static_cast<uint8_t>(utc.tm_min));
    regs[2] = dec_to_bcd(static_cast<uint8_t>(utc.tm_hour));
    regs[3] = dec_to_bcd(static_cast<uint8_t>(utc.tm_mday));
    regs[4] = static_cast<uint8_t>(utc.tm_wday & 0x07);
    regs[5] = dec_to_bcd(static_cast<uint8_t>(utc.tm_mon + 1));
    regs[6] = dec_to_bcd(static_cast<uint8_t>(utc.tm_year % 100));
    return true;
}

}  // namespace hande_rtc_policy
