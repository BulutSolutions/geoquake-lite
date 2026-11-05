#include "wifi_manager.h"
#include "config.h"
#include "settings_manager.h"
#include "battery.h"
#include "hande_time.h"

#include <WiFi.h>
#include <atomic>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

namespace {

constexpr uint32_t RECONNECT_INITIAL_MS = 5000;
constexpr uint32_t RECONNECT_MAX_MS = 300000;
constexpr uint32_t RECONNECT_ATTEMPT_TIMEOUT_MS = WIFI_CONNECT_TIMEOUT * 1000UL;
constexpr size_t MAX_SSID_BYTES = 33;
constexpr size_t MAX_PASSWORD_BYTES = 65;
constexpr int kScanAttempts = 3;           // a scan fails while a connect is in progress: retry a couple of times
constexpr uint32_t kScanRetryDelayMs = 1000;

bool initialized = false;
bool currently_connected = false;
uint32_t last_reconnect_attempt_ms = 0;   // when the last attempt finished (or was skipped)
uint32_t reconnect_backoff_ms = RECONNECT_INITIAL_MS;
bool reconnect_in_flight = false;
uint32_t reconnect_started_ms = 0;
char saved_ssid_buf[MAX_SSID_BYTES] = {};
char saved_password_buf[MAX_PASSWORD_BYTES] = {};
char current_ssid_buf[MAX_SSID_BYTES] = {};
char ip_buf[20] = {};
bool credentials_loaded = false;
SemaphoreHandle_t wifi_api_mutex = nullptr;

unsigned long last_operation_start_ms = 0;
unsigned long last_operation_duration_ms = 0;
unsigned long total_wifi_on_time_ms = 0;

// The app's own saved network: the station config the Wi-Fi driver keeps in NVS for this firmware.
void load_saved_credentials() {
    if (credentials_loaded) return;
    credentials_loaded = true;
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) return;
    const size_t ssid_length =
        strnlen(reinterpret_cast<const char*>(config.sta.ssid), sizeof(config.sta.ssid));
    const size_t password_length =
        strnlen(reinterpret_cast<const char*>(config.sta.password), sizeof(config.sta.password));
    memcpy(saved_ssid_buf, config.sta.ssid, ssid_length);
    saved_ssid_buf[ssid_length] = '\0';
    memcpy(saved_password_buf, config.sta.password, password_length);
    saved_password_buf[password_length] = '\0';
}

bool prepare_wifi() {
    if (WiFi.getMode() == WIFI_OFF) {
        if (!WiFi.mode(WIFI_STA)) {
            Serial.println("[WIFI] Cannot start Wi-Fi; staying offline");
            return false;
        }
    }
    load_saved_credentials();
    return true;
}

