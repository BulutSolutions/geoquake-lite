// Rules of the time service (src/hande_time_policy.h): which source of a time zone replaces which, the stored
// record, and the answer of the time-zone lookup (raw HTTP and its JSON), all without a board.

#include <unity.h>
#include <string.h>

#include "../src/hande_time_policy.h"

using namespace hande_time_policy;

namespace {

// The user's pick on the Settings page always wins and the internet lookup never replaces it ("Set automatically"
// is off then); without a pick the lookup replaces whatever is stored.
void test_which_zone_source_replaces_which() {
    TEST_ASSERT_TRUE(should_replace(Source::kNone, Source::kNetwork));
    TEST_ASSERT_TRUE(should_replace(Source::kNetwork, Source::kNetwork));
    TEST_ASSERT_TRUE(should_replace(Source::kMigrated, Source::kNetwork));
    TEST_ASSERT_FALSE(should_replace(Source::kUser, Source::kNetwork));

    TEST_ASSERT_TRUE(should_replace(Source::kNone, Source::kUser));
    TEST_ASSERT_TRUE(should_replace(Source::kNetwork, Source::kUser));
    TEST_ASSERT_TRUE(should_replace(Source::kUser, Source::kUser));
    TEST_ASSERT_TRUE(should_replace(Source::kMigrated, Source::kUser));

    TEST_ASSERT_TRUE(should_replace(Source::kNone, Source::kMigrated));
    TEST_ASSERT_FALSE(should_replace(Source::kNetwork, Source::kMigrated));
    TEST_ASSERT_FALSE(should_replace(Source::kUser, Source::kMigrated));
}

// A zone picked with 1.0.2 or older was stored as source 2: it must still read as the user's pick.
void test_a_pick_stored_by_an_older_version_stays_a_pick() {
    TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(Source::kUser));
    Record r = {};
    r.version = kRecordVersion;
    r.source = 2;
    strcpy(r.zone, "Europe/Istanbul");
    strcpy(r.tz, "<+03>-3");
    TEST_ASSERT_TRUE(record_valid(r));
    TEST_ASSERT_FALSE(should_replace(static_cast<Source>(r.source), Source::kNetwork));
}

