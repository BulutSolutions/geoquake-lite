// Time-zone picker split (src/hande_tz_db.h, hande_tz_names::split_zone): region = the part before the first '/',
// city = the rest with '_' shown as a space. Every generated name splits and rebuilds to itself, so the picker
// stores exactly the IANA name the time service looks up its POSIX rule for.

#include <unity.h>
#include <string.h>

#include <string>

#include "../src/hande_tz_db.h"

namespace {

void split(const char* name, std::string& region, std::string& city) {
    char region_buf[48] = {};
    char city_buf[64] = {};
    TEST_ASSERT_TRUE(hande_tz_names::split_zone(name, region_buf, sizeof(region_buf), city_buf, sizeof(city_buf)));
    region = region_buf;
    city = city_buf;
}

void test_region_and_city() {
    std::string region, city;
    split("America/New_York", region, city);
    TEST_ASSERT_EQUAL_STRING("America", region.c_str());
    TEST_ASSERT_EQUAL_STRING("New York", city.c_str());
    split("America/Argentina/Buenos_Aires", region, city);
    TEST_ASSERT_EQUAL_STRING("America", region.c_str());
    TEST_ASSERT_EQUAL_STRING("Argentina/Buenos Aires", city.c_str());
    split("Africa/Abidjan", region, city);
    TEST_ASSERT_EQUAL_STRING("Africa", region.c_str());
    TEST_ASSERT_EQUAL_STRING("Abidjan", city.c_str());
    split("Pacific/Chatham", region, city);
    TEST_ASSERT_EQUAL_STRING("Pacific", region.c_str());
    TEST_ASSERT_EQUAL_STRING("Chatham", city.c_str());
    // No '/': no region, the whole name is the city (the picker's "UTC" entry).
    split("UTC", region, city);
    TEST_ASSERT_EQUAL_STRING("", region.c_str());
    TEST_ASSERT_EQUAL_STRING("UTC", city.c_str());
}

void test_refuses_bad_input() {
    char region[16] = {};
    char city[32] = {};
    TEST_ASSERT_FALSE(hande_tz_names::split_zone(nullptr, region, sizeof(region), city, sizeof(city)));
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("", region, sizeof(region), city, sizeof(city)));
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("America/New_York", nullptr, sizeof(region), city, sizeof(city)));
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("America/New_York", region, sizeof(region), nullptr, sizeof(city)));
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("America/New_York", region, 0, city, sizeof(city)));
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("America/New_York", region, sizeof(region), city, 0));
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("America/New_York", region, 4, city, sizeof(city)));   // "America"
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("America/New_York", region, sizeof(region), city, 4));  // "New York"
    TEST_ASSERT_FALSE(hande_tz_names::split_zone("America/", region, sizeof(region), city, sizeof(city)));
}

// Every name in the table splits; rebuilding region + "/" + city (spaces -> '_') gives the name back, so a pick
// stores a name the table resolves.
void test_every_zone_name_round_trips() {
    for (uint32_t i = 0; i < hande_tz_db::kZoneCount; ++i) {
        const char* name = hande_tz_db::kZones[i].name;
        char region[48] = {};
        char city[48] = {};
        TEST_ASSERT_TRUE(hande_tz_names::split_zone(name, region, sizeof(region), city, sizeof(city)));
        if (!strchr(name, '/')) {
            TEST_ASSERT_EQUAL_STRING("", region);
            TEST_ASSERT_EQUAL_STRING(name, city);
            continue;
        }
        std::string rebuilt(region);
        rebuilt += '/';
        for (const char* c = city; *c; ++c) rebuilt += (*c == ' ') ? '_' : *c;
        TEST_ASSERT_EQUAL_STRING(name, rebuilt.c_str());
    }
}

// The picker's region list: every region it offers has zones in the table (the "UTC" entry is the zone itself).
void test_picker_regions_have_zones() {
    const char* const regions[] = {"Africa", "America", "Asia", "Atlantic",
                                   "Australia", "Europe", "Indian", "Pacific"};
    for (const char* wanted : regions) {
        uint32_t matches = 0;
        for (uint32_t i = 0; i < hande_tz_db::kZoneCount; ++i) {
            char region[48] = {};
            char city[48] = {};
            TEST_ASSERT_TRUE(
                hande_tz_names::split_zone(hande_tz_db::kZones[i].name, region, sizeof(region), city, sizeof(city)));
            if (strcmp(region, wanted) == 0) ++matches;
        }
        TEST_ASSERT_TRUE(matches > 0);
    }
    bool utc = false;
    for (uint32_t i = 0; i < hande_tz_db::kZoneCount; ++i) {
        if (strcmp(hande_tz_db::kZones[i].name, "UTC") == 0) utc = true;
    }
    TEST_ASSERT_TRUE(utc);
}

}  // namespace

void run_hande_tz_tests() {
    RUN_TEST(test_region_and_city);
    RUN_TEST(test_refuses_bad_input);
    RUN_TEST(test_every_zone_name_round_trips);
    RUN_TEST(test_picker_regions_have_zones);
}
