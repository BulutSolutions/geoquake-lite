#include "lv_fs_flash.h"
#include "tile_pak.h"
#include "crash_trace.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>

#ifndef GEOQUAKE_MAP_STORAGE_TEST
#define GEOQUAKE_MAP_STORAGE_TEST 0
#endif

static bool fs_initialized = false;
static SemaphoreHandle_t flash_mutex = NULL;
static volatile bool flash_quiesced = false;

// F3: Tracks number of open LittleFS File handles (atomic — safe to read/write from any core).
static std::atomic<int> flash_open_handles{0};
static uint32_t s_quiesce_timeout_ms = 5000;

static bool flash_mount() {
    if (!flash_mutex) {
        flash_mutex = xSemaphoreCreateMutex();
    }
    flash_lock();
    if (flash_quiesced) {
        flash_unlock();
        return false;
    }
    if (fs_initialized) {
        flash_unlock();
        return true;
    }
    if (!LittleFS.begin(false)) {
        Serial.println("[FS_FLASH] LittleFS mount failed; embedded fallback tiles unavailable");
        flash_unlock();
        return false;
    }
    fs_initialized = true;
    Serial.println("[FS_FLASH] LittleFS mounted");
    flash_unlock();
    return true;
}

static void* flash_open_cb(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
    (void)drv;

    if (!flash_mount()) {
        return NULL;
    }

    if (mode != LV_FS_MODE_RD) {
        Serial.printf("[FS_FLASH] Write mode not supported: %s\n", path);
        return NULL;
    }

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", path);
    } else {
        snprintf(full_path, sizeof(full_path), "/%s", path);
    }

    File* file = new File();
    flash_lock();
    if (flash_quiesced) {
        flash_unlock();
        delete file;
        return NULL;
    }
    *file = LittleFS.open(full_path, FILE_READ);
    // F3: If open succeeded, increment the open-handle counter so quiesce can wait
    if (*file) {
        flash_open_handles.fetch_add(1, std::memory_order_relaxed);
    }
    flash_unlock();

    if (!*file) {
        delete file;
        return NULL;
    }

    return (void*)file;
}

static lv_fs_res_t flash_close_cb(lv_fs_drv_t* drv, void* file_p) {
    (void)drv;

    if (!file_p) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;
    flash_lock();
    if (!flash_quiesced) {
        file->close();
    }
    // F3: Decrement open-handle counter (counter is read during quiesce wait)
    if (flash_open_handles.load(std::memory_order_relaxed) > 0) {
        flash_open_handles.fetch_sub(1, std::memory_order_relaxed);
    }
    flash_unlock();
    delete file;

    return LV_FS_RES_OK;
}

static lv_fs_res_t flash_read_cb(lv_fs_drv_t* drv, void* file_p, void* buf, uint32_t btr, uint32_t* br) {
    (void)drv;

    if (!file_p || !buf || !br) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;
    flash_lock();
    if (flash_quiesced) {
        *br = 0;
        flash_unlock();
        return LV_FS_RES_OK;
    }
    *br = file->read((uint8_t*)buf, btr);
    flash_unlock();

    return LV_FS_RES_OK;
}

static lv_fs_res_t flash_seek_cb(lv_fs_drv_t* drv, void* file_p, uint32_t pos, lv_fs_whence_t whence) {
    (void)drv;

    if (!file_p) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;
    flash_lock();
    if (flash_quiesced) {
        flash_unlock();
        return LV_FS_RES_OK;
    }
    switch (whence) {
        case LV_FS_SEEK_SET:
            file->seek(pos);
            break;
        case LV_FS_SEEK_CUR:
            file->seek(file->position() + pos);
            break;
        case LV_FS_SEEK_END:
            file->seek(file->size() - pos);
            break;
        default:
            flash_unlock();
            return LV_FS_RES_INV_PARAM;
    }
    flash_unlock();

    return LV_FS_RES_OK;
}

static lv_fs_res_t flash_tell_cb(lv_fs_drv_t* drv, void* file_p, uint32_t* pos_p) {
    (void)drv;

    if (!file_p || !pos_p) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;
    flash_lock();
    if (flash_quiesced) {
        *pos_p = 0;
        flash_unlock();
        return LV_FS_RES_OK;
    }
    *pos_p = file->position();
    flash_unlock();

    return LV_FS_RES_OK;
}

