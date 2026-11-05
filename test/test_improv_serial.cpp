// Improv serial routing rule (src/improv_serial.h, improv_should_parse): only the first byte of a line may start an
// Improv header ('I'), and the bytes held for a header that turns out not to be one are replayed into the JSON line
// buffer in order. The model here is serial_command_task's decision loop; the packet parser itself is the SDK's.

#include <unity.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "../src/improv_serial.h"

namespace {

// serial_command_task's routing, with `accept_at[i]` standing in for improv_serial_feed_byte()'s answer for the i-th
// byte: true = the byte was taken by Improv. No real packet is fed, so the parser only accepts a lone 'I' and rejects
// the next byte: exactly the "looked like a header, was not one" case.
std::string route_line(const char* text, const std::vector<uint8_t>& accept_at, std::string* consumed) {
    std::string line;
    std::vector<uint8_t> pending;
    size_t index = 0;
    for (const char* p = text; *p; ++p, ++index) {
        const uint8_t byte = static_cast<uint8_t>(*p);
        const bool offered = !pending.empty() || improv_should_parse(line.empty(), byte);
        if (offered && index < accept_at.size() && accept_at[index]) {
            pending.push_back(byte);
            if (consumed) *consumed += static_cast<char>(byte);
            continue;
        }
        if (offered) {  // rejected: replay what was held, then this byte
            for (uint8_t held : pending) line += static_cast<char>(held);
            pending.clear();
        }
        line += static_cast<char>(byte);
    }
    return line;
}

void test_only_a_new_line_may_start_a_packet() {
    TEST_ASSERT_TRUE(improv_should_parse(true, 'I'));
    TEST_ASSERT_FALSE(improv_should_parse(false, 'I'));  // the line already has text: it is JSON, not Improv
    TEST_ASSERT_FALSE(improv_should_parse(true, '{'));   // a JSON command never looks like Improv
    TEST_ASSERT_FALSE(improv_should_parse(true, '\n'));
    TEST_ASSERT_FALSE(improv_should_parse(false, '{'));
}

// The command that must always arrive intact: no byte of it is offered to Improv, so nothing is swallowed.
void test_json_command_arrives_intact() {
    const char* json = "{\"cmd\":\"setState\",\"state\":\"MAIN\"}";
    std::string consumed;
    const std::string line = route_line(json, std::vector<uint8_t>(), &consumed);
    TEST_ASSERT_EQUAL_STRING(json, line.c_str());
    TEST_ASSERT_TRUE(consumed.empty());
}

// A line that starts like a header and turns out not to be one: every held byte comes back, in order.
void test_rejected_prefix_is_replayed_in_order() {
    const char* text = "IMPROVX: not a packet";
    std::string consumed;
    const std::vector<uint8_t> accept = {1, 0};  // 'I' accepted, 'M' rejected
    const std::string line = route_line(text, accept, &consumed);
    TEST_ASSERT_EQUAL_STRING(text, line.c_str());
    TEST_ASSERT_EQUAL_STRING("I", consumed.c_str());
}

// Once a line has started, no later byte is offered: an 'I' inside a command is plain JSON text.
void test_continuation_is_never_offered() {
    std::string consumed;
    const std::vector<uint8_t> accept = {0, 1, 1, 1, 1, 1, 1};  // would accept any 'I' if it were offered
    const std::string line = route_line("{I}mport", accept, &consumed);
    TEST_ASSERT_EQUAL_STRING("{I}mport", line.c_str());
    TEST_ASSERT_TRUE(consumed.empty());
}

}  // namespace

void run_improv_serial_tests() {
    RUN_TEST(test_only_a_new_line_may_start_a_packet);
    RUN_TEST(test_json_command_arrives_intact);
    RUN_TEST(test_rejected_prefix_is_replayed_in_order);
    RUN_TEST(test_continuation_is_never_offered);
}
