// USGS GeoJSON reader (src/usgs_parse.cpp): one feature object at a time, bounded
// newest-100 retention, strict per-feature validation and truncation detection.

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "../src/usgs_parse.h"

#ifndef GQ_PROJECT_DIR
#define GQ_PROJECT_DIR "."
#endif

namespace {

struct MemReader : UsgsReader {
    const std::string* data = nullptr;
    size_t pos = 0;

    int read() override {
        if (pos >= data->size()) return -1;
        return static_cast<unsigned char>((*data)[pos++]);
    }
    size_t readBytes(char* buffer, size_t length) override {
        if (pos >= data->size() || length == 0) return 0;
        size_t n = data->size() - pos;
        if (n > length) n = length;
        memcpy(buffer, data->data() + pos, n);
        pos += n;
        return n;
    }
};

int g_yields = 0;
void count_yield() { ++g_yields; }

std::string read_file(const char* path) {
    std::string out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

std::string fixture_all_day() { return read_file(GQ_PROJECT_DIR "/test/fixtures/usgs_all_day.geojson"); }

// One USGS-shaped feature; place_json == nullptr leaves the place out entirely.
std::string make_feature(const char* id, const char* mag_json, const char* place_json, long long time_ms,
                         const char* coords_json) {
    std::string s = "{\"type\":\"Feature\",\"id\":\"";
    s += id;
    s += "\",\"properties\":{\"mag\":";
    s += mag_json;
    if (place_json) {
        s += ",\"place\":";
        s += place_json;
    }
    s += ",\"time\":";
    s += std::to_string(time_ms);
    s += "},\"geometry\":{\"type\":\"Point\",\"coordinates\":";
    s += coords_json;
    s += "}}";
    return s;
}

std::string make_collection(const std::vector<std::string>& features) {
    std::string s = "{\"type\":\"FeatureCollection\",\"metadata\":{\"count\":";
    s += std::to_string(features.size());
    s += "},\"features\":[";
    for (size_t i = 0; i < features.size(); ++i) {
        if (i) s += ',';
        s += features[i];
    }
    s += "]}";
    return s;
}

// n features, times increasing with the index (feature n-1 is the newest).
std::string make_burst(size_t n, long long base_ms) {
    std::string s = "{\"type\":\"FeatureCollection\",\"features\":[";
    char buf[320];
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ',';
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"Feature\",\"id\":\"burst%zu\",\"properties\":{\"mag\":1.5,"
                 "\"place\":\"Synthetic %zu\",\"time\":%lld},\"geometry\":{\"type\":\"Point\","
                 "\"coordinates\":[10.5,20.5,5.0]}}",
                 i, i, base_ms + static_cast<long long>(i) * 1000);
        s += buf;
    }
    s += "]}";
    return s;
}

UsgsParseResult parse(const std::string& json, Earthquake* out, const UsgsParseLimits& limits = UsgsParseLimits()) {
    MemReader reader;
    reader.data = &json;
    return usgs_parse_features(reader, out, MAX_STORED_QUAKES, limits);
}

const Earthquake* find_by_id(const Earthquake* out, size_t count, const char* id) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(out[i].id, id) == 0) return &out[i];
    }
    return nullptr;
}

int count_by_id(const Earthquake* out, size_t count, const char* id) {
    int n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(out[i].id, id) == 0) ++n;
    }
    return n;
}

