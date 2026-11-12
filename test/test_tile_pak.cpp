// Map pack parser (src/tile_pak_format.h): packs with four, five and six levels parse, in the v1 and v2 layouts;
// every malformed header/TOC is refused before the firmware reads a tile from it; the bundled pack is valid, has
// all six levels, and every tile in it decodes with the map's QOI decoder.

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <utility>
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

// The pack pack_core_tiles.py writes for levels z1..z`levels`. v2 stores identical payloads once (here: every tile
// with an even slot shares one payload). A tile's filler byte is the low byte of its slot.
std::vector<uint8_t> make_pak(uint16_t version, uint8_t levels = 4) {
    const uint32_t count = tile_count(levels);
    std::vector<uint8_t> b(toc_end(count), 0);
    memcpy(b.data(), "QPAK", 4);
    put_le16(b, 4, version);
    put_le16(b, 6, static_cast<uint16_t>(count));
    uint32_t shared = 0;
    uint32_t i = 0;
    for (uint32_t z = 1; z <= levels; z++) {
        for (uint32_t x = 0; x < (1u << z); x++) {
            for (uint32_t y = 0; y < (1u << z); y++, i++) {
                const int slot = tile_slot(z, x, y, levels);
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

const uint32_t kToc4 = toc_end(tile_count(4));  // 4,096: header + TOC of a four-level pack


void test_counts() {
    TEST_ASSERT_EQUAL_UINT32(340, tile_count(4));
    TEST_ASSERT_EQUAL_UINT32(1364, tile_count(5));
    TEST_ASSERT_EQUAL_UINT32(5460, tile_count(6));
    TEST_ASSERT_EQUAL_UINT32(0, tile_count(3));
    TEST_ASSERT_EQUAL_UINT32(0, tile_count(7));
    TEST_ASSERT_EQUAL_UINT32(kMaxTileCount, tile_count(kMaxLevels));
    for (uint8_t levels = kMinLevels; levels <= kMaxLevels; levels++) {
        TEST_ASSERT_EQUAL_UINT8(levels, levels_for_count(tile_count(levels)));
    }
    TEST_ASSERT_EQUAL_UINT8(0, levels_for_count(0));
    TEST_ASSERT_EQUAL_UINT8(0, levels_for_count(339));
    TEST_ASSERT_EQUAL_UINT8(0, levels_for_count(341));
    TEST_ASSERT_EQUAL_UINT8(0, levels_for_count(5461));
    TEST_ASSERT_EQUAL_UINT32(4096, kToc4);
    TEST_ASSERT_EQUAL_UINT32(65536, toc_end(tile_count(6)));  // does not fit 16 bits
}

void test_slots() {
    TEST_ASSERT_EQUAL(0, tile_slot(1, 0, 0, 4));
    TEST_ASSERT_EQUAL(3, tile_slot(1, 1, 1, 4));
    TEST_ASSERT_EQUAL(4, tile_slot(2, 0, 0, 4));
    TEST_ASSERT_EQUAL(20, tile_slot(3, 0, 0, 4));
    TEST_ASSERT_EQUAL(84, tile_slot(4, 0, 0, 4));
    TEST_ASSERT_EQUAL(339, tile_slot(4, 15, 15, 4));
    TEST_ASSERT_EQUAL(-1, tile_slot(0, 0, 0, 4));
    TEST_ASSERT_EQUAL(-1, tile_slot(5, 0, 0, 4));   // a four-level pack has no z5 tile
    TEST_ASSERT_EQUAL(-1, tile_slot(2, 4, 0, 4));
    TEST_ASSERT_EQUAL(-1, tile_slot(4, 0, 16, 4));
    TEST_ASSERT_EQUAL(340, tile_slot(5, 0, 0, 5));
    TEST_ASSERT_EQUAL(1363, tile_slot(5, 31, 31, 5));
    TEST_ASSERT_EQUAL(-1, tile_slot(6, 0, 0, 5));   // a five-level pack has no z6 tile
    TEST_ASSERT_EQUAL(1364, tile_slot(6, 0, 0, 6));
    TEST_ASSERT_EQUAL(5459, tile_slot(6, 63, 63, 6));
    TEST_ASSERT_EQUAL(-1, tile_slot(6, 64, 0, 6));
    TEST_ASSERT_EQUAL(-1, tile_slot(6, 0, 64, 6));
    TEST_ASSERT_EQUAL(-1, tile_slot(7, 0, 0, 6));
    TEST_ASSERT_EQUAL(-1, tile_slot(1, 0, 0, 7));   // more levels than the format has
    // The low levels keep their slots in a bigger pack, and z/x/y order is slot order: every tile of every level
    // has its own slot, 0..count-1.
    for (uint8_t levels = kMinLevels; levels <= kMaxLevels; levels++) {
        int expect = 0;
        for (uint32_t z = 1; z <= levels; z++) {
            for (uint32_t x = 0; x < (1u << z); x++) {
                for (uint32_t y = 0; y < (1u << z); y++, expect++) {
                    if (tile_slot(z, x, y, levels) != expect) TEST_ASSERT_EQUAL(expect, tile_slot(z, x, y, levels));
                }
            }
        }
        TEST_ASSERT_EQUAL(static_cast<int>(tile_count(levels)), expect);
    }
}

void test_v1_and_v2_parse() {
    for (uint8_t levels = kMinLevels; levels <= kMaxLevels; levels++) {
        for (uint16_t v = 1; v <= 2; v++) {
            const std::vector<uint8_t> b = make_pak(v, levels);
            Index idx;
            idx.levels = 0;
            TEST_ASSERT_EQUAL_STRING("ok", result_name(parse_buf(b, &idx)));
            TEST_ASSERT_EQUAL_UINT8(levels, idx.levels);
            // A tile's entry points at its own payload: its filler byte is its slot (z3 5/2 = slot 62, 6 filler
            // bytes; in v2 odd slot 65 = z3 5/5, 2 filler bytes, not shared).
            const int slot = tile_slot(3, 5, v == 1 ? 2 : 5, levels);
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(slot), b[idx.entry[slot].offset + 14]);
            // The last tile of the highest level (an odd slot with filler bytes in every pack size).
            const uint32_t n = 1u << levels;
            const int last = tile_slot(levels, n - 1, n - 1, levels);
            TEST_ASSERT_EQUAL(static_cast<int>(tile_count(levels)) - 1, last);
            TEST_ASSERT_TRUE(last % 2 == 1 && last % 7 != 0);
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(last), b[idx.entry[last].offset + 14]);
        }
        // v2 really shares: two even slots point at the same bytes.
        const std::vector<uint8_t> b2 = make_pak(2, levels);
        Index idx;
        TEST_ASSERT_EQUAL(static_cast<int>(Result::Ok), static_cast<int>(parse_buf(b2, &idx)));
        TEST_ASSERT_EQUAL_UINT32(idx.entry[0].offset, idx.entry[2].offset);
        TEST_ASSERT_TRUE(b2.size() < make_pak(1, levels).size());
    }
}

void expect(Result want, const std::vector<uint8_t>& b, uint32_t file_size) {
    Index idx;
    idx.levels = kMaxLevels;  // a refused pack must leave no level behind
    TEST_ASSERT_EQUAL_STRING(result_name(want), result_name(parse(b.data(), b.size(), file_size, &idx)));
    TEST_ASSERT_EQUAL_UINT8(0, idx.levels);
}

void test_rejects() {
    const std::vector<uint8_t> good = make_pak(1);
    const uint32_t size = static_cast<uint32_t>(good.size());
    std::vector<uint8_t> b;

    Index idx;
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(good.data(), kToc4 - 1, size, &idx)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(good.data(), good.size(), kToc4 - 1, &idx)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(good.data(), kHeaderBytes - 1, size, &idx)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(nullptr, good.size(), size, &idx)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(good.data(), good.size(), size, nullptr)));

    b = good; b[0] = 'X'; expect(Result::Magic, b, size);
    b = good; put_le16(b, 4, 3); expect(Result::Version, b, size);
    b = good; put_le16(b, 4, 0); expect(Result::Version, b, size);
    b = good; put_le16(b, 6, 0); expect(Result::Count, b, size);
    b = good; put_le16(b, 6, 339); expect(Result::Count, b, size);
    b = good; put_le16(b, 6, 341); expect(Result::Count, b, size);
    b = good; put_le16(b, 6, 1363); expect(Result::Count, b, size);
    b = good; put_le16(b, 6, 5461); expect(Result::Count, b, size);
    // A header that announces more levels than the file has room for a TOC of.
    TEST_ASSERT_TRUE(size < toc_end(tile_count(5)));
    b = good; put_le16(b, 6, 1364); expect(Result::Short, b, size);
    b = good; b[entry_at(7)] = 5; expect(Result::Coord, b, size);                 // z5 in a four-level pack
    b = good; b[entry_at(7)] = 0; expect(Result::Coord, b, size);                 // z0
    b = good; b[entry_at(0) + 1] = 2; expect(Result::Coord, b, size);             // z1 x=2
    b = good; memcpy(&b[entry_at(1)], &b[entry_at(0)], 3); expect(Result::Duplicate, b, size);
    b = good; put_le32(b, entry_at(9) + 4, kToc4 - 1); expect(Result::Bounds, b, size);    // inside the TOC
    b = good; put_le32(b, entry_at(9) + 4, size - 10); expect(Result::Bounds, b, size);    // runs past the end
    b = good; put_le32(b, entry_at(9) + 4, size + 1); expect(Result::Bounds, b, size);     // starts past the end
    b = good; put_le32(b, entry_at(9) + 4, 0xFFFFFFF0u); put_le32(b, entry_at(9) + 8, 0x20);
    expect(Result::Bounds, b, size);                                                       // offset + length overflows
    b = good; put_le32(b, entry_at(9) + 8, kMinTileBytes - 1); expect(Result::Length, b, size);
    b = good; put_le32(b, entry_at(9) + 8, kMaxTileBytes + 1); expect(Result::Length, b, size);
    // The same pack cut short (a download that stopped early) is out of bounds.
    expect(Result::Bounds, good, size - 1);

    // The same checks hold in a six-level pack: a level it does not have, a payload inside its (longer) TOC.
    const std::vector<uint8_t> good6 = make_pak(2, 6);
    const uint32_t size6 = static_cast<uint32_t>(good6.size());
    b = good6; b[entry_at(5459)] = 7; expect(Result::Coord, b, size6);            // z7
    b = good6; b[entry_at(5459) + 1] = 64; expect(Result::Coord, b, size6);       // z6 x=64
    b = good6; memcpy(&b[entry_at(5459)], &b[entry_at(5458)], 3); expect(Result::Duplicate, b, size6);
    b = good6; put_le32(b, entry_at(1364) + 4, kToc4); expect(Result::Bounds, b, size6);   // inside the 64 KB TOC
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse(good6.data(), kToc4, size6, &idx)));  // only 4 KB read
    // A five-level pack refuses a z6 entry.
    const std::vector<uint8_t> good5 = make_pak(1, 5);
    b = good5; b[entry_at(1363)] = 6; expect(Result::Coord, b, static_cast<uint32_t>(good5.size()));
}