bool wait_for_connection(uint32_t timeout_ms) {
    const uint32_t deadline = millis() + timeout_ms;
    while (WiFi.status() != WL_CONNECTED &&
           static_cast<int32_t>(deadline - millis()) > 0) {
        esp_task_wdt_reset();
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

void on_connected() {
    currently_connected = true;
    reconnect_in_flight = false;
    reconnect_backoff_ms = RECONNECT_INITIAL_MS;
    last_reconnect_attempt_ms = 0;
    // Enable automatic DTIM modem sleep: saves 80% RF power while keeping IP and AP alive
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    Serial.printf("[WIFI] Connected to %s, IP %s (Modem-Sleep enabled)\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    hande_time_on_wifi_up();  // clock: starts SNTP once (UDP, non-blocking)
}

// Starts a connection without waiting: the result is read on later loop passes.
bool begin_saved_connection() {
    if (!prepare_wifi()) return false;
    if (saved_ssid_buf[0] == '\0') return false;
    WiFi.disconnect(false, false);
    WiFi.begin(saved_ssid_buf, saved_password_buf);
    Serial.printf("[WIFI] Trying saved network: %s\n", saved_ssid_buf);
    return true;
}

bool try_saved_credentials() {
    if (!begin_saved_connection()) return false;
    delay(100);
    if (!wait_for_connection(RECONNECT_ATTEMPT_TIMEOUT_MS)) return false;
    on_connected();
    return true;
}

bool connect_available_credentials() {
    if (!prepare_wifi()) {
        currently_connected = false;
        last_reconnect_attempt_ms = millis();
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        on_connected();
        return true;
    }
    if (try_saved_credentials()) return true;
    currently_connected = false;
    last_reconnect_attempt_ms = millis();
    if (saved_ssid_buf[0] == '\0') Serial.println("[WIFI] Not connected. No saved network.");
    return false;
}

const char* wifi_event_name(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_READY: return "WIFI_READY";
        case ARDUINO_EVENT_WIFI_SCAN_DONE: return "SCAN_DONE";
        case ARDUINO_EVENT_WIFI_STA_START: return "STA_START";
        case ARDUINO_EVENT_WIFI_STA_STOP: return "STA_STOP";
        case ARDUINO_EVENT_WIFI_STA_CONNECTED: return "STA_CONNECTED";
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: return "STA_DISCONNECTED";
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: return "GOT_IP";
        case ARDUINO_EVENT_WIFI_STA_LOST_IP: return "LOST_IP";
        default: return "UNKNOWN";
    }
}

void wifi_event_handler(WiFiEvent_t event) {
    Serial.printf("[WIFI_EVENT] %s (%d) at %lu ms\n", wifi_event_name(event),
                  event, millis());
}

}  // namespace

bool wifi_api_lock(uint32_t timeout_ms) {
    if (!wifi_api_mutex) return false;
    const TickType_t wait_ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(wifi_api_mutex, wait_ticks) == pdTRUE;
}

void wifi_api_unlock() {
    if (wifi_api_mutex) xSemaphoreGiveRecursive(wifi_api_mutex);
}

void wifi_init() {
    if (!wifi_api_mutex) wifi_api_mutex = xSemaphoreCreateRecursiveMutex();
    if (!wifi_api_mutex) {
        Serial.println("[WIFI] Cannot create WiFi API mutex; staying offline");
        return;
    }
    if (!wifi_api_lock(0)) return;
    if (initialized) {
        wifi_api_unlock();
        return;
    }
    initialized = true;
    WiFi.onEvent(wifi_event_handler);
    connect_available_credentials();
    wifi_api_unlock();
}

static std::atomic<int> s_active_managed_ops{0};
// Workers (the quake fetch) that extend an active managed operation: the
// operation's caller may return before the worker is done with the radio.
static std::atomic<int> s_worker_leases{0};
static std::atomic<bool> s_pending_power_off{false};
static SemaphoreHandle_t s_managed_op_mutex = NULL;
static bool s_intentional_power_off = false;

void wifi_process() {
    if (s_pending_power_off.load() && s_active_managed_ops.load() <= 0) {
        wifi_power_off();  // retry a shutdown that was skipped because the Wi-Fi lock was busy
    }
    if (s_intentional_power_off && !battery_is_usb_connected()) {
        // Wi-Fi was intentionally powered down for battery duty cycling; do not background reconnect
        return;
    }
    if (!wifi_api_lock(0)) return;
    const bool connected_now = WiFi.status() == WL_CONNECTED;
    if (connected_now && !currently_connected) {
        on_connected();
    } else if (!connected_now && currently_connected) {
        currently_connected = false;
        last_reconnect_attempt_ms = millis();
        Serial.println("[WIFI] Connection lost; reconnecting in the background");
    }
    const uint32_t now_ms = millis();
    if (!connected_now) {
        if (reconnect_in_flight) {
            // Poll the attempt started on an earlier pass: never block loop().
            if (now_ms - reconnect_started_ms >= RECONNECT_ATTEMPT_TIMEOUT_MS) {
                reconnect_in_flight = false;
                last_reconnect_attempt_ms = now_ms;  // completed (unsuccessfully) now
                reconnect_backoff_ms = reconnect_backoff_ms >= RECONNECT_MAX_MS / 2
                                           ? RECONNECT_MAX_MS
                                           : reconnect_backoff_ms * 2;
                Serial.printf("[WIFI] Reconnect timed out; next try in %lu s\n",
                              static_cast<unsigned long>(reconnect_backoff_ms / 1000));
            }
        } else if (now_ms - last_reconnect_attempt_ms >= reconnect_backoff_ms) {
            if (begin_saved_connection()) {
                reconnect_in_flight = true;
                reconnect_started_ms = now_ms;
            } else {
                last_reconnect_attempt_ms = now_ms;  // no saved network: nothing to retry
            }
        }
    }
    wifi_api_unlock();
}

// Last status read under the Wi-Fi lock. When the lock is busy (a connect holds it), the getters return this instead
// of "disconnected", so status readers (Settings, HUD) never flicker and never wait on the UI thread.
static std::atomic<bool> s_last_connected{false};
static std::atomic<int32_t> s_last_rssi{0};

bool wifi_is_connected() {
    if (!wifi_api_lock(0)) return s_last_connected.load(std::memory_order_relaxed);
    const bool connected = currently_connected && (WiFi.status() == WL_CONNECTED);
    wifi_api_unlock();
    s_last_connected.store(connected, std::memory_order_relaxed);
    return connected;
}

bool wifi_is_powered_off() {
    if (!wifi_api_lock(0)) return false;
    const bool powered_off = WiFi.getMode() == WIFI_OFF;
    wifi_api_unlock();
    return powered_off;
}

int32_t wifi_get_rssi() {
    if (!wifi_api_lock(0)) return s_last_rssi.load(std::memory_order_relaxed);
    const int32_t rssi = WiFi.status() == WL_CONNECTED
                             ? static_cast<int32_t>(WiFi.RSSI())
                             : 0;
    wifi_api_unlock();
    s_last_rssi.store(rssi, std::memory_order_relaxed);
    return rssi;
}

const char* wifi_get_ssid() {
    if (!wifi_api_lock(0)) return current_ssid_buf;  // the last one read
    if (WiFi.status() != WL_CONNECTED) {
        current_ssid_buf[0] = '\0';
        wifi_api_unlock();
        return current_ssid_buf;
    }
    WiFi.SSID().toCharArray(current_ssid_buf, sizeof(current_ssid_buf));
    wifi_api_unlock();
    return current_ssid_buf;
}

const char* wifi_get_ip_str() {
    if (!wifi_api_lock(0)) return ip_buf;  // the last one read
    if (WiFi.status() != WL_CONNECTED) {
        ip_buf[0] = '\0';
        wifi_api_unlock();
        return ip_buf;
    }
    WiFi.localIP().toString().toCharArray(ip_buf, sizeof(ip_buf));
    wifi_api_unlock();
    return ip_buf;
}

void wifi_get_mac(uint8_t mac[6]) {
    if (!mac) return;
    memset(mac, 0, 6);
    if (!wifi_api_lock(0)) return;
    WiFi.macAddress(mac);
    wifi_api_unlock();
}

String wifi_get_mac_address() {
    if (!wifi_api_lock(0)) return String();
    const String mac = WiFi.macAddress();
    wifi_api_unlock();
    return mac;
}

bool wifi_has_saved_credentials() {
    if (!wifi_api_lock(100)) return false;
    load_saved_credentials();
    const bool has_credentials = saved_ssid_buf[0] != '\0';
    wifi_api_unlock();
    return has_credentials;
}

bool wifi_try_saved_credentials() {
    if (!wifi_api_lock(100)) return false;
    const bool connected = connect_available_credentials();
    wifi_api_unlock();
    return connected;
}

const char* wifi_get_saved_ssid() {
    if (!wifi_api_lock(100)) return "";
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.SSID().toCharArray(saved_ssid_buf, sizeof(saved_ssid_buf));
        wifi_api_unlock();
        return saved_ssid_buf;
    }
    load_saved_credentials();
    wifi_api_unlock();
    return saved_ssid_buf;
}

