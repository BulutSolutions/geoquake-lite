#pragma once

// The ONE lock for the shared I2C bus (GPIO 15/14: touch, AXP2101 PMU, QMI8658 IMU, PCF85063 RTC, audio codecs).
// Every task that talks to a device on that bus holds this lock for the whole transaction.
//
// - Safe before i2c_mutex_init(): the first lock creates the mutex (race-free), so start-up order cannot turn a
//   lock into a silent no-op or a permanent failure.
// - Recursive: a task that holds the lock may take it again (helpers that lock inside a locked section).
//   The task that locked must be the task that unlocks.
// - timeout_ms 0 = try once without waiting.
// - A wait longer than 10 ms is logged (at most once per second): it means another task held the bus too long.

#include <stdint.h>

bool i2c_mutex_init(void);
bool i2c_mutex_lock(uint32_t timeout_ms);
bool i2c_mutex_trylock(void);
void i2c_mutex_unlock(void);

// Holds the lock for one scope. Check locked() before touching the bus.
class I2CMutexGuard {
public:
    explicit I2CMutexGuard(uint32_t timeout_ms) : locked_(i2c_mutex_lock(timeout_ms)) {}
    ~I2CMutexGuard() {
        if (locked_) i2c_mutex_unlock();
    }
    bool locked() const { return locked_; }

    I2CMutexGuard(const I2CMutexGuard&) = delete;
    I2CMutexGuard& operator=(const I2CMutexGuard&) = delete;

private:
    bool locked_;
};
