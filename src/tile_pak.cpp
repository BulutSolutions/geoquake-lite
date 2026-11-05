#include "tile_pak.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "qoi_decoder.h"

bool tile_pak_load_index(fs::FS& fs, const char* path, tile_pak::Index* out) {
    File f = fs.open(path, FILE_READ);
    if (!f) return false;
    const size_t size = f.size();
    if (size < tile_pak::kTocEnd || size > tile_pak::kMaxPakBytes) {
        f.close();
        Serial.printf("[TILE_PAK] %s: size %u out of range\n", path, static_cast<unsigned>(size));
        return false;
    }
    uint8_t* head = static_cast<uint8_t*>(heap_caps_malloc(tile_pak::kTocEnd, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!head) head = static_cast<uint8_t*>(malloc(tile_pak::kTocEnd));
    if (!head) {
        f.close();
        return false;
    }
    const size_t got = f.read(head, tile_pak::kTocEnd);
    f.close();
    const tile_pak::Result r = got == tile_pak::kTocEnd
                                   ? tile_pak::parse(head, got, static_cast<uint32_t>(size), out)
                                   : tile_pak::Result::Short;
    heap_caps_free(head);
    if (r != tile_pak::Result::Ok) {
        Serial.printf("[TILE_PAK] %s: invalid pack (%s)\n", path, tile_pak::result_name(r));
        return false;
    }
    return true;
}

bool tile_pak_check_payloads(fs::FS& fs, const char* path, const tile_pak::Index& index, bool (*lock)(),
                             void (*unlock)()) {
    const size_t pixels_bytes = tile_pak::kTileSide * tile_pak::kTileSide * 2;
    uint8_t* in = static_cast<uint8_t*>(heap_caps_malloc(tile_pak::kMaxTileBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t* px = static_cast<uint8_t*>(heap_caps_malloc(pixels_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool ok = in && px;
    if (!ok) Serial.println("[TILE_PAK] Out of memory for the tile check");
    for (uint32_t s = 0; s < tile_pak::kTileCount && ok; s++) {
        const tile_pak::Entry& e = index.entry[s];
        bool seen = false;  // v2 shares payloads: decode each distinct (offset, length) once
        for (uint32_t p = 0; p < s && !seen; p++) {
            seen = index.entry[p].offset == e.offset && index.entry[p].length == e.length;
        }
        if (seen) continue;
        if (!lock()) {
            ok = false;  // the app is leaving or the filesystem is shutting down
            break;
        }
        File f = fs.open(path, FILE_READ);
        ok = f && f.seek(e.offset) && f.read(in, e.length) == e.length;
        if (f) f.close();
        unlock();
        if (ok) ok = qoi_decode_from_memory(in, e.length, px, tile_pak::kTileSide, tile_pak::kTileSide, false) == 1;
        if (!ok) Serial.printf("[TILE_PAK] %s: tile slot %u does not decode\n", path, (unsigned)s);
        if ((s & 15) == 15) vTaskDelay(1);  // a background task: let lower-priority work and IDLE run
    }
    if (in) heap_caps_free(in);
    if (px) heap_caps_free(px);
    return ok;
}
