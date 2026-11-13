#include "hande_time.h"

#include <Arduino.h>
#if defined(HANDE_SIM)
#include <HTTPClient.h>
#endif
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>

#include "hande_rtc.h"
#include "hande_time_policy.h"
#include "hande_tls_client.h"
#include "handeos_root_cas.h"

using namespace hande_time_policy;

namespace {

constexpr const char* kNamespace = "gql_time";
constexpr const char* kRecordKey = "rec";
constexpr const char* kZoneHost = "api.handeos.com";  // GET /v1/timezone -> {"timezone":"...","utc_offset_sec":N}
constexpr const char* kZonePath = "/v1/timezone";
// The caller's deadline bounds the lookup: DNS 3 s + TCP connect 3 s + TLS handshake + request write 3 s
// (ssl_client sets SO_SNDTIMEO to the connect timeout); reading stops 3 s before the deadline, which is what
// client.stop() may need (its FIN can wait SO_SNDTIMEO under memory pressure). The handshake gets what is left,
// at most 20 s: it runs at idle priority next to the tile decoders, about 2 s on a quiet board.
constexpr uint32_t kZoneStepTimeoutMs = 3000;
constexpr uint32_t kZoneHandshakeMaxS = 20;
constexpr uint32_t kZoneHandshakeMinS = 8;
constexpr uint32_t kZoneMinBudgetMs = 4 * kZoneStepTimeoutMs + kZoneHandshakeMinS * 1000;
constexpr uint32_t kZoneRefreshMs = 6UL * 3600000UL;  // a zone learned in this boot is asked again after 6 h
constexpr uint32_t kRetryFirstMs = 60000;              // a failed lookup: again after 1, 2, 4 ... min, at most 6 h
constexpr uint32_t kRetryMaxMs = kZoneRefreshMs;

const char* s_user_agent = "GeoQuake-Lite";
SemaphoreHandle_t s_lock = nullptr;  // TZ changes and every local-time conversion (newlib keeps one TZ state)
SemaphoreHandle_t s_adopt_lock = nullptr;  // one adopt() at a time, decision to flash write (priority survives reboot)
Record s_record = {};                // what is applied (guarded by s_lock)
std::atomic<bool> s_zone_known{false};
std::atomic<bool> s_sntp_started{false};
std::atomic<bool> s_sntp_answered{false};  // since boot
std::atomic<bool> s_rtc_write_pending{false};
std::atomic<uint32_t> s_changes{0};
std::atomic<bool> s_zone_looked_up{false};  // a lookup succeeded since boot
std::atomic<uint32_t> s_zone_lookup_ms{0};
std::atomic<uint32_t> s_retry_at_ms{0};      // after a failed lookup; 0 = none pending
std::atomic<uint32_t> s_retry_interval_ms{kRetryFirstMs};
std::atomic<uint32_t> s_lookup_asked{0};     // counts "Set automatically" switched on: a lookup is wanted afresh

struct Lock {
    explicit Lock(SemaphoreHandle_t m = s_lock) : mutex(m), held(m && xSemaphoreTake(m, portMAX_DELAY) == pdTRUE) {}
    ~Lock() {
        if (held) xSemaphoreGive(mutex);
    }
    SemaphoreHandle_t mutex;
    bool held;
};

void apply_rule_locked(const char* tz) {
    setenv("TZ", tz, 1);
    tzset();
}

void save(const Record& r) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        Serial.println("[TIME] cannot open the time record");
        return;
    }
    const bool ok = prefs.putBytes(kRecordKey, &r, sizeof(r)) == sizeof(r);
    prefs.end();
    if (!ok) Serial.println("[TIME] saving the time record failed");
}

