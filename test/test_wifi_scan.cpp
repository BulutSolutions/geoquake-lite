// Improv Wi-Fi scan answer rules (src/wifi_scan_policy.h): empty/hidden SSIDs are skipped, each SSID is listed once
// (its strongest sighting), strongest first, at most 20 networks. The scan itself lives in wifi_manager.

#include <unity.h>
#include <string.h>

#include "../src/wifi_scan_policy.h"

namespace {

using wifi_scan_policy::Network;
using wifi_scan_policy::select_networks;

Network make(const char* ssid, int32_t rssi, bool secured = true) {
    Network n = {};
    snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    n.rssi = rssi;
    n.secured = secured;
    return n;
}

void test_empty_and_hidden_skipped() {
    const Network in[] = {make("", -40), make("Home", -70), make("", -30)};
    Network out[4] = {};
    const size_t count = select_networks(in, 3, out, 4);
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_STRING("Home", out[0].ssid);
}

void test_strongest_first() {
    const Network in[] = {make("Weak", -85), make("Strong", -40), make("Middle", -65)};
    Network out[4] = {};
    const size_t count = select_networks(in, 3, out, 4);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_STRING("Strong", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Middle", out[1].ssid);
    TEST_ASSERT_EQUAL_STRING("Weak", out[2].ssid);
    TEST_ASSERT_EQUAL_INT32(-85, out[2].rssi);
}

// The same SSID seen twice (two access points, or a mesh band): one entry, the stronger signal, strongest first.
void test_duplicate_ssid_keeps_strongest() {
    const Network in[] = {make("Home", -80, true), make("Office", -50, false), make("Home", -55, true)};
    Network out[4] = {};
    const size_t count = select_networks(in, 3, out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_STRING("Office", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Home", out[1].ssid);
    TEST_ASSERT_EQUAL_INT32(-55, out[1].rssi);
    TEST_ASSERT_FALSE(out[0].secured);  // the surviving sighting keeps its own security flag
    TEST_ASSERT_TRUE(out[1].secured);
}

// A duplicate that is already listed stays a duplicate when the list is full.
void test_duplicate_recognised_when_full() {
    Network in[wifi_scan_policy::kMaxNetworks + 1];
    for (size_t i = 0; i < wifi_scan_policy::kMaxNetworks; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "Net%02u", static_cast<unsigned>(i));
        in[i] = make(name, -40 - static_cast<int32_t>(i));
    }
    in[wifi_scan_policy::kMaxNetworks] = make("Net00", -90);  // weaker repeat of an entry already in the list
    Network out[wifi_scan_policy::kMaxNetworks + 1] = {};
    const size_t count = select_networks(in, wifi_scan_policy::kMaxNetworks + 1, out,
                                         wifi_scan_policy::kMaxNetworks + 1);
    TEST_ASSERT_EQUAL_UINT32(wifi_scan_policy::kMaxNetworks, count);
    TEST_ASSERT_EQUAL_STRING("Net00", out[0].ssid);
    TEST_ASSERT_EQUAL_INT32(-40, out[0].rssi);
}

void test_limit_is_twenty_strongest() {
    Network in[26];
    for (size_t i = 0; i < 26; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "Net%02u", static_cast<unsigned>(i));
        in[i] = make(name, -30 - static_cast<int32_t>(i));  // Net00 strongest, Net25 weakest
    }
    Network out[32] = {};
    const size_t count = select_networks(in, 26, out, 32);
    TEST_ASSERT_EQUAL_UINT32(wifi_scan_policy::kMaxNetworks, count);
    TEST_ASSERT_EQUAL_STRING("Net00", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Net19", out[wifi_scan_policy::kMaxNetworks - 1].ssid);
    for (size_t i = 1; i < count; ++i) TEST_ASSERT_TRUE(out[i - 1].rssi >= out[i].rssi);
}

// A caller that asks for fewer entries (a small array) gets only that many, still the strongest ones.
void test_smaller_capacity() {
    const Network in[] = {make("A", -80), make("B", -45), make("C", -60)};
    Network out[2] = {};
    const size_t count = select_networks(in, 3, out, 2);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_STRING("B", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("C", out[1].ssid);
}

// A duplicate that arrives later with a stronger signal must be deduped to that sighting AND sorted into its
// place (dedupe -> sort -> cut), also when the list is capped below the number of networks.
void test_late_stronger_duplicate_crosses() {
    const Network in[] = {make("A", -90), make("B", -60), make("C", -50), make("A", -30), make("D", -70)};
    Network out[8] = {};
    const size_t count = select_networks(in, 5, out, 8);
    TEST_ASSERT_EQUAL_UINT32(4, count);
    TEST_ASSERT_EQUAL_STRING("A", out[0].ssid);
    TEST_ASSERT_EQUAL_INT32(-30, out[0].rssi);
    TEST_ASSERT_EQUAL_STRING("C", out[1].ssid);
    TEST_ASSERT_EQUAL_STRING("B", out[2].ssid);
    TEST_ASSERT_EQUAL_STRING("D", out[3].ssid);

    Network small[2] = {};
    const size_t small_count = select_networks(in, 5, small, 2);
    TEST_ASSERT_EQUAL_UINT32(2, small_count);
    TEST_ASSERT_EQUAL_STRING("A", small[0].ssid);
    TEST_ASSERT_EQUAL_INT32(-30, small[0].rssi);
    TEST_ASSERT_EQUAL_STRING("C", small[1].ssid);
}

}  // namespace

void run_wifi_scan_tests() {
    RUN_TEST(test_empty_and_hidden_skipped);
    RUN_TEST(test_strongest_first);
    RUN_TEST(test_duplicate_ssid_keeps_strongest);
    RUN_TEST(test_duplicate_recognised_when_full);
    RUN_TEST(test_late_stronger_duplicate_crosses);
    RUN_TEST(test_limit_is_twenty_strongest);
    RUN_TEST(test_smaller_capacity);
}
