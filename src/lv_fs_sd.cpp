#include "lv_fs_sd.h"
#include "sd_card.h"
#include <Arduino.h>
#include <SD.h>

// LVGL Filesystem Driver for SD Card
// Bridges LVGL's filesystem abstraction to Arduino SD library

static bool fs_initialized = false;

// --- LVGL Filesystem Callbacks ---

static void* sd_open_cb(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
    (void)drv;

    if (!sd_card_ready()) {
        return NULL;
    }

    // Build full path (path comes without drive letter)
    // Handle both "S:path" and "S:/path" formats
    char full_path[128];
    if (path[0] == '/') {
        // Path already has leading slash
        snprintf(full_path, sizeof(full_path), "%s", path);
    } else {
        // Add leading slash
        snprintf(full_path, sizeof(full_path), "/%s", path);
    }

    Serial.printf("[FS] Opening: %s\n", full_path);

    // Only support read mode for tiles
    if (mode != LV_FS_MODE_RD) {
        Serial.printf("[FS] Write mode not supported: %s\n", full_path);
        return NULL;
    }

    File* file = new File();
    sd_lock();
    *file = SD.open(full_path, FILE_READ);
    sd_unlock();

    if (!*file) {
        delete file;
        return NULL;
    }

    return (void*)file;
}

static lv_fs_res_t sd_close_cb(lv_fs_drv_t* drv, void* file_p) {
    (void)drv;

    if (!file_p) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;
    sd_lock();
    file->close();
    sd_unlock();
    delete file;

    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_read_cb(lv_fs_drv_t* drv, void* file_p, void* buf, uint32_t btr, uint32_t* br) {
    (void)drv;

    if (!file_p || !buf || !br) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;
    sd_lock();
    *br = file->read((uint8_t*)buf, btr);
    sd_unlock();

    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_seek_cb(lv_fs_drv_t* drv, void* file_p, uint32_t pos, lv_fs_whence_t whence) {
    (void)drv;

    if (!file_p) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;

    sd_lock();
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
            sd_unlock();
            return LV_FS_RES_INV_PARAM;
    }
    sd_unlock();

    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_tell_cb(lv_fs_drv_t* drv, void* file_p, uint32_t* pos_p) {
    (void)drv;

    if (!file_p || !pos_p) return LV_FS_RES_INV_PARAM;

    File* file = (File*)file_p;
    sd_lock();
    *pos_p = file->position();
    sd_unlock();

    return LV_FS_RES_OK;
}

// --- Public API ---

void lv_fs_sd_init() {
    if (fs_initialized) {
        return;
    }

    if (!sd_card_ready()) {
        Serial.println("[FS] SD card not ready, skipping filesystem init");
        return;
    }

    // Register 'S' drive for SD card
    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);

    fs_drv.letter = 'S';
    fs_drv.open_cb = sd_open_cb;
    fs_drv.close_cb = sd_close_cb;
    fs_drv.read_cb = sd_read_cb;
    fs_drv.seek_cb = sd_seek_cb;
    fs_drv.tell_cb = sd_tell_cb;

    lv_fs_drv_register(&fs_drv);

    fs_initialized = true;
    Serial.println("[FS] LVGL SD filesystem registered (drive S:)");
}

bool lv_fs_sd_ready() {
    return fs_initialized && sd_card_ready();
}
