#pragma once

// Which earthquakes of a refresh are new to the user (pure, no Arduino: host-tested).
// The alert sound plays for an earthquake that passes the magnitude filter and was not alerted before. "Before" is
// a memory of kCapacity ids, each with the number of the refresh that last had it. When the memory is full, a new
// id takes the place of the one that was in a feed longest ago. The feed holds at most 100 earthquakes and the
// memory 200, so an id of the current refresh is never the one given up: an earthquake cannot alert twice while
// it stays in the feed and over the filter. One that left the feed, or fell under the filter, and comes back
// alerts again only after 100 or more newer ids pushed it out.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace quake_seen_policy {

constexpr size_t kIdLen = 32;      // Earthquake::id
constexpr size_t kCapacity = 200;  // 2 x MAX_STORED_QUAKES

struct Seen {
    char ids[kCapacity][kIdLen];
    uint32_t refresh_of[kCapacity];  // the refresh that last had the id
    size_t count;                    // ids stored
    uint32_t refresh;                // number of the refresh in progress (begin_refresh)
};

// Call once before the earthquakes of a refresh are noted.
inline void begin_refresh(Seen& seen) { ++seen.refresh; }

// One earthquake of the refresh in progress. True when it passes the filter and is not remembered: it is
// remembered now and counts as new. An earthquake under the filter is neither new nor remembered.
inline bool note(Seen& seen, const char* id, float mag, float min_mag) {
    if (!id || !id[0] || mag < min_mag) return false;
    size_t oldest = 0;
    for (size_t i = 0; i < seen.count; ++i) {
        if (strncmp(seen.ids[i], id, kIdLen) == 0) {
            seen.refresh_of[i] = seen.refresh;  // still in the feed: not the one to give up
            return false;
        }
        // "Longest ago" survives the counter wrapping round (after 4 billion refreshes).
        if (seen.refresh - seen.refresh_of[i] > seen.refresh - seen.refresh_of[oldest]) oldest = i;
    }
    const size_t slot = seen.count < kCapacity ? seen.count++ : oldest;
    strncpy(seen.ids[slot], id, kIdLen - 1);
    seen.ids[slot][kIdLen - 1] = '\0';
    seen.refresh_of[slot] = seen.refresh;
    return true;
}

}  // namespace quake_seen_policy
