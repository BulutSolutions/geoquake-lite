// Core map pack parser (src/tile_pak_format.h): the v1 and v2 layouts parse to the same tiles; every malformed
// header/TOC is refused before the firmware reads a tile from it; the bundled pack is valid and every tile in it
// decodes with the map's QOI decoder.

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../src/tile_pak_format.h"
#include "../src/qoi_decoder.h"

#ifndef GQ_PROJECT_DIR
#define GQ_PROJECT_DIR "."
#endif

using namespace tile_pak;

namespace {

// A minimal 256x256 QOI payload: header + `extra` filler bytes + end marker (parse() does not decode).
std::vector<uint8_t> fake_tile(uint8_t fill, size_t extra = 0) {
    std::vector<uint8_t> t = {'q', 'o', 'i', 'f', 0, 0, 1, 0, 0, 0, 1, 0, 3, 0};
    t.insert(t.end(), extra, fill);
    const uint8_t end[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    t.insert(t.end(), end, end + 8);
    return t;
}

void put_le16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = v & 0xFF; b[at + 1] = v >> 8; }
void put_le32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

// The pack pack_core_tiles.py writes. v2 stores identical payloads once (here: every tile with an even slot shares
// one payload).
std::vector<uint8_t> make_pak(uint16_t version) {
    std::vector<uint8_t> b(kTocEnd, 0);
    memcpy(b.data(), "QPAK", 4);
    put_le16(b, 4, version);
    put_le16(b, 6, kTileCount);
    uint32_t shared = 0;
    uint32_t i = 0;
    for (uint32_t z = 1; z <= 4; z++) {
        for (uint32_t x = 0; x < (1u << z); x++) {
            for (uint32_t y = 0; y < (1u << z); y++, i++) {
                const int slot = tile_slot(z, x, y);
                const bool dup = version == 2 && slot % 2 == 0;
                const std::vector<uint8_t> tile = fake_tile(dup ? 0xAA : static_cast<uint8_t>(slot), dup ? 4 : slot % 7);
                uint32_t offset;
                if (dup && shared) {
                    offset = shared;
                } else {
                    offset = static_cast<uint32_t>(b.size());
                    b.insert(b.end(), tile.begin(), tile.end());
                    if (dup) shared = offset;
                }
                const size_t e = kHeaderBytes + i * kEntryBytes;
                b[e] = z; b[e + 1] = x; b[e + 2] = y;
                put_le32(b, e + 4, offset);
                put_le32(b, e + 8, static_cast<uint32_t>(tile.size()));
            }
        }
    }
    return b;
}

Result parse_buf(const std::vector<uint8_t>& b, Index* idx) {
    return parse(b.data(), b.size(), static_cast<uint32_t>(b.size()), idx);
}

size_t entry_at(uint32_t i) { return kHeaderBytes + i * kEntryBytes; }


void test_slots() {
    TEST_ASSERT_EQUAL(0, tile_slot(1, 0, 0));
    TEST_ASSERT_EQUAL(3, tile_slot(1, 1, 1));
    TEST_ASSERT_EQUAL(4, tile_slot(2, 0, 0));
    TEST_ASSERT_EQUAL(20, tile_slot(3, 0, 0));
    TEST_ASSERT_EQUAL(84, tile_slot(4, 0, 0));
    TEST_ASSERT_EQUAL(339, tile_slot(4, 15, 15));
    TEST_ASSERT_EQUAL(-1, tile_slot(0, 0, 0));
    TEST_ASSERT_EQUAL(-1, tile_slot(5, 0, 0));
    TEST_ASSERT_EQUAL(-1, tile_slot(2, 4, 0));
    TEST_ASSERT_EQUAL(-1, tile_slot(4, 0, 16));
}

void test_v1_and_v2_parse() {
    for (uint16_t v = 1; v <= 2; v++) {
        const std::vector<uint8_t> b = make_pak(v);
        Index idx;
        TEST_ASSERT_EQUAL_STRING("ok", result_name(parse_buf(b, &idx)));
        // A tile's entry points at its own payload: its filler byte is its slot (z3 5/2 = slot 62, 6 filler bytes;
        // in v2 odd slot 65 = z3 5/5, 2 filler bytes, not shared).
        const int slot = tile_slot(3, 5, v == 1 ? 2 : 5);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(slot), b[idx.entry[slot].offset + 14]);
    }
    // v2 really shares: two even slots point at the same bytes.
    const std::vector<uint8_t> b2 = make_pak(2);
    Index idx;
    TEST_ASSERT_EQUAL(static_cast<int>(Result::Ok), static_cast<int>(parse_buf(b2, &idx)));
    TEST_ASSERT_EQUAL_UINT32(idx.entry[0].offset, idx.entry[2].offset);
    TEST_ASSERT_TRUE(b2.size() < make_pak(1).size());
}

