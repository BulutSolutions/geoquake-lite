#include "hande_time.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include <atomic>

#include "hande_rtc.h"
#include "hande_time_policy.h"

using namespace hande_time_policy;

namespace {

constexpr const char* kNamespace = "gql_time";
constexpr const char* kRecordKey = "rec";

const char* s_user_agent = "GeoQuake-Lite";
SemaphoreHandle_t s_lock = nullptr;  // TZ changes and every local-time conversion (newlib keeps one TZ state)
SemaphoreHandle_t s_adopt_lock = nullptr;  // one adopt() at a time, decision to flash write (priority survives reboot)
Record s_record = {};                // what is applied (guarded by s_lock)
std::atomic<bool> s_zone_known{false};
std::atomic<bool> s_sntp_started{false};
std::atomic<bool> s_sntp_answered{false};  // since boot
std::atomic<bool> s_rtc_write_pending{false};
std::atomic<uint32_t> s_changes{0};

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

// Applies and stores a zone from `source` when the priority rule allows it (or always, `force`); flash is written
// only on a change.
void adopt(Source source, const char* zone, const char* tz, bool force = false) {
    Lock adopting(s_adopt_lock);  // held through save(): a slower lower-priority adopt cannot overwrite a newer one
    Record next = {};
    bool changed = false;
    {
        Lock lock;
        const Source stored = s_zone_known ? static_cast<Source>(s_record.source) : Source::kNone;
        if (!force && !should_replace(stored, source)) return;
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
                  source == Source::kNetwork ? "internet" : source == Source::kBrowser ? "browser" : "old setting");
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

void hande_time_set_from_browser(int64_t utc_s, const char* zone) {
    if (epoch_valid(utc_s) && !s_sntp_answered) {
        struct timeval tv = {static_cast<time_t>(utc_s), 0};
        settimeofday(&tv, nullptr);
        s_rtc_write_pending = true;
        s_changes.fetch_add(1);
        Serial.printf("[TIME] clock from browser: %lld\n", static_cast<long long>(utc_s));
    }
    const char* rule = zone_rule(zone);
    if (rule) adopt(Source::kBrowser, zone, rule);
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
    adopt(Source::kBrowser, zone, rule, true);  // the user's pick replaces whatever was stored
    return true;
}