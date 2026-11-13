#include "quake_loader.h"
#include "ui_sync.h"
#include "settings_manager.h"
#include "quake_notifications.h"
#include "audio_manager.h"
#include "perf_telemetry.h"
#include "map_projection.h"
#include "wifi_manager.h"
#include "usgs_parse.h"
#include "usgs_root_ca.h"
#include "hande_time.h"
#include <HTTPClient.h>
#include "hande_tls_client.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <time.h>

// --- PRIVATE STATIC STATE (Zero Runtime Allocations) ---
// Cold records (192 B each, 19.2 KB) live in PSRAM: allocated once in
// init_quake_loader(), read only by the detail/status/notification UIs.
static Earthquake* s_quakes_array = nullptr;
static size_t s_quakes_count = 0;
// Hot records for the map renderer (16 B each, internal RAM), same order.
static HotQuake s_quakes_hot[MAX_STORED_QUAKES];
static uint32_t s_quakes_version = 0;  // bumped under quake_mutex on every rewrite
static std::vector<Earthquake> s_compat_vector; // Maintained only for legacy callers
static SemaphoreHandle_t quake_mutex = NULL;
static bool first_fetch_done = false;

// Custom PSRAM allocator for ArduinoJson 7 (Zero Internal SRAM Choke Point)
class SpiRamJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    static SpiRamJsonAllocator* instance() {
        static SpiRamJsonAllocator alloc;
        return &alloc;
    }
};

std::atomic<bool> is_fetching_quakes{false};
static std::atomic<int> pending_notification_audio_count{0};
static std::atomic<uint32_t> pending_notification_audio_after_ms{0};
std::atomic<bool> quake_data_ready{false};
unsigned long last_sync_time_ms = 0;
time_t last_sync_epoch = 0;
static std::atomic<QuakeFetchResult> last_fetch_result{FETCH_SUCCESS};  // written on Core 0, read by the status bar on Core 1

QuakeFetchResult quake_last_fetch_result() { return last_fetch_result.load(); }
void quake_note_connect_failed() { last_fetch_result = FETCH_WIFI_ERROR; }