// Applies and stores a zone from `source` when the priority rule allows it; flash is written only on a change.
void adopt(Source source, const char* zone, const char* tz) {
    Lock adopting(s_adopt_lock);  // held through save(): a slower lower-priority adopt cannot overwrite a newer one
    Record next = {};
    bool changed = false;
    {
        Lock lock;
        const Source stored = s_zone_known ? static_cast<Source>(s_record.source) : Source::kNone;
        if (!should_replace(stored, source)) return;
        next.version = kRecordVersion;
        next.source = static_cast<uint8_t>(source);
        strlcpy(next.zone, zone ? zone : "", sizeof(next.zone));
        strlcpy(next.tz, tz, sizeof(next.tz));
        const time_t now = time(nullptr);
        next.updated_utc = epoch_valid(now) ? now : 0;
        if (!record_valid(next)) return;
        changed = !s_zone_known || s_record.source != next.source || strcmp(s_record.tz, next.tz) != 0 ||
                  strcmp(s_record.zone, next.zone) != 0;
        if (strcmp(s_record.tz, next.tz) != 0 || !s_record.tz[0]) apply_rule_locked(next.tz);
        s_record = next;
        s_zone_known = true;
    }
    if (!changed) return;
    save(next);
    s_changes.fetch_add(1);
    Serial.printf("[TIME] zone %s (%s) from %s\n", next.zone[0] ? next.zone : "-", next.tz,
                  source == Source::kNetwork ? "internet" : source == Source::kUser ? "Settings" : "old setting");
}

}  // namespace

void hande_time_begin(const char* user_agent) {
    if (user_agent && user_agent[0]) s_user_agent = user_agent;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_adopt_lock) s_adopt_lock = xSemaphoreCreateMutex();

    Record stored = {};
    Preferences prefs;
    if (prefs.begin(kNamespace, true)) {  // read-only open fails when nothing stored a zone yet
        if (prefs.getBytesLength(kRecordKey) == sizeof(stored)) prefs.getBytes(kRecordKey, &stored, sizeof(stored));
        prefs.end();
    }
    {
        Lock lock;
        if (record_valid(stored)) {
            s_record = stored;
            apply_rule_locked(stored.tz);
            s_zone_known = true;
        } else {
            // No stored zone: UTC is a real, usable zone (the HUD clock and night schedule work after NTP).
            memset(&s_record, 0, sizeof(s_record));
            strlcpy(s_record.tz, "UTC0", sizeof(s_record.tz));
            apply_rule_locked(s_record.tz);
            s_zone_known = true;
        }
    }

    uint32_t rtc_epoch = 0;
    const HandeRtcStatus rtc = hande_rtc_read_utc(&rtc_epoch);
    if (rtc == HandeRtcStatus::kOk && epoch_valid(rtc_epoch)) {
        struct timeval tv = {static_cast<time_t>(rtc_epoch), 0};
        settimeofday(&tv, nullptr);
    }
    Serial.printf("[TIME] start: clock %s (RTC %s), zone %s\n", hande_time_valid() ? "set" : "not set",
                  hande_rtc_status_text(rtc), s_zone_known ? s_record.tz : "UTC0");
}

void hande_time_on_wifi_up() {
    if (s_sntp_started.exchange(true)) return;  // SNTP keeps polling (every 3 h) from now on, also across reconnects
    // Not configTime(): it also resets TZ. The esp_sntp_* wrappers run on the TCP/IP thread (raw sntp_* calls from
    // an app task are not safe without lwIP core locking). Unsmeared servers only (Google smears leap seconds).
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "time.cloudflare.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "time.nist.gov");
    esp_sntp_init();
}

bool hande_time_zone_automatic() {
    Lock lock;
    return s_record.source != static_cast<uint8_t>(Source::kUser);
}

bool hande_time_zone_lookup_due() {
    if (!hande_time_zone_automatic()) return false;  // the user picked the zone: nothing is asked
    const uint32_t now = millis();
    const uint32_t retry_at = s_retry_at_ms.load();
    if (retry_at && static_cast<int32_t>(now - retry_at) < 0) return false;
    return !s_zone_looked_up || now - s_zone_lookup_ms.load() >= kZoneRefreshMs;
}

