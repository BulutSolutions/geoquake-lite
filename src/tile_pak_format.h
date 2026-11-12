#pragma once

// The map pack (QPAK, tools/tile_generator/pack_core_tiles.py): the QOI tiles of zoom levels z1..zN in one file,
// /GeoQuakeLite/tiles/core.pak, read tile by tile at an offset. A pack holds every tile of its levels, so the tile
// count tells N: 340 = z1..z4, 1,364 = z1..z5, 5,460 = z1..z6. Pure code (no Arduino): unit-tested in test/.
//
//   header  16 B   "QPAK", u16 version (1 or 2), u16 tile count, 8 reserved bytes
//   TOC     count x 12 B, little-endian: u8 z, u8 x, u8 y, u8 pad, u32 offset (from file start), u32 length
//   payload QOI tiles. v1: one payload per entry, in TOC order. v2: identical payloads stored once, their entries
//           share an offset; the packer stores the payloads of z1..z4 last (cheapest to reach in LittleFS).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace tile_pak {

constexpr uint32_t kHeaderBytes = 16;
constexpr uint32_t kEntryBytes = 12;
constexpr uint8_t kMinLevels = 4;
constexpr uint8_t kMaxLevels = 6;
constexpr uint32_t kMaxTileCount = 4 + 16 + 64 + 256 + 1024 + 4096;      // z1..z6: 5,460
constexpr uint32_t kMinTileBytes = 14 + 8;                               // QOI header + end marker
constexpr uint32_t kMaxTileBytes = 64 * 1024;                            // tile_manager's QOI scratch buffer
constexpr uint32_t kMaxPakBytes = 12 * 1024 * 1024;                      // a bigger file is not a map pack
constexpr uint32_t kTileSide = 256;

// Tiles in a pack with levels z1..z`levels` (0: not a supported number of levels).
constexpr uint32_t tile_count(uint8_t levels) {
    return levels == 4 ? 340u : levels == 5 ? 1364u : levels == 6 ? 5460u : 0u;
}

// Levels of a pack with `count` tiles (0: not a whole set of levels).
constexpr uint8_t levels_for_count(uint32_t count) {
    return count == 340u ? 4 : count == 1364u ? 5 : count == 5460u ? 6 : 0;
}

// Header + TOC bytes of a pack with `count` tiles (65,536 for six levels: keep it out of 16-bit variables).
constexpr uint32_t toc_end(uint32_t count) { return kHeaderBytes + count * kEntryBytes; }

struct Entry {
    uint32_t offset;
    uint32_t length;
};

// Entries by slot (tile_slot), filled by parse(). 43.7 KB: lives in PSRAM on the device, never on a task stack or
// in static data (internal RAM).
struct Index {
    uint8_t levels;  // highest level in the pack (4..6); 0 = no valid pack parsed, the entries mean nothing
    Entry entry[kMaxTileCount];
};

enum class Result : uint8_t { Ok, Short, Magic, Version, Count, Coord, Duplicate, Bounds, Length, Size, Memory };

inline const char* result_name(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::Short: return "short";
        case Result::Magic: return "magic";
        case Result::Version: return "version";
        case Result::Count: return "count";
        case Result::Coord: return "coord";
        case Result::Duplicate: return "duplicate";
        case Result::Bounds: return "bounds";
        case Result::Length: return "length";
        case Result::Size: return "size";
        case Result::Memory: return "memory";
    }
    return "?";
}

inline uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Slot of a tile in Index::entry for a pack with levels z1..z`levels`; -1 when the pack has no such tile. Always
// pass the levels of the pack the index was parsed from: a z5 tile has a slot only in a pack that has z5.
inline int tile_slot(uint32_t z, uint32_t x, uint32_t y, uint8_t levels) {
    if (levels > kMaxLevels || z < 1 || z > levels) return -1;
    const uint32_t n = 1u << z;
    if (x >= n || y >= n) return -1;
    static const uint16_t kBase[kMaxLevels + 1] = {0, 0, 4, 20, 84, 340, 1364};
    return static_cast<int>(kBase[z] + x * n + y);
}