void test_usgs_fixture_all_day() {
    const std::string json = fixture_all_day();
    TEST_ASSERT_GREATER_THAN(10000, static_cast<int>(json.size()));

    Earthquake out[MAX_STORED_QUAKES];
    UsgsParseLimits limits;
    g_yields = 0;
    limits.yield_cb = &count_yield;

    const UsgsParseResult res = parse(json, out, limits);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MAX_STORED_QUAKES), static_cast<int>(res.count));
    TEST_ASSERT_GREATER_THAN(0, g_yields);  // 201 features -> the 16-feature hook fired

    for (size_t i = 0; i < res.count; ++i) {
        TEST_ASSERT_TRUE(out[i].time > 0);
        TEST_ASSERT_TRUE(out[i].lat >= -90.0f && out[i].lat <= 90.0f);
        TEST_ASSERT_TRUE(out[i].lon >= -180.0f && out[i].lon <= 180.0f);
        TEST_ASSERT_TRUE(out[i].mag >= -2.0f && out[i].mag <= 12.0f);
        TEST_ASSERT_TRUE(out[i].depth >= -20.0f && out[i].depth <= 800.0f);  // USGS reports negative depths
        TEST_ASSERT_TRUE(out[i].id[0] != '\0');
        TEST_ASSERT_TRUE(out[i].place[0] != '\0');
        TEST_ASSERT_EQUAL_CHAR('\0', out[i].id[sizeof(out[i].id) - 1]);
        TEST_ASSERT_EQUAL_CHAR('\0', out[i].place[sizeof(out[i].place) - 1]);
        if (i + 1 < res.count) TEST_ASSERT_TRUE(out[i].time >= out[i + 1].time);  // newest first
    }
}

void test_usgs_synthetic_burst_keeps_newest_100() {
    const long long base = 1700000000000LL;
    const std::string json = make_burst(3000, base);

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(MAX_STORED_QUAKES), static_cast<int>(res.count));

    // The 100 newest (indices 2900..2999) survive, newest first; the first ones were evicted.
    for (size_t i = 0; i < res.count; ++i) {
        TEST_ASSERT_TRUE(out[i].time == base + static_cast<long long>(2999 - i) * 1000);
    }
    TEST_ASSERT_EQUAL_STRING("Synthetic 2999", out[0].place);
    TEST_ASSERT_EQUAL_STRING("Synthetic 2900", out[99].place);
}

void test_usgs_feature_validation() {
    const long long base = 1700000000000LL;
    const std::string long_place = "\"" + std::string(200, 'x') + "\"";

    std::vector<std::string> features;
    features.push_back(make_feature("nullmag", "null", "\"Null mag\"", base + 1000, "[10,20,5]"));
    features.push_back(make_feature("zeromag", "0.0", "\"Zero mag\"", base + 2000, "[10,20]"));
    features.push_back(make_feature("badlat", "3.0", "\"Bad lat\"", base + 3000, "[10,91,5]"));
    features.push_back(make_feature("badlon", "3.1", "\"Bad lon\"", base + 4000, "[181,20,5]"));
    features.push_back(make_feature("longplace", "2.0", long_place.c_str(), base + 5000, "[10,20,7]"));
    features.push_back(make_feature("noplace", "1.0", nullptr, base + 6000, "[10,20,3]"));

    const std::string json = make_collection(features);
    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(res.count));

    TEST_ASSERT_EQUAL_INT(0, count_by_id(out, res.count, "nullmag"));
    TEST_ASSERT_EQUAL_INT(0, count_by_id(out, res.count, "badlat"));
    TEST_ASSERT_EQUAL_INT(0, count_by_id(out, res.count, "badlon"));

    const Earthquake* zero = find_by_id(out, res.count, "zeromag");
    TEST_ASSERT_NOT_NULL(zero);
    TEST_ASSERT_TRUE(zero->mag == 0.0f);  // a real 0.0 magnitude is kept
    TEST_ASSERT_TRUE(zero->depth == 0.0f);  // no third coordinate -> depth 0

    const Earthquake* lp = find_by_id(out, res.count, "longplace");
    TEST_ASSERT_NOT_NULL(lp);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sizeof(lp->place)) - 1, static_cast<int>(strlen(lp->place)));
    TEST_ASSERT_EQUAL_CHAR('\0', lp->place[sizeof(lp->place) - 1]);
    TEST_ASSERT_EQUAL_CHAR('x', lp->place[0]);

    const Earthquake* np = find_by_id(out, res.count, "noplace");
    TEST_ASSERT_NOT_NULL(np);
    TEST_ASSERT_EQUAL_STRING("Unknown location", np->place);
}

