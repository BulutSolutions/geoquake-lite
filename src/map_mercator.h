#pragma once

#ifndef MAP_MERCATOR_H
#define MAP_MERCATOR_H

#include "config_constants.h"
#include <cmath>

// Web Mercator latitude <-> normalized Y. Screen placement of tiles and dots is
// integer (map_projection.h); these two serve the float paths in map_renderer.cpp,
// map_tiles.cpp and map_projection.cpp.

// Convert latitude to Mercator Y coordinate (0.0 to 1.0)
float lat_to_mercator_y(float lat);

// Convert Mercator Y coordinate back to latitude
float mercator_y_to_lat(float y);

#endif // MAP_MERCATOR_H
