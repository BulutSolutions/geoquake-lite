#pragma once

// The core map pack (QPAK, tools/tile_generator/pack_core_tiles.py): the 340 z1-z4 QOI tiles in one file,
// /GeoQuakeLite/tiles/core.pak, read tile by tile at an offset. Pure code (no Arduino): unit-tested in test/.
//
//   header  16 B   "QPAK", u16 version (1 or 2), u16 tile count (340), 8 reserved bytes
//   TOC     340 x 12 B, little-endian: u8 z, u8 x, u8 y, u8 pad, u32 offset (from file start), u32 length
//   payload QOI tiles. v1: one payload per entry, in TOC order (what firmware before the single-file pack
//           streams). v2: identical payloads stored once, their entries share an offset.

#include <stddef.h>
#include <stdint.h>

namespace tile_pak {

constexpr uint32_t kHeaderBytes = 16;
constexpr uint32_t kEntryBytes = 12;
constexpr uint32_t kTileCount = 4 + 16 + 64 + 256;                       // z1..z4
constexpr uint32_t kTocEnd = kHeaderBytes + kTileCount * kEntryBytes;    // 4096: header + TOC
constexpr uint32_t kMinTileBytes = 14 + 8;                               // QOI header + end marker
constexpr uint32_t kMaxTileBytes = 64 * 1024;                            // tile_manager's QOI scratch buffer
constexpr uint32_t kMaxPakBytes = 3 * 1024 * 1024;                       // a download bigger than this is refused
constexpr uint32_t kTileSide = 256;

struct Entry {
    uint32_t offset;
    uint32_t length;
};

// Entries by slot (tile_slot), filled by parse().
struct Index {
    Entry entry[kTileCount];
};

enum class Result : uint8_t { Ok, Short, Magic, Version, Count, Coord, Duplicate, Bounds, Length };

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
    }
    return "?";
}

inline uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Slot of a z1..z4 tile in Index::entry, -1 when the coordinate is not a core tile.
inline int tile_slot(uint32_t z, uint32_t x, uint32_t y) {
    if (z < 1 || z > 4) return -1;
    const uint32_t n = 1u << z;
    if (x >= n || y >= n) return -1;
    static const uint16_t kBase[5] = {0, 0, 4, 20, 84};
    return static_cast<int>(kBase[z] + x * n + y);
}

// Checks the header and TOC (the first kTocEnd bytes of a pack whose total size is file_size) and fills *out.
// Every z1..z4 tile must be listed exactly once, and every payload must lie after the TOC and inside the file.
inline Result parse(const uint8_t* head, size_t head_len, uint32_t file_size, Index* out) {
    if (!head || !out || head_len < kTocEnd || file_size < kTocEnd) return Result::Short;
    if (head[0] != 'Q' || head[1] != 'P' || head[2] != 'A' || head[3] != 'K') return Result::Magic;
    const uint16_t version = read_le16(head + 4);
    if (version != 1 && version != 2) return Result::Version;
    if (read_le16(head + 6) != kTileCount) return Result::Count;
    bool seen[kTileCount] = {};
    for (uint32_t i = 0; i < kTileCount; i++) {
        const uint8_t* e = head + kHeaderBytes + i * kEntryBytes;
        const int slot = tile_slot(e[0], e[1], e[2]);
        if (slot < 0) return Result::Coord;
        if (seen[slot]) return Result::Duplicate;
        seen[slot] = true;
        const uint32_t offset = read_le32(e + 4);
        const uint32_t length = read_le32(e + 8);
        if (length < kMinTileBytes || length > kMaxTileBytes) return Result::Length;
        // Overflow-safe: offset <= file_size first, then length <= the bytes after it.
        if (offset < kTocEnd || offset > file_size || length > file_size - offset) return Result::Bounds;
        out->entry[slot] = {offset, length};
    }
    return Result::Ok;  // 340 distinct in-range coordinates: every core tile is listed
}

}  // namespace tile_pak