void lv_fs_flash_init() {
    if (!flash_mount()) {
        return;
    }

    static bool driver_registered = false;
    if (driver_registered) {
        return;
    }

    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);

    fs_drv.letter = 'F';
    fs_drv.open_cb = flash_open_cb;
    fs_drv.close_cb = flash_close_cb;
    fs_drv.read_cb = flash_read_cb;
    fs_drv.seek_cb = flash_seek_cb;
    fs_drv.tell_cb = flash_tell_cb;

    lv_fs_drv_register(&fs_drv);

    driver_registered = true;
    Serial.println("[FS_FLASH] LVGL flash filesystem registered (drive F:)");
}

bool lv_fs_flash_ready() {
    return flash_mount();
}

bool flash_load_core_pak(tile_pak::Index* out) {
    // C0: All operations (quiesce check + mount + read) under one lock —
    // eliminates TOCTOU between flash_mount() return and the pack read.
    flash_lock();
    if (flash_quiesced) {
        flash_unlock();
        return false;
    }
    if (!fs_initialized) {
        if (!LittleFS.begin(false)) {
            Serial.println("[FS_FLASH] LittleFS mount failed; flash map tiles unavailable");
            flash_unlock();
            return false;
        }
        fs_initialized = true;
        Serial.println("[FS_FLASH] LittleFS mounted");
    }
    const bool found = tile_pak_load_index(LittleFS, TILE_PAK_PATH, out);
    flash_unlock();
    Serial.println(found ? "[FS_FLASH] Core map pack found" : "[FS_FLASH] No valid core map pack in flash");
    return found;
}

#if GEOQUAKE_MAP_STORAGE_TEST
// Map storage check: drop the map so the next boot starts from an empty flash. The tool resets the chip right after,
// so the tile manager never reads the removed file.
bool flash_map_test_remove_pack() {
    flash_lock();
    bool removed = false;
    if (LittleFS.exists(TILE_PAK_PATH)) removed = LittleFS.remove(TILE_PAK_PATH);
    if (LittleFS.exists(TILE_PAK_PART_PATH)) removed = LittleFS.remove(TILE_PAK_PART_PATH) || removed;
    flash_unlock();
    Serial.println("[MAP_TEST] map removed");
    return removed;
}
#endif

void lv_fs_flash_handle_opened() {
    flash_open_handles.fetch_add(1, std::memory_order_relaxed);
}

void lv_fs_flash_handle_closed() {
    if (flash_open_handles.load(std::memory_order_relaxed) > 0) {
        flash_open_handles.fetch_sub(1, std::memory_order_relaxed);
    }
}

void flash_lock() {
    if (!flash_mutex) return;
    xSemaphoreTake(flash_mutex, portMAX_DELAY);
    crash_trace_lock_taken(TRACE_LOCK_FLASH);
}

void flash_unlock() {
    if (!flash_mutex) return;
    crash_trace_lock_released(TRACE_LOCK_FLASH);
    xSemaphoreGive(flash_mutex);
}

void lv_fs_flash_quiesce() {
    flash_quiesced = true;
    // Drain: take mutex — blocks until any in-flight flash op completes.
    // Because flash_quiesced is set BEFORE taking the lock, any
    // subsequent operation that acquires the lock will see the flag
    // and bail before touching LittleFS.
    flash_lock();
    // Release immediately — the flag alone now gates new operations.
    flash_unlock();

    // F3: Wait for open File handles to be closed by their owners.
    // A handle opened before quiesce was set may still be in use;
    // we must not let LittleFS.end() run while a handle lives.
    uint32_t start = millis();
    while (flash_open_handles.load(std::memory_order_relaxed) > 0 &&
           (millis() - start) < s_quiesce_timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (flash_open_handles.load(std::memory_order_relaxed) > 0) {
        Serial.printf("[FS_FLASH] FATAL: %d open handles still active after quiesce timeout — rebooting\n",
                      (int)flash_open_handles.load(std::memory_order_relaxed));
        delay(1000);
        ESP.restart();
    }
}

void lv_fs_flash_unquiesce() {
    flash_quiesced = false;
}

bool lv_fs_flash_is_quiesced() {
    return flash_quiesced;
}

void lv_fs_flash_set_quiesce_timeout(uint32_t timeout_ms) {
    s_quiesce_timeout_ms = timeout_ms;
}

void flash_unmount() {
    LittleFS.end();
    fs_initialized = false;
    Serial.println("[FS_FLASH] LittleFS unmounted");
}
