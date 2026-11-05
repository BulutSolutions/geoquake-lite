#include "map_mercator.h"

static constexpr float DEG_TO_RAD_F = 0.017453292519943295f; // M_PI / 180.0f
static constexpr float RAD_TO_DEG_F = 57.29577951308232f;    // 180.0f / M_PI
static constexpr float INV_PI_F     = 0.3183098861837907f;    // 1.0f / M_PI
static constexpr float PI_4_F       = 0.7853981633974483f;    // M_PI / 4.0f

static float clamp_latitude_to_mercator(float lat) {
    constexpr float MERCATOR_MAX_LAT = 85.0511287798066f;
    if (lat > MERCATOR_MAX_LAT) return MERCATOR_MAX_LAT;
    if (lat < -MERCATOR_MAX_LAT) return -MERCATOR_MAX_LAT;
    return lat;
}

float lat_to_mercator_y(float lat) {
    float clamped_lat = clamp_latitude_to_mercator(lat);
    float lat_rad = clamped_lat * DEG_TO_RAD_F;
    float y = 0.5f * (1.0f - logf(tanf(PI_4_F + lat_rad * 0.5f)) * INV_PI_F);
    return fmaxf(0.0f, fminf(1.0f, y));
}

float mercator_y_to_lat(float y) {
    return (2.0f * atanf(expf((1.0f - 2.0f * y) * 3.141592653589793f)) - 1.5707963267948966f) * RAD_TO_DEG_F;
}
