#pragma once
// Quake records shared by the firmware and the native parser tests: no Arduino
// headers here, so src/usgs_parse.cpp builds on the native platform too.

#include <stddef.h>

// Maximum number of earthquakes retained in memory (strictly zero dynamic allocations)
constexpr size_t MAX_STORED_QUAKES = 100;

// Earthquake data structure (Compact POD Layout)
struct Earthquake {
    char id[32];
    float mag;
    char place[128];
    long long time;
    float lat;
    float lon;
    float depth;
};