void test_record_checks() {
    Record r = {};
    r.version = kRecordVersion;
    r.source = static_cast<uint8_t>(Source::kNetwork);
    strcpy(r.zone, "America/New_York");
    strcpy(r.tz, "EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_TRUE(record_valid(r));
    Record bad = r;
    bad.version = 2;
    TEST_ASSERT_FALSE(record_valid(bad));
    bad = r;
    bad.tz[0] = '\0';
    TEST_ASSERT_FALSE(record_valid(bad));
    bad = r;
    memset(bad.tz, 'A', sizeof(bad.tz));  // not terminated
    TEST_ASSERT_FALSE(record_valid(bad));
    bad = r;
    strcpy(bad.tz, "EST 5");
    TEST_ASSERT_FALSE(record_valid(bad));
    bad = r;
    bad.source = 0;
    TEST_ASSERT_FALSE(record_valid(bad));
    bad.source = 9;
    TEST_ASSERT_FALSE(record_valid(bad));
}

void test_zone_rules_and_fixed_offsets() {
    TEST_ASSERT_NOT_NULL(zone_rule("America/New_York"));
    TEST_ASSERT_NOT_NULL(zone_rule("UTC"));
    TEST_ASSERT_NULL(zone_rule("Mars/Olympus"));
    TEST_ASSERT_NULL(zone_rule(""));
    TEST_ASSERT_NULL(zone_rule(nullptr));
    char buf[24];
    TEST_ASSERT_TRUE(fixed_offset_rule(3 * 3600 + 1800, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("<+0330>-3:30", buf);
    TEST_ASSERT_TRUE(fixed_offset_rule(-4 * 3600, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("<-04>4", buf);
    TEST_ASSERT_TRUE(fixed_offset_rule(0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("UTC0", buf);
    TEST_ASSERT_FALSE(fixed_offset_rule(15 * 3600, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(fixed_offset_rule(3600 + 7, buf, sizeof(buf)));
}

// The lookup's answer as it arrives: raw HTTP (plain, chunked with a chunk boundary inside the number, cut short,
// an error status) turned into a status and a body.
void test_http_answer() {
    char body[256];
    const char plain[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
                         "{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-14400}";
    TEST_ASSERT_EQUAL_INT(200, http_answer(plain, sizeof(plain) - 1, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-14400}", body);

    const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                           "35\r\n{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-1440\r\n"
                           "2\r\n0}\r\n0\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(200, http_answer(chunked, sizeof(chunked) - 1, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-14400}", body);

    const char cut_short[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                             "35\r\n{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-1440\r\n";
    TEST_ASSERT_EQUAL_INT(0, http_answer(cut_short, sizeof(cut_short) - 1, body, sizeof(body)));
    const char no_head_end[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
    TEST_ASSERT_EQUAL_INT(0, http_answer(no_head_end, sizeof(no_head_end) - 1, body, sizeof(body)));
    const char unavailable[] = "HTTP/1.1 503 Service Unavailable\r\n\r\n{}";
    TEST_ASSERT_EQUAL_INT(503, http_answer(unavailable, sizeof(unavailable) - 1, body, sizeof(body)));
    TEST_ASSERT_EQUAL_INT(0, http_answer("garbage", 7, body, sizeof(body)));
    TEST_ASSERT_EQUAL_INT(0, http_answer(nullptr, 0, body, sizeof(body)));
    char small[8];  // a body that does not fit is refused, never cut
    TEST_ASSERT_EQUAL_INT(0, http_answer(plain, sizeof(plain) - 1, small, sizeof(small)));

    // What the service sends: a Content-Length. A body cut short (the connection dropped, the deadline came) or
    // longer than declared is no answer.
    const char sized[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 55\r\n"
                         "Connection: close\r\n\r\n{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-18000}";
    TEST_ASSERT_EQUAL_INT(200, http_answer(sized, sizeof(sized) - 1, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-18000}", body);
    for (size_t cut = 1; cut <= 55; ++cut) {
        TEST_ASSERT_EQUAL_INT(0, http_answer(sized, sizeof(sized) - 1 - cut, body, sizeof(body)));
    }
    const char too_long[] = "HTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\n{}x";
    TEST_ASSERT_EQUAL_INT(0, http_answer(too_long, sizeof(too_long) - 1, body, sizeof(body)));
    const char no_number[] = "HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n{}";
    TEST_ASSERT_EQUAL_INT(0, http_answer(no_number, sizeof(no_number) - 1, body, sizeof(body)));
    const char empty_ok[] = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(204, http_answer(empty_ok, sizeof(empty_ok) - 1, body, sizeof(body)));
    const char junk_length[] = "HTTP/1.1 200 OK\r\nContent-Length: 2junk\r\n\r\n{}";
    TEST_ASSERT_EQUAL_INT(0, http_answer(junk_length, sizeof(junk_length) - 1, body, sizeof(body)));
    const char spaced_length[] = "HTTP/1.1 200 OK\r\nContent-Length:  2 \r\nX-Other: 1\r\n\r\n{}";
    TEST_ASSERT_EQUAL_INT(200, http_answer(spaced_length, sizeof(spaced_length) - 1, body, sizeof(body)));
    const char last_header[] = "HTTP/1.1 200 OK\r\nX-Other: 1\r\nContent-Length: 2\r\n\r\n{}";
    TEST_ASSERT_EQUAL_INT(200, http_answer(last_header, sizeof(last_header) - 1, body, sizeof(body)));

    // Chunked: the last chunk must be followed by the final empty line; without it the answer is cut off.
    const char no_final_line[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n";
    TEST_ASSERT_EQUAL_INT(0, http_answer(no_final_line, sizeof(no_final_line) - 1, body, sizeof(body)));
    const char whole_chunks[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n";
    TEST_ASSERT_EQUAL_INT(200, http_answer(whole_chunks, sizeof(whole_chunks) - 1, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("{}", body);
    const char after_chunks[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\nmore";
    TEST_ASSERT_EQUAL_INT(0, http_answer(after_chunks, sizeof(after_chunks) - 1, body, sizeof(body)));
}

// The JSON of the answer, read strictly: a zone name of plain characters and an offset that is a real one.
void test_zone_answer() {
    char zone[48];
    int32_t off = 0;
    TEST_ASSERT_TRUE(zone_answer("{\"timezone\":\"Asia/Kolkata\",\"utc_offset_sec\":19800}", zone, sizeof(zone), &off));
    TEST_ASSERT_EQUAL_STRING("Asia/Kolkata", zone);
    TEST_ASSERT_EQUAL_INT32(19800, off);
    TEST_ASSERT_TRUE(zone_answer("{\"utc_offset_sec\": 0, \"timezone\": \"Etc/UTC\"}", zone, sizeof(zone), &off));
    TEST_ASSERT_EQUAL_INT32(0, off);
    TEST_ASSERT_TRUE(zone_answer("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-14400}", zone, sizeof(zone), &off));
    TEST_ASSERT_EQUAL_INT32(-14400, off);

    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-1440", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":\"x\"}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Bad zone\",\"utc_offset_sec\":0}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":99999}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"America/New_York\"}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"error\":\"timezone_unavailable\"}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer(nullptr, zone, sizeof(zone), &off));

    // Only a whole object counts: every cut-off form of a good answer is refused, also one that ends right after a
    // complete-looking number.
    const char good[] = "{\"timezone\":\"America/New_York\",\"utc_offset_sec\":-18000}";
    TEST_ASSERT_TRUE(zone_answer(good, zone, sizeof(zone), &off));
    char cut[sizeof(good)];
    for (size_t n = 0; n + 1 < sizeof(good) - 1; ++n) {
        memcpy(cut, good, n);
        cut[n] = '\0';
        TEST_ASSERT_FALSE(zone_answer(cut, zone, sizeof(zone), &off));
    }
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0 ", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"utc_offset_sec\":0 ,\"timezone\":\"Etc/UTC\"", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0}", zone, sizeof(zone), &off));
    TEST_ASSERT_TRUE(zone_answer(" {\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0}\r\n", zone, sizeof(zone), &off));

    // One flat object and nothing after it: a missing outer brace hidden behind a nested value, two objects, a
    // field given twice, values of another kind.
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0,\"extra\":{}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0,\"extra\":{}}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0}{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0}",
                                  zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0} x", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"timezone\":\"Asia/Tokyo\",\"utc_offset_sec\":0}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0,\"utc_offset_sec\":3600}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":0,}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":1.5}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":+3600}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc/UTC\",\"utc_offset_sec\":null}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":3,\"utc_offset_sec\":0}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"Etc\\/UTC\",\"utc_offset_sec\":0}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("{}", zone, sizeof(zone), &off));
    TEST_ASSERT_FALSE(zone_answer("", zone, sizeof(zone), &off));
    // Another flat field next to the two is fine (the service may grow), in any order, with white space.
    TEST_ASSERT_TRUE(zone_answer("{ \"city\" : \"x\" , \"utc_offset_sec\" : -3600 ,\n \"n\":7, \"timezone\" : \"Atlantic/Azores\" }",
                                 zone, sizeof(zone), &off));
    TEST_ASSERT_EQUAL_STRING("Atlantic/Azores", zone);
    TEST_ASSERT_EQUAL_INT32(-3600, off);
    // The caller's buffer is never overrun and stays untouched on a refusal.
    char tiny[8] = "keep";
    TEST_ASSERT_FALSE(zone_answer("{\"timezone\":\"America/New_York\",\"utc_offset_sec\":0}", tiny, sizeof(tiny), &off));
    TEST_ASSERT_EQUAL_STRING("keep", tiny);
}

}  // namespace

void run_time_policy_tests() {
    RUN_TEST(test_which_zone_source_replaces_which);
    RUN_TEST(test_a_pick_stored_by_an_older_version_stays_a_pick);
    RUN_TEST(test_record_checks);
    RUN_TEST(test_zone_rules_and_fixed_offsets);
    RUN_TEST(test_http_answer);
    RUN_TEST(test_zone_answer);
}