namespace {

#if defined(HANDE_SIM)
// Emulator: the answer comes from the fixture's http/ folder through the HTTPClient stand-in (no sockets).
int fetch_answer(uint32_t, uint32_t, char* body, size_t capacity) {
    HTTPClient http;
    if (!http.begin(String("https://") + kZoneHost + kZonePath)) return 0;
    http.setUserAgent(s_user_agent);
    const int code = http.GET();
    if (code == 200) strlcpy(body, http.getString().c_str(), capacity);
    http.end();
    return code > 0 ? code : 0;
}
#else
// One HTTP/1.0 request on a bare TLS connection, everything (DNS, connect, handshake, request, answer) inside one
// hard deadline: HTTPClient restarts its timeouts on every header line and character, so it cannot promise that.
// HTTP/1.0 also rules out chunked answers (http_answer() decodes them anyway). Returns the status, 0 = no answer.
int fetch_answer(uint32_t read_deadline_ms, uint32_t handshake_s, char* body, size_t capacity) {
    constexpr size_t kRawCapacity = 2048;  // the real answer is about 1 KB, almost all headers
    char* raw = static_cast<char*>(heap_caps_malloc(kRawCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw) raw = static_cast<char*>(malloc(kRawCapacity));
    if (!raw) return 0;
    size_t used = 0;
    {
        HandeTlsClient client;
        client.setCACert(HANDEOS_ROOT_CAS);
        client.setDnsTimeout(kZoneStepTimeoutMs);
        client.setHandshakeTimeout(handshake_s);  // seconds; the default is 120 s
        if (client.connect(kZoneHost, 443, static_cast<int32_t>(kZoneStepTimeoutMs))) {
            char request[192];
            const int len = snprintf(request, sizeof(request),
                                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: application/json\r\n"
                                     "Connection: close\r\n\r\n",
                                     kZonePath, kZoneHost, s_user_agent);  // Cloudflare refuses empty user agents
            if (len > 0 && static_cast<size_t>(len) < sizeof(request) &&
                client.write(reinterpret_cast<const uint8_t*>(request), static_cast<size_t>(len)) ==
                    static_cast<size_t>(len)) {
                while (used + 1 < kRawCapacity && static_cast<int32_t>(read_deadline_ms - millis()) > 0) {
                    const int available = client.available();
                    if (available > 0) {
                        const size_t want = std::min(static_cast<size_t>(available), kRawCapacity - 1 - used);
                        const int got = client.read(reinterpret_cast<uint8_t*>(raw + used), want);
                        if (got > 0) used += static_cast<size_t>(got);
                        continue;
                    }
                    if (!client.connected()) break;  // the server closed: the whole answer is here
                    delay(10);
                }
            }
        }
        if (!used) {
            char reason[96] = "";
            client.lastError(reason, sizeof(reason));
            Serial.printf("[TIME] zone lookup: no answer (%s)\n", reason[0] ? reason : "connection");
        }
        client.stop();
    }
    const int status = http_answer(raw, used, body, capacity);
    free(raw);
    return status;
}
#endif

bool lookup_now(uint32_t deadline_ms, uint32_t left_ms) {
    const uint32_t started = millis();
    const uint32_t handshake_s = std::min(kZoneHandshakeMaxS, (left_ms - 4 * kZoneStepTimeoutMs) / 1000);
    char answer[160];
    const int code = fetch_answer(deadline_ms - kZoneStepTimeoutMs, handshake_s, answer, sizeof(answer));
    char zone[sizeof(Record::zone)];
    int32_t server_offset = 0;
    if (code != 200 || !zone_answer(answer, zone, sizeof(zone), &server_offset)) {
        Serial.printf("[TIME] zone lookup failed: http=%d ms=%lu\n", code,
                      static_cast<unsigned long>(millis() - started));
        return false;
    }

    // The table rule, unless the server's live offset says it is wrong right now (a footer cannot express every
    // rule, e.g. Morocco's Ramadan change; a zone missing from the table): then the live offset as a fixed rule.
    char tz[sizeof(Record::tz)];
    const char* rule = zone_rule(zone);
    bool rule_matches = rule != nullptr;
    const time_t now = time(nullptr);
    if (rule && epoch_valid(now)) {
        Lock lock;
        apply_rule_locked(rule);
        struct tm local = {};
        localtime_r(&now, &local);
        rule_matches = offset_of(local, now) == server_offset;
        apply_rule_locked(s_record.tz);  // adopt() applies the result
    }
    if (rule_matches) strlcpy(tz, rule, sizeof(tz));
    else if (!fixed_offset_rule(server_offset, tz, sizeof(tz))) return false;
    Serial.printf("[TIME] zone lookup ok: %s offset=%ld%s ms=%lu\n", zone, static_cast<long>(server_offset),
                  rule_matches ? "" : " (fixed: table rule differs now)",
                  static_cast<unsigned long>(millis() - started));
    adopt(Source::kNetwork, zone, tz);  // does nothing when the user picked a zone meanwhile
    return true;
}

}  // namespace

bool hande_time_lookup_zone(uint32_t deadline_ms) {
    if (!hande_time_zone_lookup_due()) return s_zone_looked_up;
    const int32_t left_ms = static_cast<int32_t>(deadline_ms - millis());
    if (left_ms < static_cast<int32_t>(kZoneMinBudgetMs)) return false;  // not a failed try: the next round asks
    const uint32_t asked = s_lookup_asked.load();
    const bool ok = lookup_now(deadline_ms, static_cast<uint32_t>(left_ms));
    // What this try means for the next one. "Set automatically" being switched on writes the same fields under the
    // same lock: when that happened while this try ran, its fresh start stands and the next refresh asks again.
    Lock bookkeeping(s_adopt_lock);
    if (asked != s_lookup_asked.load()) return ok;
    if (ok) {
        s_zone_lookup_ms = millis();
        s_zone_looked_up = true;
        s_retry_at_ms = 0;
        s_retry_interval_ms = kRetryFirstMs;
    } else {
        const uint32_t interval = s_retry_interval_ms.load();
        s_retry_at_ms = (millis() + interval) | 1u;  // never 0 (= none pending)
        s_retry_interval_ms = interval >= kRetryMaxMs / 2 ? kRetryMaxMs : interval * 2;
    }
    return ok;
}

void hande_time_loop() {
    if (s_sntp_started && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        const bool first = !s_sntp_answered.exchange(true);
        s_rtc_write_pending = true;
        s_changes.fetch_add(1);
        if (first) Serial.printf("[TIME] SNTP answered: %ld UTC\n", static_cast<long>(time(nullptr)));
    }
    if (s_rtc_write_pending && hande_time_valid()) {
        const HandeRtcStatus rtc = hande_rtc_write_utc(static_cast<uint32_t>(time(nullptr)), 5);
        if (rtc != HandeRtcStatus::kBusy) {  // busy bus: next loop
            s_rtc_write_pending = false;
            if (rtc != HandeRtcStatus::kOk) Serial.printf("[TIME] RTC write: %s\n", hande_rtc_status_text(rtc));
        }
    }
}

bool hande_time_valid() { return epoch_valid(time(nullptr)); }

bool hande_time_zone_known() { return s_zone_known; }

bool hande_time_local_at(time_t utc, struct tm* out) {
    if (!out || !s_zone_known || !epoch_valid(utc)) return false;
    Lock lock;
    localtime_r(&utc, out);
    return true;
}

bool hande_time_local(struct tm* out) { return hande_time_local_at(time(nullptr), out); }

uint32_t hande_time_changes() { return s_changes.load(); }

const char* hande_time_zone_name(char* buf, size_t len) {
    if (!buf || len == 0) return "";
    Lock lock;
    const char* zone = (s_zone_known && s_record.zone[0]) ? s_record.zone : "UTC";
    strlcpy(buf, zone, len);
    return buf;
}

bool hande_time_set_zone(const char* zone) {
    const char* rule = zone_rule(zone);
    if (!rule) return false;
    adopt(Source::kUser, zone, rule);  // the user's pick replaces whatever was stored
    return true;
}

void hande_time_set_zone_automatic(bool on) {
    // One transaction with every adopt(): the zone in use cannot change between reading it and storing it under
    // its new source. On: it stays until the lookup answers. Off: it becomes the user's pick.
    Lock adopting(s_adopt_lock);
    Record next = {};
    {
        Lock lock;
        if (on == (s_record.source != static_cast<uint8_t>(Source::kUser))) return;
        next = s_record;
        next.version = kRecordVersion;
        next.source = static_cast<uint8_t>(on ? Source::kNetwork : Source::kUser);
        if (!record_valid(next)) return;
        s_record = next;
    }
    save(next);
    s_changes.fetch_add(1);
    if (on) {  // ask at the next download, whatever was learned or failed before (still under s_adopt_lock)
        s_lookup_asked.fetch_add(1);
        s_zone_looked_up = false;
        s_retry_at_ms = 0;
        s_retry_interval_ms = kRetryFirstMs;
    }
}
