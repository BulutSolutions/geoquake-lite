#pragma once

// Pure rules of the Improv Wi-Fi scan answer (src/improv_serial.cpp for the packets, src/wifi_manager.cpp for the
// scan): hidden/empty SSIDs are skipped, each SSID appears once (its strongest sighting), the list is strongest
// first and holds at most 20 networks. No Arduino: host-tested in test/test_wifi_scan.cpp.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace wifi_scan_policy {

constexpr size_t kMaxNetworks = 20;     // what the Improv result list shows (the browser scrolls this list)
constexpr size_t kMaxSsidBytes = 32;    // 802.11 SSID limit (no NUL when full)

struct Network {
    char ssid[kMaxSsidBytes + 1];
    int32_t rssi;
    bool secured;
};

inline bool ssid_valid(const char* ssid) {
    if (!ssid || !ssid[0]) return false;  // hidden network: the driver reports an empty SSID
    return strlen(ssid) <= kMaxSsidBytes;
}

// `in` in any order (the driver's scan order) -> `out`: one entry per SSID (its strongest sighting), strongest
// first, at most min(capacity, kMaxNetworks). Returns the number of entries written.
inline size_t select_networks(const Network* in, size_t count, Network* out, size_t capacity) {
    if (!in || !out || capacity == 0) return 0;
    if (capacity > kMaxNetworks) capacity = kMaxNetworks;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const Network& candidate = in[i];
        if (!ssid_valid(candidate.ssid)) continue;

        // Dedupe first: only the strongest sighting of an SSID may be listed (the first of equal strengths), so a
        // stronger duplicate that arrives later still wins and lands in the right place after the sort.
        bool strongest = true;
        for (size_t j = 0; j < count && strongest; ++j) {
            if (j == i || strcmp(in[j].ssid, candidate.ssid) != 0) continue;
            if (in[j].rssi > candidate.rssi || (in[j].rssi == candidate.rssi && j < i)) strongest = false;
        }
        if (!strongest) continue;

        // Then sort (insert in place, strongest first) and cut to the limit by dropping the weakest.
        if (used == capacity && candidate.rssi <= out[used - 1].rssi) continue;
        if (used == capacity) --used;
        size_t pos = used;
        while (pos > 0 && out[pos - 1].rssi < candidate.rssi) {
            out[pos] = out[pos - 1];
            --pos;
        }
        snprintf(out[pos].ssid, sizeof(out[pos].ssid), "%s", candidate.ssid);
        out[pos].rssi = candidate.rssi;
        out[pos].secured = candidate.secured;
        ++used;
    }
    return used;
}

}  // namespace wifi_scan_policy