void test_header() {
    const std::vector<uint8_t> good = make_pak(2, 6);
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_STRING("ok", result_name(parse_header(good.data(), kHeaderBytes, &count)));
    TEST_ASSERT_EQUAL_UINT32(5460, count);
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse_header(good.data(), kHeaderBytes - 1, &count)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse_header(nullptr, kHeaderBytes, &count)));
    TEST_ASSERT_EQUAL_STRING("short", result_name(parse_header(good.data(), kHeaderBytes, nullptr)));
    std::vector<uint8_t> b = good;
    put_le16(b, 6, 5459);
    count = 7;
    TEST_ASSERT_EQUAL_STRING("count", result_name(parse_header(b.data(), kHeaderBytes, &count)));
    TEST_ASSERT_EQUAL_UINT32(7, count);  // left alone when refused
}

// A pack file in memory for load_index: read() hands out the bytes in order until `readable` runs out,
// alloc()/release() are counted.
struct FakeFile {
    const std::vector<uint8_t>& bytes;
    size_t readable;            // bytes the file really gives (a file cut short gives fewer than its size says)
    bool can_alloc = true;
    size_t pos = 0;
    int allocs = 0;
    int releases = 0;
    uint32_t alloc_bytes = 0;   // size of the last TOC request
    std::vector<uint32_t> reads;
};