// Internal heap now, its largest block and its lowest ever, plus the least stack this task (the worker) ever had left.
static void log_quake_heap(const char* label) {
    Serial.printf("[QUAKE] Heap %s: free=%u KB largest=%u KB min=%u KB stack_left=%u B\n",
                  label,
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
}

void init_quake_loader() {
    if (quake_mutex == NULL) {
        quake_mutex = xSemaphoreCreateMutex();
    }
    if (s_quakes_array == nullptr) {
        // One-time allocation (not a hot path). >4 KB buffers belong in PSRAM.
        s_quakes_array = static_cast<Earthquake*>(
            heap_caps_calloc(MAX_STORED_QUAKES, sizeof(Earthquake), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_quakes_array == nullptr) {
            s_quakes_array = static_cast<Earthquake*>(calloc(MAX_STORED_QUAKES, sizeof(Earthquake)));
        }
    }
}

// Derive the render-path record for cold record i (caller holds quake_mutex).
static void quake_fill_hot_locked(size_t i) {
    const Earthquake& eq = s_quakes_array[i];
    const WorldPoint wp = world_from_latlon(eq.lat, eq.lon);
    HotQuake& h = s_quakes_hot[i];
    h.wx = wp.x;
    h.wy = wp.y;
    h.mag = eq.mag;
    h.time_s = (eq.time > 0) ? static_cast<uint32_t>(eq.time / 1000) : 0u;
}

bool quake_snapshot_hot(HotQuake* out, size_t cap, size_t* out_count, uint32_t* version) {
    if (!quake_mutex) return false;
    if (xSemaphoreTake(quake_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    if (*version != s_quakes_version) {
        const size_t n = (s_quakes_count < cap) ? s_quakes_count : cap;
        memcpy(out, s_quakes_hot, n * sizeof(HotQuake));
        *out_count = n;
        *version = s_quakes_version;
    }
    xSemaphoreGive(quake_mutex);
    return true;
}

// Load dummy earthquake data for testing coordinate alignment
void load_test_earthquakes() {
    if (!s_quakes_array) return;
    if (quake_mutex) xSemaphoreTake(quake_mutex, portMAX_DELAY);

    s_quakes_count = 0;

    auto add_test_quake = [](const char* id, float mag, const char* place, float lat, float lon, float depth) {
        if (s_quakes_count >= MAX_STORED_QUAKES) return;
        Earthquake& eq = s_quakes_array[s_quakes_count];
        memset(&eq, 0, sizeof(Earthquake));
        strncpy(eq.id, id, sizeof(eq.id) - 1);
        eq.mag = mag;
        strncpy(eq.place, place, sizeof(eq.place) - 1);
        eq.time = 1704067200000LL;
        eq.lat = lat;
        eq.lon = lon;
        eq.depth = depth;
        quake_fill_hot_locked(s_quakes_count);
        s_quakes_count++;
    };

    add_test_quake("test_nyc", 5.5f, "New York City, USA", 40.7128f, -74.0060f, 10.0f);
    add_test_quake("test_istanbul", 6.2f, "Istanbul, Turkey", 41.0082f, 28.9784f, 15.0f);
    add_test_quake("test_tokyo", 4.8f, "Tokyo, Japan", 35.6762f, 139.6503f, 20.0f);
    add_test_quake("test_sydney", 4.2f, "Sydney, Australia", -33.8688f, 151.2093f, 12.0f);
    add_test_quake("test_saopaulo", 3.9f, "Sao Paulo, Brazil", -23.5505f, -46.6333f, 8.0f);
    add_test_quake("test_reykjavik", 5.0f, "Reykjavik, Iceland", 64.1466f, -21.9426f, 5.0f);

    // Update compatibility vector
    s_compat_vector.assign(s_quakes_array, s_quakes_array + s_quakes_count);
    s_quakes_version++;

    quake_data_ready = true;
    if (quake_mutex) xSemaphoreGive(quake_mutex);

    Serial.printf("[QUAKE] Loaded %zu TEST earthquakes for coordinate testing (Zero Heap Alloc)\n", s_quakes_count);
}

// Zero-copy thread-safe accessors
size_t quake_get_count() {
    if (!quake_mutex) return 0;
    xSemaphoreTake(quake_mutex, portMAX_DELAY);
    size_t count = s_quakes_count;
    xSemaphoreGive(quake_mutex);
    return count;
}

bool quake_read_item(size_t index, Earthquake& out) {
    if (!quake_mutex) return false;
    if (xSemaphoreTake(quake_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool ok = false;
    if (index < s_quakes_count) {
        out = s_quakes_array[index];
        ok = true;
    }
    xSemaphoreGive(quake_mutex);
    return ok;
}

bool quake_lock_read(const Earthquake** out_ptr, size_t* out_count) {
    if (!quake_mutex) return false;
    if (xSemaphoreTake(quake_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    *out_ptr = s_quakes_array;
    *out_count = s_quakes_count;
    return true;
}

void quake_unlock_read() {
    if (quake_mutex) {
        xSemaphoreGive(quake_mutex);
    }
}

// Backward-compatible accessors
const std::vector<Earthquake>& get_quakes_list() {
    return s_compat_vector;
}

bool quake_copy_list(std::vector<Earthquake>& out) {
    if (!quake_mutex) return false;
    if (xSemaphoreTake(quake_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        out.assign(s_quakes_array, s_quakes_array + s_quakes_count);
        xSemaphoreGive(quake_mutex);
        return true;
    }
    return false;
}

// Wall clock for the reader's idle/deadline checks.
static uint32_t quake_now_ms() { return static_cast<uint32_t>(millis()); }

// The TLS stream is NON-blocking: read() returns -1 on a packet gap, which is
// not EOF. Poll instead of failing the parse — unless the client is closed and
// has no buffered bytes left, which is a real end of stream.
static void quake_wait_tick() { vTaskDelay(pdMS_TO_TICKS(20)); }

class UsgsTlsReader : public UsgsBlockingReader<WiFiClient> {
public:
    UsgsTlsReader(WiFiClient& client, uint32_t deadline_ms)
        : UsgsBlockingReader<WiFiClient>(client, deadline_ms, &quake_now_ms, &quake_wait_tick), client_(client) {}

    bool source_closed() const override { return !client_.connected() && client_.available() == 0; }

private:
    WiFiClient& client_;
};

// Whole fetch: connect + TLS + headers + body + parse.
static constexpr uint32_t kFetchDeadlineMs = 45000;
static constexpr uint32_t kTlsHandshakeTimeoutSec = 15;
// Feed name for the existing time_range setting (0=1h, 1=24h, 2=7d).
static const char* usgs_feed_name(int time_range) {
    static const char* const kFeeds[] = {"all_hour", "all_day", "all_week"};
    if (time_range < 0 || time_range > 2) time_range = 1;
    return kFeeds[time_range];
}

// Certificate validation needs the current date, so no fetch before the clock is set.
static bool quake_clock_ready() {
    return static_cast<long long>(time(nullptr)) > 1700000000LL;
}

static void quake_parse_yield() { vTaskDelay(1); }

// The worker owns the radio until it is fully done with it: the caller that
// waits (wifi_managed_operation's callback) may return first, and the lease is
// the only thing that keeps Wi-Fi powered while this task still reads.
struct RadioLease {
    RadioLease() { wifi_worker_lease_acquire(); }
    ~RadioLease() { wifi_worker_lease_release(); }
};

static void task_fetch_quakes_worker() {
    RadioLease radio_lease;  // released at the very end of this function
    last_fetch_result = FETCH_WIFI_ERROR;
    int notification_audio_count = 0;

    Serial.println("====================================");
    Serial.println("[QUAKE] Fetching real-time earthquake data from USGS...");

    if (!quake_clock_ready()) {
        // Not ready: TLS would reject the certificate. The next normal cycle retries.
        Serial.println("[QUAKE] Clock not set yet - skipping fetch, will retry next cycle");
        is_fetching_quakes = false;
        ui_queue_status_text("restore");
        return;
    }

    log_quake_heap("before TLS");

    const uint32_t fetch_start_ms = quake_now_ms();
    const uint32_t fetch_deadline_ms = fetch_start_ms + kFetchDeadlineMs;

    {
        HTTPClient http;
        HandeTlsClient client;
        client.setCACert(USGS_ROOT_CA);
        client.setHandshakeTimeout(kTlsHandshakeTimeoutSec);

        // Get settings for filtering
        AppSettings* settings = settings_get();
        const char* feed = usgs_feed_name(settings ? settings->time_range : 1);

        // Build URL using stack buffer (Zero heap allocation)
        char url_buffer[128];
        snprintf(url_buffer, sizeof(url_buffer),
                 "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/%s.geojson", feed);

        Serial.printf("[QUAKE] Request URL: %s\n", url_buffer);

        if (!http.begin(client, url_buffer)) {
            Serial.println("[QUAKE] ERROR: http.begin() failed");
            last_fetch_result = FETCH_WIFI_ERROR;
            http.end();
            client.stop();
            is_fetching_quakes = false;
            ui_queue_status_text("restore");
            return;
        }

        http.useHTTP10(true);  // plain body, no chunked framing
        http.addHeader("Accept-Encoding", "identity");
        http.setConnectTimeout(5000);
        http.setTimeout(10000);

        Serial.println("[QUAKE] Sending HTTP GET request...");
        unsigned long http_start_ms = millis();
        int httpCode = http.GET();
        unsigned long http_duration_ms = millis() - http_start_ms;

        Serial.printf("[QUAKE] HTTP Response: %d (took %lu ms)\n", httpCode, http_duration_ms);
        log_quake_heap("after HTTP GET");

        if (httpCode == 200) {
            WiFiClient* stream = http.getStreamPtr();
            Earthquake* parsed = static_cast<Earthquake*>(
                heap_caps_malloc(MAX_STORED_QUAKES * sizeof(Earthquake), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (parsed == nullptr) {
                parsed = static_cast<Earthquake*>(malloc(MAX_STORED_QUAKES * sizeof(Earthquake)));
            }
            struct ParsedFree {
                Earthquake* p;
                ~ParsedFree() { free(p); }
            } parsed_free{parsed};  // one cleanup point for every path below

            if (stream == nullptr || parsed == nullptr) {
                Serial.println("[QUAKE] ERROR: no memory/stream for the USGS parse");
                last_fetch_result = FETCH_PARSE_ERROR;
            } else {
                UsgsTlsReader reader(*stream, fetch_deadline_ms);
                UsgsParseLimits limits;
                limits.yield_cb = &quake_parse_yield;
                const time_t now_epoch = time(nullptr);
                if (now_epoch > 0) limits.now_ms = static_cast<long long>(now_epoch) * 1000LL;

                UsgsParseResult parsed_result;
                {
                    PERF_SCOPE("QUAKE_PARSE");
                    parsed_result =
                        usgs_parse_features(reader, parsed, MAX_STORED_QUAKES, limits,
                                            SpiRamJsonAllocator::instance());
                }

                Serial.printf("[QUAKE] USGS parse status=%d features=%zu\n",
                              static_cast<int>(parsed_result.status), parsed_result.count);

                if (parsed_result.status == UsgsParseStatus::Ok && s_quakes_array != nullptr) {
                    xSemaphoreTake(quake_mutex, portMAX_DELAY);
                    s_quakes_count = 0;

                    for (size_t i = 0; i < parsed_result.count && i < MAX_STORED_QUAKES; ++i) {
                        s_quakes_array[i] = parsed[i];
                        quake_fill_hot_locked(i);  // world coords once per fetch
                        ++s_quakes_count;
                    }

                    // Sync backward-compatible vector
                    s_compat_vector.assign(s_quakes_array, s_quakes_array + s_quakes_count);
                    s_quakes_version++;

                    quake_data_ready = true;
                    last_sync_time_ms = millis();
                    last_sync_epoch = time(nullptr);
                    xSemaphoreGive(quake_mutex);

                    unsigned long total_fetch_time = millis() - fetch_start_ms;
                    Serial.printf("[QUAKE] SUCCESS: Loaded %zu earthquakes (Retained Fixed Storage)\n",
                                  s_quakes_count);

                    int newly_added = quake_notifs_add_from_list(s_compat_vector, settings_get()->mag_min);
                    if (!first_fetch_done) {
                        first_fetch_done = true;
                        Serial.printf(
                            "[AUDIO] Baseline populated with %d earthquake notification(s); no sound on first fetch\n",
                            newly_added);
                    } else if (newly_added > 0) {
                        notification_audio_count = newly_added;
                    }

                    // Quick summary
                    if (s_quakes_count > 0) {
                        float mag_min = 99.0f, mag_max = -99.0f;
                        for (size_t i = 0; i < s_quakes_count; ++i) {
                            if (s_quakes_array[i].mag < mag_min) mag_min = s_quakes_array[i].mag;
                            if (s_quakes_array[i].mag > mag_max) mag_max = s_quakes_array[i].mag;
                        }
                        Serial.printf("[QUAKE] Magnitude range: %.1f - %.1f | Count: %zu\n", mag_min, mag_max,
                                      s_quakes_count);
                    }

                    Serial.printf("[QUAKE] Total fetch time: %lu ms\n", total_fetch_time);
                    Serial.println("====================================");

                    last_fetch_result = FETCH_SUCCESS;
                } else {
                    // Keep the last good data; the status bar reports the failure.
                    unsigned long total_fetch_time = millis() - fetch_start_ms;
                    Serial.printf("[QUAKE] ERROR: USGS parse failed (status=%d)\n",
                                  static_cast<int>(parsed_result.status));
                    Serial.printf("[QUAKE] Total fetch time: %lu ms\n", total_fetch_time);
                    Serial.println("====================================");
                    last_fetch_result = (parsed_result.status == UsgsParseStatus::Timeout) ? FETCH_TIMEOUT
                                                                                          : FETCH_PARSE_ERROR;
                }
            }
        } else {
            unsigned long total_fetch_time = millis() - fetch_start_ms;
            Serial.printf("[QUAKE] ERROR: HTTP request failed with code: %d\n", httpCode);
            Serial.printf("[QUAKE] Total fetch time: %lu ms\n", total_fetch_time);
            Serial.println("====================================");
            last_fetch_result = FETCH_HTTP_ERROR;
        }

        http.end();
        client.stop();
    }


    log_quake_heap("after HTTP cleanup");

    // The time zone, when due (once per boot, then every 6 h; never when the user picked one in Settings). Here
    // because this task does the firmware's HTTPS: the earthquake connection is closed (two TLS connections at once
    // do not fit in memory), the lookup ends inside the same deadline, and no other fetch starts while
    // is_fetching_quakes is set.
    if (hande_time_zone_lookup_due()) {
        hande_time_lookup_zone(fetch_deadline_ms);
        log_quake_heap("after the time-zone lookup");
    }

    is_fetching_quakes = false;
    ui_queue_status_text("restore");  // the type: put the "40/24h" summary back (not the word itself)
    if (notification_audio_count > 0) {
        pending_notification_audio_after_ms.store(millis() + 50);
        pending_notification_audio_count.store(notification_audio_count);
    }
}

// The worker's stack. Measured on the board (2026-11-12, least stack ever left, printed by log_quake_heap): the USGS
// download (an RSA chain) uses 4.7 KB; the time-zone lookup's TLS handshake (an ECDSA P-384 chain) 6.3 KB. With 8 KB
// that left 1.9 KB, so the stack grew by 2 KB to keep the margin the download alone had.
static constexpr uint32_t kQuakeTaskStackBytes = 10240;

static void task_fetch_quakes(void* param) {
    task_fetch_quakes_worker();
    vTaskDelete(NULL);
}

void quake_dispatch_pending_audio() {
    const int count = pending_notification_audio_count.load();
    if (count <= 0 || is_fetching_quakes.load()) return;

    const uint32_t now = millis();
    const uint32_t ready_at = pending_notification_audio_after_ms.load();
    if (static_cast<int32_t>(now - ready_at) < 0) return;

    int expected = count;
    if (!pending_notification_audio_count.compare_exchange_strong(expected, 0)) {
        return;
    }
    Serial.printf("[AUDIO] %d new earthquake(s) detected, playing notification sound\n", count);
    audio_play_notification();
}

void fetch_quakes() {
    bool expected = false;
    if (is_fetching_quakes.compare_exchange_strong(expected, true)) {
        if (xTaskCreatePinnedToCore(task_fetch_quakes, "QuakeTask", kQuakeTaskStackBytes, NULL, tskIDLE_PRIORITY, NULL,
                                    0) != pdPASS) {
            last_fetch_result = FETCH_WIFI_ERROR;  // the status bar shows Sync failed instead of a stuck Syncing...
            is_fetching_quakes = false;
        }
    }
}

// The worker's fetch deadline is 45 s (TLS + body + parse): the waiter must outlast it, otherwise it would return
// and let Wi-Fi be powered off while the worker still reads.
static constexpr unsigned long kFetchWaitMs = 50000UL;
static_assert(kFetchWaitMs > kFetchDeadlineMs, "the waiter must outlast the fetch deadline");

QuakeFetchResult fetch_quakes_with_result() {
    bool expected = false;
    if (!is_fetching_quakes.compare_exchange_strong(expected, true)) {
        unsigned long start = millis();
        while (is_fetching_quakes && (millis() - start < kFetchWaitMs)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (is_fetching_quakes) {
            return FETCH_TIMEOUT;
        }
        return last_fetch_result;
    }

    last_fetch_result = FETCH_WIFI_ERROR;
    if (xTaskCreatePinnedToCore(task_fetch_quakes, "QuakeTask", kQuakeTaskStackBytes, NULL, tskIDLE_PRIORITY, NULL,
                                0) != pdPASS) {
        is_fetching_quakes = false;
        return FETCH_WIFI_ERROR;
    }

    unsigned long start = millis();
    while (is_fetching_quakes && (millis() - start < kFetchWaitMs)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (is_fetching_quakes) {
        return FETCH_TIMEOUT;
    }

    return last_fetch_result;
}

void abort_quake_fetch() {
    // Cleanup hooks if needed
}
