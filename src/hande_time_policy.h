#pragma once

// Pure rules of the time service (hande_time.cpp), no Arduino: host-tested on the host.
// Contract: the RTC and the system clock hold UTC; the local time comes from one POSIX TZ string that every
// firmware on the board shares (NVS "gql_time"/"rec"), whichever firmware learned it.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "hande_rtc_policy.h"
#include "hande_tz_db.h"

namespace hande_time_policy {

// 2100-01-01 (the RTC's range), or 2038-01-19 where time_t is 32 bits (Arduino 2.0.17's newlib): a larger value
// would turn negative in the time_t cast.
constexpr int64_t kMaxEpoch = sizeof(time_t) >= 8 ? 4102444800LL : 2147483647LL;
constexpr int32_t kMaxOffsetS = 14 * 3600;   // real UTC offsets lie within -12 h .. +14 h

inline bool epoch_valid(int64_t epoch) {
    return epoch >= static_cast<int64_t>(hande_rtc_policy::kValidEpoch) && epoch < kMaxEpoch;
}
inline bool offset_valid(int32_t offset_s) { return offset_s >= -kMaxOffsetS && offset_s <= kMaxOffsetS; }

// The table's POSIX rule for an IANA zone name ("America/New_York"), nullptr when the name is unknown.
inline const char* zone_rule(const char* name) {
    if (!name || !name[0]) return nullptr;
    uint32_t lo = 0, hi = hande_tz_db::kZoneCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const int cmp = strcmp(hande_tz_db::kZones[mid].name, name);
        if (cmp == 0) return hande_tz_db::kRules[hande_tz_db::kZones[mid].rule];
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

// A fixed UTC offset (east positive, seconds) as a POSIX TZ string: +3:30 -> "<+0330>-3:30", -4 h -> "<-04>4",
// 0 -> "UTC0". POSIX counts west as positive, so the sign flips. False when the offset is not a real one.
inline bool fixed_offset_rule(int32_t offset_s, char* out, size_t out_len) {
    if (!out || out_len < 20 || !offset_valid(offset_s) || offset_s % 60 != 0) return false;
    if (offset_s == 0) {
        snprintf(out, out_len, "UTC0");
        return true;
    }
    const char sign = offset_s > 0 ? '+' : '-';
    const int32_t abs_s = offset_s > 0 ? offset_s : -offset_s;
    const int hours = static_cast<int>(abs_s / 3600);
    const int minutes = static_cast<int>((abs_s % 3600) / 60);
    const char* posix_sign = offset_s > 0 ? "-" : "";  // tzdata style: "<-04>4", "<+0330>-3:30"
    if (minutes) snprintf(out, out_len, "<%c%02d%02d>%s%d:%02d", sign, hours, minutes, posix_sign, hours, minutes);
    else snprintf(out, out_len, "<%c%02d>%s%d", sign, hours, posix_sign, hours);
    return true;
}

// UTC offset (east positive) that a local broken-down time `local` of the UTC instant `utc` implies.
inline int32_t offset_of(const struct tm& local, int64_t utc) {
    const int64_t days = hande_rtc_policy::days_from_civil(local.tm_year + 1900, static_cast<uint32_t>(local.tm_mon + 1),
                                                          static_cast<uint32_t>(local.tm_mday));
    const int64_t local_s = days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    return static_cast<int32_t>(local_s - utc);
}

enum class Source : uint8_t { kNone = 0, kNetwork = 1, kBrowser = 2, kMigrated = 3 };

// The shared record (NVS blob). `tz` is what is applied; `zone` is the IANA name when known ("" for a migrated
// offset). Version 1; a record with another version or broken fields is ignored (the next sync writes a new one).
struct Record {
    uint8_t version;
    uint8_t source;
    uint8_t reserved[2];
    int64_t updated_utc;  // when it was learned (0 = unknown, e.g. migrated before the clock was set)
    char zone[48];
    char tz[64];
};
constexpr uint8_t kRecordVersion = 1;

inline bool terminated(const char* s, size_t n) { return memchr(s, '\0', n) != nullptr; }

inline bool record_valid(const Record& r) {
    if (r.version != kRecordVersion || r.source < 1 || r.source > 3) return false;
    if (!terminated(r.zone, sizeof(r.zone)) || !terminated(r.tz, sizeof(r.tz)) || !r.tz[0]) return false;
    for (const char* p = r.tz; *p; ++p) {
        if (*p < 0x21 || *p > 0x7E) return false;  // printable, no spaces: what a POSIX TZ string is made of
    }
    return true;
}

// Does a zone from `incoming` replace what is stored? A network answer always does; a browser zone only when no
// network zone was ever learned (the IP lookup is the automatic source; the browser covers a board that never
// reaches the internet); a migrated offset never replaces anything.
inline bool should_replace(Source stored, Source incoming) {
    if (incoming == Source::kNetwork) return true;
    if (incoming == Source::kBrowser) return stored != Source::kNetwork;
    return stored == Source::kNone;
}

// Older time-zone settings, for the one-time migration (each returns false when none was stored).
// "utc_off": seconds east, 0 = never set.
inline bool migrate_east_seconds(int32_t value, int32_t* offset_s) {
    if (value == 0 || !offset_valid(value)) return false;
    *offset_s = value;
    return true;
}
// "tz-offset": minutes east, 0 = never set (also what was saved without a page visit).
inline bool migrate_east_minutes(int32_t value, int32_t* offset_s) {
    return value != 0 && migrate_east_seconds(value * 60, offset_s);
}
// "timezone_offset_minutes": JavaScript getTimezoneOffset(), minutes WEST, INT16_MAX = never set.
inline bool migrate_west_minutes(int32_t value, int32_t* offset_s) {
    if (value == 0x7FFF) return false;
    const int32_t east = -value * 60;
    if (!offset_valid(east)) return false;
    *offset_s = east;
    return true;
}

}  // namespace hande_time_policy
