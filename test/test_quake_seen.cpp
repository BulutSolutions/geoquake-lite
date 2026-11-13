// Which earthquakes of a refresh are new (src/quake_seen_policy.h): the alert sound plays for those only.

#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "../src/quake_seen_policy.h"

using namespace quake_seen_policy;

namespace {

Seen s_seen;  // 6.4 KB: not on the stack

// One refresh: `count` earthquakes with ids "<prefix><first>" .. newest first, all of magnitude `mag`, walked from
// the oldest to the newest as the firmware does. Returns how many counted as new.
int refresh(const char* prefix, int first, int count, float mag, float min_mag) {
    int fresh = 0;
    begin_refresh(s_seen);
    for (int i = first; i < first + count; ++i) {  // rising numbers = from the oldest to the newest
        char id[kIdLen];
        snprintf(id, sizeof(id), "%s%d", prefix, i);
        if (note(s_seen, id, mag, min_mag)) ++fresh;
    }
    return fresh;
}

// The fault this replaces: with 100 earthquakes over the filter the same feed counted as new on every refresh.
void test_the_same_feed_is_new_only_once() {
    memset(&s_seen, 0, sizeof(s_seen));
    TEST_ASSERT_EQUAL_INT(100, refresh("us", 0, 100, 1.0f, 0.0f));  // the first fetch (the firmware plays no sound)
    TEST_ASSERT_EQUAL_INT(0, refresh("us", 0, 100, 1.0f, 0.0f));
    TEST_ASSERT_EQUAL_INT(0, refresh("us", 0, 100, 1.0f, 0.0f));
}

// The feed moves on: three new earthquakes push the three oldest out. Only the three count, now and afterwards.
void test_only_the_new_ones_count_when_the_feed_moves_on() {
    memset(&s_seen, 0, sizeof(s_seen));
    refresh("us", 0, 100, 1.0f, 0.0f);
    TEST_ASSERT_EQUAL_INT(3, refresh("us", 3, 100, 1.0f, 0.0f));
    TEST_ASSERT_EQUAL_INT(0, refresh("us", 3, 100, 1.0f, 0.0f));
    // Many refreshes, a few new ones each time: never more than the truly new ones, although ids are forgotten.
    for (int first = 4; first < 900; first += 7) {
        TEST_ASSERT_EQUAL_INT(first == 4 ? 1 : 7, refresh("us", first, 100, 1.0f, 0.0f));
        TEST_ASSERT_EQUAL_INT(0, refresh("us", first, 100, 1.0f, 0.0f));
    }
    TEST_ASSERT_EQUAL_UINT(kCapacity, s_seen.count);
}

void test_the_magnitude_filter() {
    memset(&s_seen, 0, sizeof(s_seen));
    TEST_ASSERT_EQUAL_INT(0, refresh("us", 0, 50, 2.0f, 4.5f));   // under the filter: not new, not remembered
    TEST_ASSERT_EQUAL_UINT(0, s_seen.count);
    TEST_ASSERT_EQUAL_INT(50, refresh("us", 0, 50, 5.0f, 4.5f));  // revised above the filter: they count now
    TEST_ASSERT_EQUAL_INT(0, refresh("us", 0, 50, 5.0f, 4.5f));
    TEST_ASSERT_TRUE(note(s_seen, "exact", 4.5f, 4.5f));          // exactly on the filter passes
}

void test_ids() {
    memset(&s_seen, 0, sizeof(s_seen));
    TEST_ASSERT_FALSE(note(s_seen, nullptr, 5.0f, 0.0f));
    TEST_ASSERT_FALSE(note(s_seen, "", 5.0f, 0.0f));  // no id: never an alert
    const char longest[] = "0123456789012345678901234567890";  // 31 characters: all that an id holds
    TEST_ASSERT_TRUE(note(s_seen, longest, 5.0f, 0.0f));
    TEST_ASSERT_FALSE(note(s_seen, longest, 5.0f, 0.0f));
    TEST_ASSERT_TRUE(note(s_seen, "012345678901234567890123456789", 5.0f, 0.0f));  // one shorter is another id
    TEST_ASSERT_FALSE(note(s_seen, "012345678901234567890123456789", 5.0f, 0.0f));
    TEST_ASSERT_TRUE(note(s_seen, "0123", 5.0f, 0.0f));  // the start of a remembered id is another id
}

// The memory is full of ids that left the feed long ago, and the whole feed is replaced at once: all 100 are new,
// none of them pushes out another one of the same refresh, so the next refresh is silent.
void test_a_full_memory_never_gives_up_an_id_of_the_current_feed() {
    memset(&s_seen, 0, sizeof(s_seen));
    refresh("old", 0, 100, 1.0f, 0.0f);
    refresh("old", 100, 100, 1.0f, 0.0f);
    TEST_ASSERT_EQUAL_UINT(kCapacity, s_seen.count);
    TEST_ASSERT_EQUAL_INT(100, refresh("new", 0, 100, 1.0f, 0.0f));
    TEST_ASSERT_EQUAL_INT(0, refresh("new", 0, 100, 1.0f, 0.0f));
    // The walk order does not matter: the same feed noted from the newest to the oldest is silent too.
    begin_refresh(s_seen);
    for (int i = 99; i >= 0; --i) {
        char id[kIdLen];
        snprintf(id, sizeof(id), "new%d", i);
        TEST_ASSERT_FALSE(note(s_seen, id, 1.0f, 0.0f));
    }
}

// An earthquake that left the feed and comes back: silent while it is remembered, an alert again once 100 or more
// newer ids pushed it out. One that stays in the feed is kept however many refreshes pass.
void test_an_id_that_leaves_the_feed_and_returns() {
    memset(&s_seen, 0, sizeof(s_seen));
    refresh("us", 0, 100, 1.0f, 0.0f);                              // us0..us99
    TEST_ASSERT_EQUAL_INT(50, refresh("us", 50, 100, 1.0f, 0.0f));  // us50..us149: us0..us49 left the feed
    begin_refresh(s_seen);
    TEST_ASSERT_FALSE(note(s_seen, "us7", 1.0f, 0.0f));             // back at once: still remembered, no alert
    TEST_ASSERT_EQUAL_INT(50, refresh("us", 100, 100, 1.0f, 0.0f)); // us100..us199: the memory is full (200)
    TEST_ASSERT_EQUAL_INT(60, refresh("us", 160, 100, 1.0f, 0.0f)); // 60 more: the 60 longest gone are given up
    begin_refresh(s_seen);
    TEST_ASSERT_TRUE(note(s_seen, "us3", 1.0f, 0.0f));              // gone since the second refresh: forgotten
    TEST_ASSERT_FALSE(note(s_seen, "us7", 1.0f, 0.0f));             // seen again in between: kept longer
    TEST_ASSERT_FALSE(note(s_seen, "us180", 1.0f, 0.0f));           // in the current feed: always kept
}

}  // namespace

void run_quake_seen_tests() {
    RUN_TEST(test_the_same_feed_is_new_only_once);
    RUN_TEST(test_only_the_new_ones_count_when_the_feed_moves_on);
    RUN_TEST(test_the_magnitude_filter);
    RUN_TEST(test_ids);
    RUN_TEST(test_a_full_memory_never_gives_up_an_id_of_the_current_feed);
    RUN_TEST(test_an_id_that_leaves_the_feed_and_returns);
}
