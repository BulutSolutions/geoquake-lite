#include "factory_reset.h"

#include "settings_manager.h"
#include "wifi_manager.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace {

bool clear_namespace(const char* name) {
    Preferences prefs;
    if (!prefs.begin(name, false)) {
        Serial.printf("[FACTORY_RESET] Could not open namespace %s\n", name);
        return false;
    }
    const bool cleared = prefs.clear();
    prefs.end();
    Serial.printf("[FACTORY_RESET] Namespace %s: %s\n", name,
                  cleared ? "cleared" : "clear failed");
    return cleared;
}

}  // namespace

bool geoquake_factory_reset() {
    Serial.println("[FACTORY_RESET] GeoQuake Lite reset started");

    // This clears gql_cfg and writes known defaults back. It does not touch the map filesystem.
    settings_reset_to_defaults(false);

    bool all_ok = true;
    const char* namespaces[] = {
        "gql_cfg",
        "gql_theme",
        "gql_auto_dim",
        "gql_time",
        "gql_ui",
    };
    for (const char* name : namespaces) {
        all_ok = clear_namespace(name) && all_ok;
    }

    // Forget the saved Wi-Fi network even when the radio is off: WiFi.mode() brings
    // the driver up (Arduino owns esp_wifi_init); disconnect(true, true) stops the
    // station and erases the stored credentials. The whole erase + readback runs
    // under the Wi-Fi API lock so a background reconnect cannot interleave with it.
    bool wifi_forgotten = false;
    if (!wifi_api_lock(1000)) {
        Serial.println("[FACTORY_RESET] Wi-Fi API busy; credentials NOT cleared");
    } else {
        const bool wifi_up = WiFi.mode(WIFI_STA);
        const bool erased = wifi_up && WiFi.disconnect(true, true);

        WiFi.mode(WIFI_STA);  // the driver must be up to read back the stored station config
        wifi_config_t config = {};
        const bool config_read = esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK;
        const bool ssid_empty = config_read && config.sta.ssid[0] == '\0';
        WiFi.mode(WIFI_OFF);
        wifi_api_unlock();

        wifi_forgotten = erased && ssid_empty;
        Serial.printf("[FACTORY_RESET] Saved Wi-Fi %s (erased=%d, config read=%d, empty ssid=%d)\n",
                      wifi_forgotten ? "forgotten" : "NOT cleared", erased, config_read, ssid_empty);
    }

    return all_ok && wifi_forgotten;
}