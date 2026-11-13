#pragma once

// Pure rules of the time service (hande_time.cpp), no Arduino: host-tested on the host.
// Contract: the RTC and the system clock hold UTC; the local time comes from one POSIX TZ string that every
// part of the firmware shares (NVS "gql_time"/"rec").

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

// Where the stored zone came from. kUser is the pick on the Settings page ("Set automatically" is off then);
// versions before 1.0.3 had no internet lookup and stored that pick under the same number.
enum class Source : uint8_t { kNone = 0, kNetwork = 1, kUser = 2, kMigrated = 3 };

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

// Does a zone from `incoming` replace what is stored? The user's pick always does, and the internet lookup never
// replaces it; a network answer replaces everything else; a migrated offset only fills an empty record.
inline bool should_replace(Source stored, Source incoming) {
    if (incoming == Source::kUser) return true;
    if (incoming == Source::kNetwork) return stored != Source::kUser;
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

// --- The time-zone service's answer (hande_time.cpp sends a raw HTTP/1.0 request with one hard deadline) ---

// Splits a raw HTTP response into its status code and body (de-chunked if the server chunked it although HTTP/1.0
// forbids that). Returns the status code, 0 when the response is incomplete or malformed: a body shorter or longer
// than its Content-Length, or chunks without the final empty chunk, is a cut-off answer and never a result.
inline int http_answer(const char* raw, size_t len, char* body, size_t capacity) {
    if (!raw || !body || capacity == 0) return 0;
    body[0] = '\0';
    if (len < 12 || strncmp(raw, "HTTP/1.", 7) != 0) return 0;
    int status = 0;
    for (size_t i = 9; i < 12; ++i) {
        if (raw[i] < '0' || raw[i] > '9') return 0;
        status = status * 10 + (raw[i] - '0');
    }
    const char* end = raw + len;
    const char* head_end = nullptr;
    for (const char* p = raw; p + 4 <= end; ++p) {
        if (memcmp(p, "\r\n\r\n", 4) == 0) {
            head_end = p;
            break;
        }
    }
    if (!head_end) return 0;
    bool chunked = false;
    for (const char* p = raw; p + 26 <= head_end; ++p) {  // "transfer-encoding: chunked", any case
        if ((p[0] | 0x20) == 't' && strncasecmp(p, "transfer-encoding:", 18) == 0) {
            const char* v = p + 18;
            while (v < head_end && *v == ' ') ++v;
            chunked = head_end - v >= 7 && strncasecmp(v, "chunked", 7) == 0;
            break;
        }
    }
    const char* p = head_end + 4;
    size_t used = 0;
    if (!chunked) {
        const size_t n = static_cast<size_t>(end - p);
        for (const char* h = raw; h + 17 <= head_end; ++h) {  // "\r\ncontent-length:", any case
            if (h[0] != '\r' || h[1] != '\n' || strncasecmp(h + 2, "content-length:", 15) != 0) continue;
            const char* v = h + 17;
            while (v < head_end && *v == ' ') ++v;
            size_t declared = 0;
            int digits = 0;
            for (; v < head_end && *v >= '0' && *v <= '9'; ++v) {
                declared = declared * 10 + static_cast<size_t>(*v - '0');
                if (++digits > 6) return 0;
            }
            while (v < head_end && *v == ' ') ++v;
            if (!digits || (v != head_end && *v != '\r') || declared != n) return 0;  // "2junk" is no length
            break;
        }
        if (n + 1 > capacity) return 0;
        memcpy(body, p, n);
        body[n] = '\0';
        return status;
    }
    for (;;) {
        size_t size = 0;
        int digits = 0;
        while (p < end && ((*p >= '0' && *p <= '9') || ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'f'))) {
            size = size * 16 + static_cast<size_t>(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
            ++p;
            if (++digits > 6) return 0;
        }
        if (!digits) return 0;
        while (p < end && *p != '\r') ++p;  // chunk extensions
        if (end - p < 2 || p[1] != '\n') return 0;
        p += 2;
        if (size == 0) return (end - p == 2 && p[0] == '\r' && p[1] == '\n') ? status : 0;  // "0\r\n\r\n", no trailers
        if (static_cast<size_t>(end - p) < size + 2 || used + size + 1 > capacity) return 0;
        memcpy(body + used, p, size);
        used += size;
        body[used] = '\0';
        p += size;
        if (p[0] != '\r' || p[1] != '\n') return 0;
        p += 2;
    }
}

// {"timezone":"America/New_York","utc_offset_sec":-14400} -> zone + offset. Strict: the whole text is one flat JSON
// object (white space around and inside it is fine) whose values are plain strings or integers. "timezone" must be
// a string of at most 47 characters of [A-Za-z0-9/_+-] and "utc_offset_sec" an integer that is a real offset, each
// exactly once. Everything else is refused: a cut-off answer, a nested value, text after the object.
inline bool zone_answer(const char* json, char* zone, size_t zone_capacity, int32_t* offset_s) {
    if (!json || !zone || zone_capacity < 2 || !offset_s) return false;
    const char* p = json;
    const auto skip_space = [&p]() {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    };
    // A plain string (no escapes, no control characters); p ends after the closing quote.
    const auto plain_string = [&p](const char** begin, size_t* len) {
        if (*p != '"') return false;
        const char* start = ++p;
        while (*p && *p != '"') {
            if (*p == '\\' || static_cast<unsigned char>(*p) < 0x20) return false;
            ++p;
        }
        if (*p != '"') return false;
        *begin = start;
        *len = static_cast<size_t>(p - start);
        ++p;
        return true;
    };
    char zone_found[48] = "";
    int32_t offset_found = 0;
    bool have_zone = false;
    bool have_offset = false;
    skip_space();
    if (*p++ != '{') return false;
    for (;;) {
        skip_space();
        const char* key = nullptr;
        size_t key_len = 0;
        if (!plain_string(&key, &key_len)) return false;
        const bool is_zone = key_len == 8 && memcmp(key, "timezone", 8) == 0;
        const bool is_offset = key_len == 14 && memcmp(key, "utc_offset_sec", 14) == 0;
        skip_space();
        if (*p++ != ':') return false;
        skip_space();
        if (*p == '"') {
            const char* value = nullptr;
            size_t n = 0;
            if (!plain_string(&value, &n) || is_offset) return false;
            if (is_zone) {
                if (have_zone || n == 0 || n > 47 || n + 1 > zone_capacity) return false;
                for (size_t i = 0; i < n; ++i) {
                    const char c = value[i];
                    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                    c == '/' || c == '_' || c == '+' || c == '-';
                    if (!ok) return false;
                }
                memcpy(zone_found, value, n);
                zone_found[n] = '\0';
                have_zone = true;
            }
        } else {  // an integer; true, false, null, fractions, objects and arrays are not what the service sends
            const bool negative = *p == '-';
            if (negative) ++p;
            int64_t value = 0;
            int digits = 0;
            while (*p >= '0' && *p <= '9') {
                value = value * 10 + (*p++ - '0');
                if (++digits > 6) return false;
            }
            if (!digits || is_zone) return false;
            if (is_offset) {
                if (negative) value = -value;
                if (have_offset || !offset_valid(static_cast<int32_t>(value))) return false;
                offset_found = static_cast<int32_t>(value);
                have_offset = true;
            }
        }
        skip_space();
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p++ != '}') return false;
        break;
    }
    skip_space();
    if (*p != '\0' || !have_zone || !have_offset) return false;
    strcpy(zone, zone_found);  // fits: checked against zone_capacity above
    *offset_s = offset_found;
    return true;
}

}  // namespace hande_time_policy