void test_usgs_truncated_stream() {
    const std::string full = make_burst(50, 1700000000000LL);
    std::string cut = full.substr(0, full.size() - 40);  // ends inside the last feature object

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(cut, out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Truncated), static_cast<int>(res.status));
}

void test_usgs_no_features_key() {
    const std::string json = "{\"type\":\"FeatureCollection\",\"metadata\":{\"count\":0}}";
    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::NoFeatures), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(res.count));
}

void test_usgs_too_large() {
    const std::string json = fixture_all_day();
    UsgsParseLimits limits;
    limits.max_bytes = 300;  // smaller than the header + one feature

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out, limits);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::TooLarge), static_cast<int>(res.status));
}

// ---- Non-blocking source (item 1): read() returns -1 on "packet gaps" ----

uint32_t g_fake_now = 0;
uint32_t fake_now() { return g_fake_now; }
void fake_wait() { g_fake_now += 1; }

struct GapSource {
    const std::string* data = nullptr;
    size_t pos = 0;
    int gap_every = 0;  // return -1 before every next byte (a packet gap)
    int gap_left = 0;

    int read() {
        if (gap_left > 0) {
            --gap_left;
            return -1;
        }
        if (pos >= data->size()) return -1;
        if (gap_every > 0) gap_left = gap_every;
        return static_cast<unsigned char>((*data)[pos++]);
    }
    size_t readBytes(char* buffer, size_t length) {
        size_t n = 0;
        while (n < length) {
            const int c = read();
            if (c < 0) break;
            buffer[n++] = static_cast<char>(c);
        }
        return n;
    }
};

void test_usgs_reader_waits_for_packet_gaps() {
    const std::string json = make_burst(50, 1700000000000LL);
    g_fake_now = 0;
    GapSource source;
    source.data = &json;
    source.gap_every = 1;  // -1 before every single byte

    UsgsBlockingReader<GapSource> reader(source, 45000, &fake_now, &fake_wait);
    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = usgs_parse_features(reader, out, MAX_STORED_QUAKES);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(50, static_cast<int>(res.count));
    TEST_ASSERT_FALSE(reader.timed_out());
}

void test_usgs_reader_read_bytes_across_gaps() {
    const std::string data = "abcdefgh";
    g_fake_now = 0;
    GapSource source;
    source.data = &data;
    source.gap_every = 1;

    UsgsBlockingReader<GapSource> reader(source, 45000, &fake_now, &fake_wait);
    char buffer[8];
    TEST_ASSERT_EQUAL_INT(8, static_cast<int>(reader.readBytes(buffer, 8)));
    TEST_ASSERT_TRUE(memcmp(buffer, "abcdefgh", 8) == 0);
}

void test_usgs_reader_idle_timeout_is_eof() {
    const std::string empty;
    g_fake_now = 0;
    GapSource source;
    source.data = &empty;

    UsgsBlockingReader<GapSource> reader(source, 60000, &fake_now, &fake_wait);
    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = usgs_parse_features(reader, out, MAX_STORED_QUAKES);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Timeout), static_cast<int>(res.status));
    TEST_ASSERT_TRUE(reader.timed_out());
    TEST_ASSERT_TRUE(g_fake_now >= 10000);  // 10 s without a byte -> EOF, reported as Timeout
    TEST_ASSERT_TRUE(g_fake_now <= 10020);
}

void test_usgs_reader_total_deadline_cuts_first() {
    const std::string empty;
    g_fake_now = 0;
    GapSource source;
    source.data = &empty;

    UsgsBlockingReader<GapSource> reader(source, 500, &fake_now, &fake_wait);  // deadline before the idle timeout
    Earthquake out[MAX_STORED_QUAKES];
    usgs_parse_features(reader, out, MAX_STORED_QUAKES);

    TEST_ASSERT_TRUE(reader.timed_out());
    TEST_ASSERT_TRUE(g_fake_now >= 500);
    TEST_ASSERT_TRUE(g_fake_now < 10000);
}

