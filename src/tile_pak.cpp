#include "tile_pak.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

tile_pak::Index* tile_pak_alloc_index() {
    return static_cast<tile_pak::Index*>(
        heap_caps_calloc(1, sizeof(tile_pak::Index), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void tile_pak_free_index(tile_pak::Index* index) {
    if (index) heap_caps_free(index);
}

bool tile_pak_load_index(fs::FS& fs, const char* path, tile_pak::Index* out) {
    if (!out) return false;
    out->levels = 0;
    File f = fs.open(path, FILE_READ);
    if (!f) return false;
    const size_t size = f.size();
    // The steps (header, then the TOC it announces, then the checks) are tile_pak::load_index, shared with the
    // host tests. Here: the file, and PSRAM for the TOC.
    const tile_pak::Result r = tile_pak::load_index(
        static_cast<uint32_t>(size),
        [&f](uint8_t* dst, uint32_t n) { return f.read(dst, n) == n; },
        [](uint32_t n) -> uint8_t* {
            uint8_t* p = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            // Without PSRAM only a four-level TOC (4 KB) may borrow internal RAM; a 64 KB one may not.
            if (!p && n <= 4096) p = static_cast<uint8_t*>(malloc(n));
            return p;
        },
        [](uint8_t* p) { heap_caps_free(p); },
        out);
    f.close();
    if (r != tile_pak::Result::Ok) {
        Serial.printf("[TILE_PAK] %s: invalid pack (%s, %u bytes)\n", path, tile_pak::result_name(r),
                      static_cast<unsigned>(size));
        return false;
    }
    return true;
}