// Stores a new network as the app's own station config (the driver keeps it in NVS; the same record the app has
// always used) and connects to it. Blocks the calling task for up to timeout_ms (Improv over USB serial only).
bool wifi_connect_new_credentials(const char* ssid, const char* password, uint32_t timeout_ms) {
    // wifi_config_t's ssid[32] and password[64] are NOT NUL-terminated when full: validate the lengths and copy
    // exactly the given bytes (the rest of the struct stays zeroed). A WPA passphrase is 8..63 chars, a 64-char
    // hex PSK is 64; an empty password is an open network.
    const size_t ssid_length = ssid ? strlen(ssid) : 0;
    const size_t password_length = password ? strlen(password) : 0;
    if (ssid_length == 0 || ssid_length > sizeof(((wifi_config_t*)nullptr)->sta.ssid) ||
        password_length > sizeof(((wifi_config_t*)nullptr)->sta.password) ||
        (password_length > 0 && password_length < 8)) {
        Serial.printf("[WIFI] Rejected credentials (ssid %u B, password %u B)\n", static_cast<unsigned>(ssid_length),
                      static_cast<unsigned>(password_length));
        return false;
    }
    if (!wifi_api_mutex) wifi_api_mutex = xSemaphoreCreateRecursiveMutex();
    if (!wifi_api_lock(1000)) {
        Serial.println("[WIFI] API busy; new credentials not stored");
        return false;
    }
    if (WiFi.getMode() == WIFI_OFF && !WiFi.mode(WIFI_STA)) {
        Serial.println("[WIFI] Cannot start Wi-Fi; new credentials not stored");
        wifi_api_unlock();
        return false;
    }
    s_intentional_power_off = false;
    s_pending_power_off = false;  // a shutdown deferred earlier must never take the freshly provisioned link down
    reconnect_in_flight = false;
    last_reconnect_attempt_ms = millis();

    // The station config is built fresh for the submitted credentials: no security field of an earlier network
    // survives (an open network must not keep a WPA2 threshold, and the reverse).
    wifi_config_t config = {};
    memcpy(config.sta.ssid, ssid, ssid_length);
    if (password_length) memcpy(config.sta.password, password, password_length);
    config.sta.threshold.authmode = password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;  // PMF is used when the access point offers it ...
    config.sta.pmf_cfg.required = false;  // ... but never required (many home routers do not)
    config.sta.bssid_set = false;
    const esp_err_t stored = esp_wifi_set_config(WIFI_IF_STA, &config);

    bool connected = false;
    if (stored == ESP_OK) {
        credentials_loaded = false;  // the cached copy of the saved network is stale now
        load_saved_credentials();
        WiFi.disconnect(false, false);
        // disconnect() only takes effect on an event: begin() before the old link is down would be ignored. Wait
        // for the status to leave WL_CONNECTED (bounded; a busy driver must not hold the serial task for long).
        const uint32_t disconnect_start = millis();
        while (WiFi.status() == WL_CONNECTED && millis() - disconnect_start < 3000) delay(50);
        WiFi.begin();  // connects with the station config stored above
        Serial.printf("[WIFI] Connecting to new network: %s\n", saved_ssid_buf);
        // Success is the requested network, not just "some link": the old association may linger for a moment.
        connected = wait_for_connection(timeout_ms) && WiFi.status() == WL_CONNECTED &&
                    WiFi.SSID() == String(ssid);
        if (!connected) Serial.printf("[WIFI] Not connected to %s\n", ssid);
    } else {
        Serial.printf("[WIFI] Storing the network failed: %s\n", esp_err_to_name(stored));
    }
    if (connected) {
        on_connected();
    } else {
        currently_connected = false;
        last_reconnect_attempt_ms = millis();
    }
    wifi_api_unlock();
    return connected;
}