// ---- Root-level "features" only (item 2) ----

void test_usgs_features_key_only_at_root() {
    const std::string real = make_feature("real", "2.5", "\"Real one\"", 1700000001000LL, "[10,20,3]");
    const std::string json =
        "{\"type\":\"FeatureCollection\",\"metadata\":{\"title\":\"features\","
        "\"x\":{\"features\":[1]},\"list\":[\"features\"]},\"features\":[" + real + "]}";

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(res.count));
    TEST_ASSERT_EQUAL_STRING("real", out[0].id);
}

void test_usgs_root_value_string_named_features() {
    const std::string real = make_feature("real", "2.5", "\"Real one\"", 1700000002000LL, "[10,20,3]");
    const std::string json = "{\"type\":\"FeatureCollection\",\"title\":\"features\",\"features\":[" + real + "]}";

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(res.count));
    TEST_ASSERT_EQUAL_STRING("real", out[0].id);
}

// ---- Per-feature memory cap (item 4) ----

void test_usgs_oversized_feature_skipped() {
    const std::string huge_place = "\"" + std::string(20000, 'p') + "\"";  // > the 16 KB feature budget
    std::vector<std::string> features;
    features.push_back(make_feature("huge", "1.0", huge_place.c_str(), 1700000000000LL, "[10,20,3]"));
    features.push_back(make_feature("ok", "2.0", "\"Normal\"", 1700000001000LL, "[11,21,4]"));

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(make_collection(features), out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(res.count));
    TEST_ASSERT_EQUAL_STRING("ok", out[0].id);
}

void test_usgs_overlong_id_and_place_skipped() {
    const std::string long_id(100, 'i');  // sane raw id length is 64
    const std::string long_place = "\"" + std::string(300, 'p') + "\"";  // sane place length is 256
    std::vector<std::string> features;
    features.push_back(make_feature(long_id.c_str(), "1.0", "\"Long id\"", 1700000000000LL, "[10,20,3]"));
    features.push_back(make_feature("longplace", "1.1", long_place.c_str(), 1700000001000LL, "[10,20,3]"));
    features.push_back(make_feature("ok", "2.0", "\"Normal\"", 1700000002000LL, "[11,21,4]"));

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(make_collection(features), out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(res.count));
    TEST_ASSERT_EQUAL_STRING("ok", out[0].id);
}

void test_usgs_repeated_over_budget_gives_too_large() {
    const std::string huge_place = "\"" + std::string(40000, 'p') + "\"";
    std::vector<std::string> features;
    for (int i = 0; i < 9; ++i) {
        features.push_back(make_feature("huge", "1.0", huge_place.c_str(), 1700000000000LL + i * 1000, "[10,20,3]"));
    }

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(make_collection(features), out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::TooLarge), static_cast<int>(res.status));
}

// ---- max_features (item 5) ----

void test_usgs_limit_reached() {
    const std::string json = make_burst(50, 1700000000000LL);
    UsgsParseLimits limits;
    limits.max_features = 10;

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out, limits);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::LimitReached), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(10, static_cast<int>(res.count));
}

// ---- Deadline is checked even while data keeps arriving (item 1) ----

void test_usgs_reader_expired_deadline_with_data_is_timeout() {
    const std::string json = make_burst(5, 1700000000000LL);
    g_fake_now = 0;
    GapSource source;
    source.data = &json;
    source.gap_every = 0;  // data is always available

    UsgsBlockingReader<GapSource> reader(source, 0, &fake_now, &fake_wait);  // deadline already passed
    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = usgs_parse_features(reader, out, MAX_STORED_QUAKES);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Timeout), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(res.count));
    TEST_ASSERT_TRUE(reader.timed_out());
}

// ---- "features" whose value is not an array (item 3) ----

void test_usgs_features_value_not_array_is_no_features() {
    const std::string json = "{\"features\":{\"features\":[]}}";
    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(json, out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::NoFeatures), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(res.count));
}

// ---- Over-budget skips only abort when they come in a row (item 4) ----

