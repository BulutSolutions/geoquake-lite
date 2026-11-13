#include <unity.h>
#include "map_mercator.h"
#include "board_profile.h"

BoardProfile g_board = {480, 0, false, false, false, "square216"};

void test_lat_to_mercator_y() {
    // Equator should be at 0.5
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, lat_to_mercator_y(0.0f));

    // North pole (clamped to Mercator limit)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, lat_to_mercator_y(85.0511287798066f));

    // South pole (clamped to Mercator limit)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, lat_to_mercator_y(-85.0511287798066f));

    // Some known values
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.25f, lat_to_mercator_y(66.51326f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.75f, lat_to_mercator_y(-66.51326f));
}

void test_mercator_y_to_lat() {
    // Should be inverse of lat_to_mercator_y
    float lat = 45.0f;
    float y = lat_to_mercator_y(lat);
    float recovered_lat = mercator_y_to_lat(y);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, lat, recovered_lat);

    lat = -30.0f;
    y = lat_to_mercator_y(lat);
    recovered_lat = mercator_y_to_lat(y);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, lat, recovered_lat);
}

void run_projection_equivalence_tests();  // test_projection_equivalence.cpp
void run_qoi_equivalence_tests();         // test_qoi_equivalence.cpp
void run_tile_pak_tests();                // test_tile_pak.cpp
void run_usgs_parse_tests();              // test_usgs_parse.cpp
void run_hande_tz_tests();                // test_hande_tz.cpp
void run_improv_serial_tests();           // test_improv_serial.cpp
void run_wifi_scan_tests();               // test_wifi_scan.cpp
void run_time_policy_tests();             // test_time_policy.cpp
void run_quake_seen_tests();              // test_quake_seen.cpp

void setup() {
    UNITY_BEGIN();

    RUN_TEST(test_lat_to_mercator_y);
    RUN_TEST(test_mercator_y_to_lat);
    run_projection_equivalence_tests();
    run_qoi_equivalence_tests();
    run_tile_pak_tests();
    run_usgs_parse_tests();
    run_hande_tz_tests();
    run_improv_serial_tests();
    run_wifi_scan_tests();
    run_time_policy_tests();
    run_quake_seen_tests();

    UNITY_END();
}

void loop() {
    // Nothing to do
}

int main() {
    setup();
    return 0;
}
