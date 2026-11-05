#pragma once

#include <stddef.h>
#include <stdint.h>

// Improv Wi-Fi over USB serial (improv-wifi/sdk-cpp, pinned in platformio.ini): the browser installer (ESP Web
// Tools) sets the Wi-Fi network right after flashing, so the board never needs a second setup step.
//
// Serial is shared with the JSON command interface and the logs. serial_command_task() may offer a byte here first;
// the Improv parser recognizes its own "IMPROV" header, so a prefix that does not match is handed back to the JSON
// line handling. A complete, valid Improv packet is consumed (never reaches the JSON buffer).

// True when a byte may begin an Improv header: only at the start of a line no JSON text has claimed yet, and only
// for 'I' (the first byte of "IMPROV"; JSON commands start with '{', so a JSON line is never cut). Pure, host-tested.
constexpr bool improv_should_parse(bool line_empty, uint8_t byte) {
    return line_empty && byte == 'I';
}

// Feeds one byte from the USB serial stream. Returns true when the byte belonged to an Improv packet (the packet
// was consumed; do not add the byte to the JSON line buffer), false when the caller should handle it as a JSON
// command byte. Runs on serial_command_task (Core 0): a WIFI_SETTINGS request blocks here while it connects, which
// is safe because the LVGL thread is a different task.
bool improv_serial_feed_byte(uint8_t byte);

// True once after a complete Improv packet was consumed: the caller then drops the bytes it held as a candidate
// header (they were a packet, not JSON text).
bool improv_serial_packet_finished();