// Erases the saved station config (the sequence factory_reset.cpp uses, under the Wi-Fi API lock), disconnects and
// stops the radio so no background reconnect brings the forgotten network back.
bool wifi_forget_network() {
    if (!wifi_api_mutex) wifi_api_mutex = xSemaphoreCreateRecursiveMutex();
    if (!wifi_api_lock(1000)) {
        Serial.println("[WIFI] API busy; saved network NOT forgotten");
        return false;
    }
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);  // the driver must be up to erase and read back
    const bool erased = WiFi.disconnect(true, true);      // wifioff, erase station config
    WiFi.mode(WIFI_STA);  // the driver must be up to read back the stored station config
    wifi_config_t config = {};
    const bool config_read = esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK;
    const bool ssid_empty = config_read && config.sta.ssid[0] == '\0';
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();

    currently_connected = false;
    reconnect_in_flight = false;
    s_intentional_power_off = true;
    credentials_loaded = false;
    saved_ssid_buf[0] = '\0';
    saved_password_buf[0] = '\0';
    wifi_api_unlock();

    const bool forgotten = erased && ssid_empty;
    Serial.printf("[WIFI] Saved network %s (erased=%d, config read=%d, empty ssid=%d)\n",
                  forgotten ? "forgotten" : "NOT cleared", erased, config_read, ssid_empty);
    return forgotten;
}

