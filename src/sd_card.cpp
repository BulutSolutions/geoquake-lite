#include "sd_card.h"
#include "tile_pak.h"
#include "crash_trace.h"
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifndef GEOQUAKE_MAP_STORAGE_TEST
#define GEOQUAKE_MAP_STORAGE_TEST 0
#endif

// SD Card SPI configuration
// Uses dedicated SPI bus to avoid conflicts with display
static SPIClass sd_spi(HSPI);
static bool sd_initialized = false;
static bool simulate_no_tiles = false;

// Thread safety: SD library is not thread-safe, protect all operations
static SemaphoreHandle_t sd_mutex = NULL;

// High-speed SPI frequency (40MHz for maximum throughput)
#define SD_SPI_FREQUENCY 40000000

bool sd_init() {
#if GEOQUAKE_MAP_STORAGE_TEST
    // Map storage check: force internal flash even when a card is inserted.
    Serial.println("[MAP_TEST] SD card ignored");
    return false;
#endif
    if (sd_initialized) {
        return true;
    }

    // Create mutex for thread-safe SD access (multi-core decode tasks)
    if (!sd_mutex) {
        sd_mutex = xSemaphoreCreateMutex();
        if (!sd_mutex) {
            Serial.println("[SD] Failed to allocate SD mutex");
            return false;
        }
    }

    Serial.println("[SD] Initializing SD card...");

    // Initialize SPI bus for SD card
    sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    // First try at low speed for compatibility
    if (!SD.begin(SD_CS, sd_spi, 4000000)) {
        Serial.println("[SD] Init failed at 4MHz");
        return false;
    }

    // Re-open at the fastest stable rate. Retain the verified 4 MHz mode as
    // the final fallback for marginal cards.
    SD.end();
    static const uint32_t speeds[] = {
        SD_SPI_FREQUENCY, 20000000, 10000000, 4000000};
    bool opened = false;
    for (uint32_t speed : speeds) {
        if (SD.begin(SD_CS, sd_spi, speed)) {
            Serial.printf("[SD] Initialized at %luMHz\n", speed / 1000000);
            opened = true;
            break;
        }
        Serial.printf("[SD] Init failed at %luMHz\n", speed / 1000000);
        SD.end();
    }
    if (!opened) return false;

    sd_initialized = true;
    sd_print_info();

    return true;
}

bool sd_card_ready() {
    return sd_initialized;
}

bool sd_file_exists(const char* path) {
    if (!sd_initialized) return false;

    // Thread-safe: protect SD.exists() for multi-core decode tasks
    if (sd_mutex) xSemaphoreTake(sd_mutex, portMAX_DELAY);
    bool exists = SD.exists(path);
    if (sd_mutex) xSemaphoreGive(sd_mutex);
    return exists;
}

size_t sd_read_file(const char* path, uint8_t* buffer, size_t max_size) {
    if (!sd_initialized || !buffer || max_size == 0) {
        return 0;
    }

    // Thread-safe: protect entire file read operation
    if (sd_mutex) xSemaphoreTake(sd_mutex, portMAX_DELAY);

    File file = SD.open(path, FILE_READ);
    if (!file) {
        if (sd_mutex) xSemaphoreGive(sd_mutex);
        Serial.printf("[SD] Failed to open: %s\n", path);
        return 0;
    }

    size_t file_size = file.size();
    size_t read_size = (file_size < max_size) ? file_size : max_size;

    size_t bytes_read = file.read(buffer, read_size);
    file.close();

    if (sd_mutex) xSemaphoreGive(sd_mutex);

    if (bytes_read != read_size) {
        Serial.printf("[SD] Read incomplete: %s (got %zu of %zu)\n", path, bytes_read, read_size);
    }

    return bytes_read;
}

size_t sd_get_file_size(const char* path) {
    if (!sd_initialized) return 0;

    // Thread-safe: protect file size query
    if (sd_mutex) xSemaphoreTake(sd_mutex, portMAX_DELAY);

    File file = SD.open(path, FILE_READ);
    if (!file) {
        if (sd_mutex) xSemaphoreGive(sd_mutex);
        return 0;
    }

    size_t size = file.size();
    file.close();

    if (sd_mutex) xSemaphoreGive(sd_mutex);
    return size;
}

bool sd_load_core_pak(tile_pak::Index* out) {
    if (simulate_no_tiles) {
        Serial.println("[SD] DEBUG: simulating missing SD tiles until reboot");
        return false;
    }
    if (!sd_initialized) return false;

    sd_lock();
    const bool found = tile_pak_load_index(SD, TILE_PAK_PATH, out);
    sd_unlock();
    Serial.println(found ? "[SD] Core map pack found" : "[SD] No valid core map pack on the SD card");
    return found;
}

uint8_t sd_detail_tile_level() {
    if (simulate_no_tiles || !sd_initialized) return 4;
    sd_lock();
    const uint8_t level = SD.exists(TILE_PAK_DIR "/z6") ? 6 : (SD.exists(TILE_PAK_DIR "/z5") ? 5 : 4);
    sd_unlock();
    return level;
}

void sd_set_simulate_no_tiles(bool enabled) {
    simulate_no_tiles = enabled;
    if (simulate_no_tiles) {
        Serial.println("[SD] DEBUG: SD tile detection override enabled until reboot");
    }
}

bool sd_simulate_no_tiles_enabled() {
    return simulate_no_tiles;
}

void sd_print_info() {
    if (!sd_initialized) {
        Serial.println("[SD] Not initialized");
        return;
    }

    uint8_t cardType = SD.cardType();
    const char* type_str = "UNKNOWN";

    switch (cardType) {
        case CARD_NONE: type_str = "NONE"; break;
        case CARD_MMC:  type_str = "MMC"; break;
        case CARD_SD:   type_str = "SD"; break;
        case CARD_SDHC: type_str = "SDHC"; break;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    uint64_t totalBytes = SD.totalBytes() / (1024 * 1024);
    uint64_t usedBytes = SD.usedBytes() / (1024 * 1024);

    Serial.printf("[SD] Card Type: %s\n", type_str);
    Serial.printf("[SD] Card Size: %llu MB\n", cardSize);
    Serial.printf("[SD] Total: %llu MB, Used: %llu MB\n", totalBytes, usedBytes);
}

// Thread-safe SD access for multi-core decode tasks
void sd_lock() {
    if (!sd_mutex) return;
    xSemaphoreTake(sd_mutex, portMAX_DELAY);
    crash_trace_lock_taken(TRACE_LOCK_SD);
}

void sd_unlock() {
    if (!sd_mutex) return;
    crash_trace_lock_released(TRACE_LOCK_SD);
    xSemaphoreGive(sd_mutex);
}
