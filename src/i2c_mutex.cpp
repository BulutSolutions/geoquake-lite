#include "i2c_mutex.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

SemaphoreHandle_t s_mutex = nullptr;
portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_last_slow_log_ms = 0;

constexpr uint32_t kSlowWaitMs = 10;
constexpr uint32_t kSlowLogIntervalMs = 1000;

}  // namespace

bool i2c_mutex_init(void) {
    if (s_mutex) return true;

    SemaphoreHandle_t created = xSemaphoreCreateRecursiveMutex();
    if (!created) {
        Serial.println("[I2C] ERROR: failed to create the I2C mutex");
        return false;
    }

    portENTER_CRITICAL(&s_init_mux);
    if (!s_mutex) {
        s_mutex = created;
        created = nullptr;
    }
    portEXIT_CRITICAL(&s_init_mux);

    // Another task won the race: keep its mutex.
    if (created) vSemaphoreDelete(created);
    return true;
}

bool i2c_mutex_lock(uint32_t timeout_ms) {
    if (!i2c_mutex_init()) return false;

    const uint32_t start_ms = millis();
    const bool locked = xSemaphoreTakeRecursive(s_mutex, timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    const uint32_t waited_ms = millis() - start_ms;

    if (waited_ms > kSlowWaitMs && start_ms - s_last_slow_log_ms >= kSlowLogIntervalMs) {
        s_last_slow_log_ms = start_ms;
        Serial.printf("[I2C] lock waited %lu ms (timeout %lu ms, %s)\n", (unsigned long)waited_ms,
                      (unsigned long)timeout_ms, locked ? "locked" : "timed out");
    }
    return locked;
}

bool i2c_mutex_trylock(void) {
    return i2c_mutex_lock(0);
}

void i2c_mutex_unlock(void) {
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}
