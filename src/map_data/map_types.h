// Map data types for GeoQuake
// Common definitions used by all map data files

#pragma once

#include <stdint.h>

// Fixed-point coordinate (lat/lon * 100)
// Range: lat [-9000, 9000] = [-90.00, 90.00]
//        lon [-18000, 18000] = [-180.00, 180.00]
typedef struct {
    int16_t lat;
    int16_t lon;
} MapPoint;

typedef struct {
    const MapPoint* points;
    uint16_t point_count;
    float min_zoom;      // Minimum zoom level to show this polyline
    float max_zoom;      // Maximum zoom level to show this polyline
} MapPolyline;
