#pragma once

// Time service: the system clock and the RTC hold UTC. Local time comes from one stored time zone
// (NVS "gql_time"/"rec"), default UTC. The clock is learned over SNTP.
//
// Calls per firmware:
//   setup():          hande_time_begin("GeoQuake-Lite/<version>")   after Wire.begin, before tasks read the time
//   Wi-Fi connected:  hande_time_on_wifi_up()                      any task; starts SNTP once (UDP, non-blocking)
//   UI/loop task:     hande_time_loop()                            writes the RTC after an SNTP answer
//   anywhere:         hande_time_local(&tm)                        false = no valid local time to show

#include <stddef.h>
#include <stdint.h>
#include <time.h>

void hande_time_begin(const char* user_agent);

void hande_time_on_wifi_up();

// A web page's browser clock and IANA zone (Intl.DateTimeFormat().resolvedOptions().timeZone), for a board that
// never reaches the internet. The clock is taken only while SNTP has not answered since boot; the zone only while
// none is stored. Either argument may be "absent" (utc_s 0 / zone nullptr).
void hande_time_set_from_browser(int64_t utc_s, const char* zone);

void hande_time_loop();

bool hande_time_valid();       // the UTC clock is set (RTC, SNTP or a browser)
bool hande_time_zone_known();  // a time zone is stored
// Local time now / at a UTC instant: false while the clock or the zone is not known.
bool hande_time_local(struct tm* out);
bool hande_time_local_at(time_t utc, struct tm* out);
// Bumps whenever the clock was set or the zone changed (redraw clocks, re-check night schedules).
uint32_t hande_time_changes();

// The stored IANA zone name ("America/New_York"); "UTC" when none was stored. Writes into buf, returns buf.
const char* hande_time_zone_name(char* buf, size_t len);

// The user's choice on the Settings page: stores `zone` in the same record (NVS "gql_time"/"rec") and applies its
// POSIX rule at once. Always replaces, also a zone learned from the network (the user saw it and picked another).
// False when the name is unknown to the table.
bool hande_time_set_zone(const char* zone);