// Blocking scan for the Improv serial installer (the driver spends up to 300 ms on each channel). The Wi-Fi API lock
// is only taken to prepare and to restore the radio state, never across the scan itself: holding it for seconds
// would stall every Wi-Fi read and a fetch's power handling.
int32_t wifi_scan_networks(WifiScanEntry* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    if (!wifi_api_mutex) wifi_api_mutex = xSemaphoreCreateRecursiveMutex();

    if (!wifi_api_lock(500)) {
        Serial.println("[WIFI] API busy; scan skipped");
        return -1;
    }
    const bool was_off = WiFi.getMode() == WIFI_OFF;
    const bool radio_up = !was_off || WiFi.mode(WIFI_STA);
    wifi_api_unlock();
    if (!radio_up) {
        Serial.println("[WIFI] Cannot start Wi-Fi; scan skipped");
        return -1;
    }

    int16_t found = WIFI_SCAN_FAILED;
    for (int attempt = 0; attempt < kScanAttempts; ++attempt) {
        // Blocking, hidden networks left out. A scan fails while a connect/reconnect is in progress: give it a
        // moment and try again instead of reporting an empty list.
        found = WiFi.scanNetworks(false, false);
        if (found >= 0) break;
        Serial.printf("[WIFI] Scan failed (%d), attempt %d of %d\n", static_cast<int>(found), attempt + 1,
                      kScanAttempts);
        if (attempt + 1 < kScanAttempts) delay(kScanRetryDelayMs);
    }

    size_t count = 0;
    if (found > 0) {
        const size_t total = static_cast<size_t>(found);
        WifiScanEntry* candidates = static_cast<WifiScanEntry*>(malloc(total * sizeof(WifiScanEntry)));
        if (candidates) {
            for (size_t i = 0; i < total; ++i) {
                snprintf(candidates[i].ssid, sizeof(candidates[i].ssid), "%s", WiFi.SSID(i).c_str());
                candidates[i].rssi = WiFi.RSSI(i);
                candidates[i].secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
            count = wifi_scan_policy::select_networks(candidates, total, out, capacity);
            free(candidates);
        }
    }
    WiFi.scanDelete();  // free the driver's result list (also after a failed scan)

    // Restore only what this scan changed: the radio goes back off when it was off before (a fetch that started in
    // the meantime owns the radio, so wifi_power_off() leaves it alone and sets no deferred shutdown).
    if (was_off && wifi_api_lock(500)) {
        wifi_power_off();
        wifi_api_unlock();
    }
    Serial.printf("[WIFI] Scan: %u of %d network(s) listed\n", static_cast<unsigned>(count),
                  static_cast<int>(found));
    return found < 0 ? -1 : static_cast<int32_t>(count);
}

void wifi_power_off() {
    if (s_active_managed_ops.load() > 0 || s_worker_leases.load() > 0) {
        // A managed operation / fetch worker still owns the radio.
        Serial.println("[WIFI_POWER] Radio still owned; not powering off");
        return;
    }
    if (!wifi_api_lock(100)) {
        Serial.println("[WIFI_POWER] WiFi API busy; deferring power off");
        s_pending_power_off = true;  // wifi_process() retries on a later pass
        return;
    }
    // An operation may have started while we waited for the lock.
    if (s_active_managed_ops.load() > 0 || s_worker_leases.load() > 0) {
        wifi_api_unlock();
        s_pending_power_off = true;
        Serial.println("[WIFI_POWER] Radio became owned while locking; deferring power off");
        return;
    }
    s_pending_power_off = false;
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    currently_connected = false;
    s_intentional_power_off = true;
    wifi_api_unlock();
}

// Powers the radio down once the last owner (managed operation or fetch worker)
// is gone, when the user's auto-off policy says so.
static void wifi_power_off_if_idle() {
    if (s_active_managed_ops.load() > 0 || s_worker_leases.load() > 0) return;
    const AppSettings* cur_settings = settings_get();
    const int auto_off_sec = cur_settings ? cur_settings->wifi_auto_off_seconds : 30;
    if (auto_off_sec == 0 || battery_is_usb_connected()) return;
    wifi_power_off();
}

void wifi_worker_lease_acquire() {
    if (s_active_managed_ops.load() <= 0) return;  // not inside a managed operation: nothing to extend
    s_worker_leases.fetch_add(1);
}

void wifi_worker_lease_release() {
    if (s_worker_leases.fetch_sub(1) <= 0) {
        s_worker_leases.fetch_add(1);  // no lease was held
        return;
    }
    wifi_power_off_if_idle();  // the waiter's operation may already have returned
}

bool wifi_power_on(bool is_background_refresh) {
    if (!wifi_api_lock(100)) return false;
    (void)is_background_refresh;
    s_intentional_power_off = false;
    if (WiFi.status() == WL_CONNECTED) {
        on_connected();
        wifi_api_unlock();
        return true;
    }
    Serial.println("[WIFI_POWER] Turning WiFi on");
    const bool connected = connect_available_credentials();
    wifi_api_unlock();
    return connected;
}

WiFiOperationResult wifi_managed_operation(const char* operation_name,
                                           WiFiOperationCallback callback) {
    if (!s_managed_op_mutex) s_managed_op_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_managed_op_mutex) xSemaphoreTakeRecursive(s_managed_op_mutex, portMAX_DELAY);

    Serial.printf("[WIFI_MGR] Starting: %s\n", operation_name);
    s_active_managed_ops.fetch_add(1);
    s_intentional_power_off = false;

    last_operation_start_ms = millis();
    WiFiOperationResult result = WIFI_OP_SUCCESS;
    if (!wifi_power_on(false)) {
        result = WIFI_OP_WIFI_FAILED;
    }
    if (result == WIFI_OP_SUCCESS && !callback()) {
        result = WIFI_OP_OPERATION_FAILED;
    }

    int remaining_ops = s_active_managed_ops.fetch_sub(1) - 1;

    // Only power down if no other managed operation or fetch worker is still using the radio
    if (remaining_ops <= 0) {
        wifi_power_off_if_idle();
    }

    last_operation_duration_ms = millis() - last_operation_start_ms;
    total_wifi_on_time_ms += last_operation_duration_ms;
    Serial.printf("[WIFI_MGR] Finished: %s (%lu ms, result %d)\n",
                  operation_name, last_operation_duration_ms, result);

    if (s_managed_op_mutex) xSemaphoreGiveRecursive(s_managed_op_mutex);
    return result;
}

unsigned long wifi_get_last_operation_duration() {
    return last_operation_duration_ms;
}

unsigned long wifi_get_total_on_time() { return total_wifi_on_time_ms; }