Result load(FakeFile& file, uint32_t file_size, Index* idx) {
    return load_index(
        file_size,
        [&file](uint8_t* dst, uint32_t n) {
            file.reads.push_back(n);
            if (file.pos + n > file.readable) return false;
            memcpy(dst, file.bytes.data() + file.pos, n);
            file.pos += n;
            return true;
        },
        [&file](uint32_t n) -> uint8_t* {
            file.alloc_bytes = n;
            if (!file.can_alloc) return nullptr;
            file.allocs++;
            return static_cast<uint8_t*>(malloc(n));
        },
        [&file](uint8_t* p) {
            file.releases++;
            free(p);
        },
        idx);
}

// The loader's steps (tile_pak::load_index, used by src/tile_pak.cpp with a real file and PSRAM): header first,
// then exactly the TOC it announces; every failure leaves no level behind and hands the TOC memory back.
void test_load_index() {
    for (uint8_t levels = kMinLevels; levels <= kMaxLevels; levels++) {
        const std::vector<uint8_t> b = make_pak(2, levels);
        const uint32_t toc = toc_end(tile_count(levels));
        FakeFile file{b, b.size()};
        Index idx;
        idx.levels = 0;
        TEST_ASSERT_EQUAL_STRING("ok", result_name(load(file, static_cast<uint32_t>(b.size()), &idx)));
        TEST_ASSERT_EQUAL_UINT8(levels, idx.levels);
        TEST_ASSERT_EQUAL(1, file.allocs);
        TEST_ASSERT_EQUAL(1, file.releases);
        TEST_ASSERT_EQUAL_UINT32(toc, file.alloc_bytes);
        TEST_ASSERT_EQUAL(2, static_cast<int>(file.reads.size()));  // the header, then the rest of the TOC
        TEST_ASSERT_EQUAL_UINT32(kHeaderBytes, file.reads[0]);
        TEST_ASSERT_EQUAL_UINT32(toc - kHeaderBytes, file.reads[1]);
        Index whole;
        TEST_ASSERT_EQUAL_STRING("ok", result_name(parse_buf(b, &whole)));
        TEST_ASSERT_EQUAL_INT(0, memcmp(idx.entry, whole.entry, tile_count(levels) * sizeof(Entry)));
    }

    const std::vector<uint8_t> good = make_pak(2, 6);
    const uint32_t size = static_cast<uint32_t>(good.size());
    const uint32_t toc6 = toc_end(tile_count(6));
    Index idx;
    auto refused = [&](const char* want, FakeFile& file, uint32_t file_size, int allocs) {
        idx.levels = kMaxLevels;
        TEST_ASSERT_EQUAL_STRING(want, result_name(load(file, file_size, &idx)));
        TEST_ASSERT_EQUAL_UINT8(0, idx.levels);
        TEST_ASSERT_EQUAL(allocs, file.allocs);
        TEST_ASSERT_EQUAL(allocs, file.releases);  // whatever was taken is handed back
    };

    {   // too small or too big to be a pack: nothing is read
        FakeFile file{good, good.size()};
        refused("size", file, kHeaderBytes - 1, 0);
        refused("size", file, kMaxPakBytes + 1, 0);
        TEST_ASSERT_TRUE(file.reads.empty());
    }
    {   // the file ends inside the header
        FakeFile file{good, 10};
        refused("short", file, size, 0);
    }
    {   // not a pack: refused after the header, before any memory is taken
        std::vector<uint8_t> bad = good;
        bad[0] = 'X';
        FakeFile file{bad, bad.size()};
        refused("magic", file, size, 0);
        TEST_ASSERT_EQUAL(1, static_cast<int>(file.reads.size()));
    }
    {   // the header announces six levels, the file is shorter than their TOC
        FakeFile file{good, good.size()};
        refused("short", file, toc6 - 1, 0);
    }
    {   // no memory for the 64 KB TOC
        FakeFile file{good, good.size()};
        file.can_alloc = false;
        refused("memory", file, size, 0);
        TEST_ASSERT_EQUAL_UINT32(toc6, file.alloc_bytes);
    }
    {   // the file ends inside the TOC: the memory is handed back
        FakeFile file{good, 40000};
        refused("short", file, size, 1);
    }
    {   // a TOC that lists a tile twice: refused, the memory is handed back
        std::vector<uint8_t> bad = good;
        memcpy(&bad[entry_at(5459)], &bad[entry_at(5458)], 3);
        FakeFile file{bad, bad.size()};
        refused("duplicate", file, size, 1);
    }
    {
        FakeFile file{good, good.size()};
        TEST_ASSERT_EQUAL_STRING("short", result_name(load(file, size, nullptr)));
        TEST_ASSERT_TRUE(file.reads.empty());
    }
}