void expect(Result want, const std::vector<uint8_t>& b, uint32_t file_size) {
    Index idx;
    TEST_ASSERT_EQUAL_STRING(result_name(want), result_name(parse(b.data(), b.size(), file_size, &idx)));
}

void test_rejects() {
    const std::vector<uint8_t> good = make_pak(1);
    const uint32_t size = static_cast<uint32_t>(good.size());
    std::vector<uint8_t> b;

    Index idx;
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(good.data(), kTocEnd - 1, size, &idx)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(good.data(), good.size(), kTocEnd - 1, &idx)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(nullptr, good.size(), size, &idx)));

    b = good; b[0] = 'X'; expect(Result::Magic, b, size);
    b = good; put_le16(b, 4, 3); expect(Result::Version, b, size);
    b = good; put_le16(b, 4, 0); expect(Result::Version, b, size);
    b = good; put_le16(b, 6, kTileCount - 1); expect(Result::Count, b, size);
    b = good; put_le16(b, 6, kTileCount + 1); expect(Result::Count, b, size);
    b = good; b[entry_at(7)] = 5; expect(Result::Coord, b, size);                 // z5 in the core pack
    b = good; b[entry_at(7)] = 0; expect(Result::Coord, b, size);                 // z0
    b = good; b[entry_at(0) + 1] = 2; expect(Result::Coord, b, size);             // z1 x=2
    b = good; memcpy(&b[entry_at(1)], &b[entry_at(0)], 3); expect(Result::Duplicate, b, size);
    b = good; put_le32(b, entry_at(9) + 4, kTocEnd - 1); expect(Result::Bounds, b, size);  // inside the TOC
    b = good; put_le32(b, entry_at(9) + 4, size - 10); expect(Result::Bounds, b, size);    // runs past the end
    b = good; put_le32(b, entry_at(9) + 4, size + 1); expect(Result::Bounds, b, size);     // starts past the end
    b = good; put_le32(b, entry_at(9) + 4, 0xFFFFFFF0u); put_le32(b, entry_at(9) + 8, 0x20);
    expect(Result::Bounds, b, size);                                                       // offset + length overflows
    b = good; put_le32(b, entry_at(9) + 8, kMinTileBytes - 1); expect(Result::Length, b, size);
    b = good; put_le32(b, entry_at(9) + 8, kMaxTileBytes + 1); expect(Result::Length, b, size);
    // The same pack cut short (a download that stopped early) is out of bounds.
    expect(Result::Bounds, good, size - 1);
}

// The bundled pack (v2, built by pack_core_tiles.py from the Natural Earth tiles).
void test_real_packs() {
    const char* path = GQ_PROJECT_DIR "/data/GeoQuakeLite/tiles/core.pak";
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::vector<uint8_t> b;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
    fclose(f);
    TEST_ASSERT_TRUE(b.size() <= kMaxPakBytes);
    Index idx;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", result_name(parse_buf(b, &idx)), path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, read_le16(&b[4]), path);
    std::vector<uint8_t> px(kTileSide * kTileSide * 2);
    for (uint32_t t = 0; t < kTileCount; t++) {
        const Entry& e = idx.entry[t];
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, qoi_decode_from_memory(&b[e.offset], e.length, px.data(), kTileSide,
                                                                kTileSide, false), path);
    }
}

}  // namespace

void run_tile_pak_tests() {  // called from test_mercator.cpp's main
    RUN_TEST(test_slots);
    RUN_TEST(test_v1_and_v2_parse);
    RUN_TEST(test_rejects);
    RUN_TEST(test_real_packs);
}