void test_usgs_over_budget_spread_between_good_features() {
    const std::string huge_place = "\"" + std::string(40000, 'p') + "\"";
    std::vector<std::string> features;
    for (int i = 0; i < 20; ++i) {
        if (i % 2 == 0) {
            features.push_back(make_feature("huge", "1.0", huge_place.c_str(), 1700000000000LL + i * 1000, "[10,20,3]"));
        } else {
            features.push_back(make_feature("good", "2.0", "\"Normal\"", 1700000000000LL + i * 1000, "[11,21,4]"));
        }
    }

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(make_collection(features), out);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(10, static_cast<int>(res.count));
}

// ---- Impossible future times (item 8) ----

void test_usgs_future_time_skipped() {
    std::vector<std::string> features;
    features.push_back(make_feature("future", "1.0", "\"Future\"", 3000000000000LL, "[10,20,3]"));
    features.push_back(make_feature("now", "2.0", "\"Now\"", 1793767710000LL, "[11,21,4]"));
    features.push_back(make_feature("past", "3.0", "\"Past\"", 1793767610000LL, "[12,22,5]"));

    UsgsParseLimits limits;
    limits.now_ms = 1793767710000LL;

    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = parse(make_collection(features), out, limits);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::Ok), static_cast<int>(res.status));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(res.count));
    TEST_ASSERT_EQUAL_INT(0, count_by_id(out, res.count, "future"));
    TEST_ASSERT_NOT_NULL(find_by_id(out, res.count, "now"));
}

// ---- A closed source is a real EOF, not an idle gap (item 3) ----

class ClosedReader : public UsgsBlockingReader<GapSource> {
public:
    ClosedReader(GapSource& source, uint32_t deadline_ms)
        : UsgsBlockingReader<GapSource>(source, deadline_ms, &fake_now, &fake_wait) {}
    bool source_closed() const override { return true; }
};

void test_usgs_closed_source_eofs_without_idle_wait() {
    const std::string empty;
    g_fake_now = 0;
    GapSource source;
    source.data = &empty;

    ClosedReader reader(source, 60000);
    Earthquake out[MAX_STORED_QUAKES];
    const UsgsParseResult res = usgs_parse_features(reader, out, MAX_STORED_QUAKES);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsgsParseStatus::NoFeatures), static_cast<int>(res.status));
    TEST_ASSERT_FALSE(reader.timed_out());
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(g_fake_now));  // no idle wait at all
}

}  // namespace

void run_usgs_parse_tests() {
    RUN_TEST(test_usgs_fixture_all_day);
    RUN_TEST(test_usgs_synthetic_burst_keeps_newest_100);
    RUN_TEST(test_usgs_feature_validation);
    RUN_TEST(test_usgs_truncated_stream);
    RUN_TEST(test_usgs_no_features_key);
    RUN_TEST(test_usgs_too_large);
    RUN_TEST(test_usgs_reader_waits_for_packet_gaps);
    RUN_TEST(test_usgs_reader_read_bytes_across_gaps);
    RUN_TEST(test_usgs_reader_idle_timeout_is_eof);
    RUN_TEST(test_usgs_reader_total_deadline_cuts_first);
    RUN_TEST(test_usgs_features_key_only_at_root);
    RUN_TEST(test_usgs_root_value_string_named_features);
    RUN_TEST(test_usgs_oversized_feature_skipped);
    RUN_TEST(test_usgs_overlong_id_and_place_skipped);
    RUN_TEST(test_usgs_repeated_over_budget_gives_too_large);
    RUN_TEST(test_usgs_limit_reached);
    RUN_TEST(test_usgs_reader_expired_deadline_with_data_is_timeout);
    RUN_TEST(test_usgs_closed_source_eofs_without_idle_wait);
    RUN_TEST(test_usgs_features_value_not_array_is_no_features);
    RUN_TEST(test_usgs_over_budget_spread_between_good_features);
    RUN_TEST(test_usgs_future_time_skipped);
}
