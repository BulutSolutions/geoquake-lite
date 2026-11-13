#pragma once

// Time service: the system clock and the RTC hold UTC. Local time comes from one stored time zone
// (NVS "gql_time"/"rec"), UTC until one is known. The clock is learned over SNTP. The zone is learned from the
// internet (api.handeos.com/v1/timezone answers the zone of the caller's internet address and its offset, nothing
// else) unless the user picked one on the Settings page ("Set automatically" off: no lookup is made then).
//
// Calls per firmware:
//   setup():          hande_time_begin("GeoQuake-Lite/<version>")   after Wire.begin, before tasks read the time
//   Wi-Fi connected:  hande_time_on_wifi_up()                      any task; starts SNTP once (UDP, non-blocking)
//   network worker:   hande_time_lookup_zone(deadline)             blocking HTTPS, after the earthquake download
//   UI/loop task:     hande_time_loop()                            writes the RTC after an SNTP answer
//   anywhere:         hande_time_local(&tm)                        false = no valid local time to show

#include <stddef.h>
#include <stdint.h>
#include <time.h>

void hande_time_begin(const char* user_agent);

void hande_time_on_wifi_up();

// The time-zone lookup: blocking HTTPS on the caller's task, which must be the one task that does the firmware's
// HTTPS (two TLS connections at once do not fit in memory). It goes online only when due: "Set automatically" is on
// and no zone was learned in this boot, or 6 h passed since, and the wait after a failed try (1, 2, 4 ... min, at
// most 6 h) is over. It never goes online by itself: a due lookup runs with the next earthquake refresh that calls
// this. Everything, closing the connection included, ends by `deadline_ms` (millis() clock); with less than 20 s
// left it does not start. True when the zone was learned now or earlier in this boot.
bool hande_time_lookup_zone(uint32_t deadline_ms);
bool hande_time_zone_lookup_due();

void hande_time_loop();

bool hande_time_valid();       // the UTC clock is set (RTC or SNTP)
bool hande_time_zone_known();  // a time zone is stored
// Local time now / at a UTC instant: false while the clock or the zone is not known.
bool hande_time_local(struct tm* out);
bool hande_time_local_at(time_t utc, struct tm* out);
// Bumps whenever the clock was set or the zone changed (redraw clocks, re-check night schedules).
uint32_t hande_time_changes();

// The stored IANA zone name ("America/New_York"); "UTC" when none was stored. Writes into buf, returns buf.
const char* hande_time_zone_name(char* buf, size_t len);

// The user's choice on the Settings page: stores `zone` in the same record (NVS "gql_time"/"rec") and applies its
// POSIX rule at once. It switches "Set automatically" off: the internet lookup never replaces a picked zone.
// False when the name is unknown to the table.
bool hande_time_set_zone(const char* zone);

// "Set automatically": on unless the user picked a zone. Switching it on keeps the zone in use until the next
// lookup answers (the caller starts a refresh); switching it off keeps the zone in use as the user's pick.
bool hande_time_zone_automatic();
void hande_time_set_zone_automatic(bool on);