// The bundled pack (v2, six levels, built by pack_core_tiles.py from the Natural Earth tiles).
void test_real_packs() {
    const char* path = GQ_PROJECT_DIR "/data/GeoQuakeLite/tiles/core.pak";
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::vector<uint8_t> b;
    static uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
    fclose(f);
    TEST_ASSERT_TRUE(b.size() <= kMaxPakBytes);
    Index idx;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", result_name(parse_buf(b, &idx)), path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, read_le16(&b[4]), path);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(kMaxLevels, idx.levels, path);  // the release ships z1..z6

    // Every distinct payload decodes as a 256x256 tile with the map's decoder.
    const uint32_t count = tile_count(idx.levels);
    const uint32_t low_count = tile_count(4);
    std::vector<uint8_t> px(kTileSide * kTileSide * 2);
    std::set<std::pair<uint32_t, uint32_t>> decoded;
    std::set<uint32_t> low_offsets;
    for (uint32_t t = 0; t < count; t++) {
        const Entry& e = idx.entry[t];
        if (t < low_count) low_offsets.insert(e.offset);
        if (!decoded.insert({e.offset, e.length}).second) continue;
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, qoi_decode_from_memory(&b[e.offset], e.length, px.data(), kTileSide,
                                                                kTileSide, false), path);
    }
    TEST_ASSERT_TRUE(decoded.size() < count);  // v2 shares the plain sea and land tiles

    // The packer stores the payloads z1..z4 use last (the cheapest part of the file to reach in LittleFS).
    uint32_t first_low = 0xFFFFFFFFu, last_high_only = 0;
    for (uint32_t o : low_offsets) first_low = o < first_low ? o : first_low;
    for (uint32_t t = low_count; t < count; t++) {
        const uint32_t o = idx.entry[t].offset;
        if (!low_offsets.count(o) && o > last_high_only) last_high_only = o;
    }
    TEST_ASSERT_TRUE_MESSAGE(last_high_only < first_low, "z1..z4 payloads are not at the end of the pack");
}

}  // namespace

void run_tile_pak_tests() {  // called from test_mercator.cpp's main
    RUN_TEST(test_counts);
    RUN_TEST(test_slots);
    RUN_TEST(test_v1_and_v2_parse);
    RUN_TEST(test_rejects);
    RUN_TEST(test_header);
    RUN_TEST(test_load_index);
    RUN_TEST(test_real_packs);
}