// Checks the 16-byte header and sets *count to its tile count (340, 1,364 or 5,460). Read this first: the count
// decides how many TOC bytes follow (toc_end).
inline Result parse_header(const uint8_t* head, size_t head_len, uint32_t* count) {
    if (!head || !count || head_len < kHeaderBytes) return Result::Short;
    if (head[0] != 'Q' || head[1] != 'P' || head[2] != 'A' || head[3] != 'K') return Result::Magic;
    const uint16_t version = read_le16(head + 4);
    if (version != 1 && version != 2) return Result::Version;
    const uint32_t n = read_le16(head + 6);
    if (levels_for_count(n) == 0) return Result::Count;
    *count = n;
    return Result::Ok;
}

// Checks the header and TOC (the first toc_end(count) bytes of a pack whose total size is file_size) and fills *out.
// Every tile of the pack's levels must be listed exactly once, and every payload must lie after the TOC and inside
// the file. out->levels is set only on Ok; on any error it is 0.
inline Result parse(const uint8_t* head, size_t head_len, uint32_t file_size, Index* out) {
    if (!out) return Result::Short;
    out->levels = 0;
    uint32_t count = 0;
    const Result header = parse_header(head, head_len, &count);
    if (header != Result::Ok) return header;
    const uint32_t toc = toc_end(count);
    if (head_len < toc || file_size < toc) return Result::Short;
    const uint8_t levels = levels_for_count(count);
    for (uint32_t i = 0; i < count; i++) out->entry[i] = {0, 0};  // length 0 = this slot is not listed yet
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = head + kHeaderBytes + i * kEntryBytes;
        const int slot = tile_slot(e[0], e[1], e[2], levels);
        if (slot < 0) return Result::Coord;
        if (out->entry[slot].length != 0) return Result::Duplicate;
        const uint32_t offset = read_le32(e + 4);
        const uint32_t length = read_le32(e + 8);
        if (length < kMinTileBytes || length > kMaxTileBytes) return Result::Length;
        // Overflow-safe: offset <= file_size first, then length <= the bytes after it.
        if (offset < toc || offset > file_size || length > file_size - offset) return Result::Bounds;
        out->entry[slot] = {offset, length};
    }
    out->levels = levels;  // `count` distinct in-range coordinates: every tile of z1..z`levels` is listed
    return Result::Ok;
}

// Loads the index of a pack of `file_size` bytes. The file and the memory come through callables, so the device
// loader (tile_pak.cpp) and the host tests run the same steps:
//   read(dst, n)   copies the next n bytes of the file to dst; false when fewer than n arrive
//   alloc(n)       n bytes for the TOC, nullptr when there are none to spare; handed back with release(p)
// Reads the 16-byte header, then exactly the TOC it announces (4 KB for four levels, 64 KB for six). *out is
// filled on Ok; on any error out->levels is 0. The TOC memory is released before returning, whatever the result.
template <typename Read, typename Alloc, typename Release>
inline Result load_index(uint32_t file_size, Read read, Alloc alloc, Release release, Index* out) {
    if (!out) return Result::Short;
    out->levels = 0;
    if (file_size < kHeaderBytes || file_size > kMaxPakBytes) return Result::Size;
    uint8_t header[kHeaderBytes];
    if (!read(header, kHeaderBytes)) return Result::Short;
    uint32_t count = 0;
    const Result checked = parse_header(header, kHeaderBytes, &count);
    if (checked != Result::Ok) return checked;
    const uint32_t toc_bytes = toc_end(count);
    if (file_size < toc_bytes) return Result::Short;
    uint8_t* toc = alloc(toc_bytes);
    if (!toc) return Result::Memory;
    memcpy(toc, header, kHeaderBytes);
    Result r = Result::Short;
    if (read(toc + kHeaderBytes, toc_bytes - kHeaderBytes)) r = parse(toc, toc_bytes, file_size, out);
    release(toc);
    return r;
}

}  // namespace tile_pak
