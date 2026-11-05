/**
 * Improv Wi-Fi over USB serial (improv-wifi/sdk-serial-js reader, SDK from improv-wifi/sdk-cpp).
 *
 * The SDK parses the serial framing ("IMPROV" | version | type | length | data | checksum); the browser reads the
 * same thing line by line. This file answers the RPCs the browser sends (REQUEST_CURRENT_STATE, REQUEST_INFO,
 * SEND_WIFI_SETTINGS) and stores the credentials through wifi_manager, which persists them exactly like the app's
 * own station config.
 *
 * Wire rules (sdk-serial-js/src/serial.ts):
 * - every packet write starts with '\n' in the SAME Serial.write, so a packet never lands mid-line after another
 *   task's log text;
 * - REQUEST_CURRENT_STATE is answered with the state AND an RPC result (empty; the browser reads the next URL from
 *   it when PROVISIONED);
 * - a new command clears the previous error (ERROR_NONE);
 * - a corrupt frame answers ERROR_INVALID_RPC and is consumed (it is Improv traffic, not a JSON command).
 *
 * Threads: everything here runs on serial_command_task (Core 0). A SEND_WIFI_SETTINGS request waits up to
 * kImprovWifiTimeoutMs for the connection: the serial task may block there, the LVGL thread is not involved.
 */

#include "improv_serial.h"

#include "version.h"
#include "wifi_manager.h"

#include <improv.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

constexpr size_t kPacketBufferSize = 272;  // 6 + 3 header bytes + 255 data + 1 checksum, rounded
constexpr size_t kFrameBytes = 6 + 3 + 255 + 1;
constexpr uint32_t kWriteLockTimeoutMs = 1000;
constexpr uint32_t kImprovWifiTimeoutMs = 20000;      // the browser gives up after ~30 s
constexpr const char* kDeviceName = "GeoQuake Lite";  // firmware name and device name
constexpr const char* kChipName = "ESP32-S3";

uint8_t s_packet[kPacketBufferSize];
size_t s_position = 0;
uint8_t s_state = improv::STATE_AUTHORIZED;
bool s_packet_handled = false;    // the callback answered the byte just fed: the packet is complete
bool s_packet_finished = false;   // a complete packet was consumed (cleared by improv_serial_packet_finished())
bool s_packet_corrupt = false;    // the parser rejected the frame's checksum
SemaphoreHandle_t s_write_mutex = nullptr;

// One line + one whole packet in a single Serial.write(): the browser never sees a log line inside a frame, and a
// packet never continues a log line printed by another task.
void write_frame(uint8_t type, const uint8_t* data, size_t len) {
    if (len > 255) return;
    uint8_t frame[1 + kFrameBytes];
    frame[0] = '\n';
    uint8_t* packet = frame + 1;
    size_t n = 0;
    memcpy(packet, "IMPROV", 6);
    n = 6;
    packet[n++] = improv::IMPROV_SERIAL_VERSION;
    packet[n++] = type;
    packet[n++] = static_cast<uint8_t>(len);
    if (len) memcpy(packet + n, data, len);
    n += len;
    uint8_t checksum = 0;  // the browser sums the packet without the leading newline
    for (size_t i = 0; i < n; ++i) checksum = static_cast<uint8_t>(checksum + packet[i]);
    packet[n++] = checksum;

    if (!s_write_mutex) s_write_mutex = xSemaphoreCreateMutex();
    const bool locked =
        !s_write_mutex || xSemaphoreTake(s_write_mutex, pdMS_TO_TICKS(kWriteLockTimeoutMs)) == pdTRUE;
    Serial.write(frame, n + 1);  // newline + packet at once
    Serial.flush();
    if (locked && s_write_mutex) xSemaphoreGive(s_write_mutex);
}

void send_state(uint8_t state) { write_frame(improv::TYPE_CURRENT_STATE, &state, 1); }

void send_error(improv::Error error) {
    const uint8_t code = static_cast<uint8_t>(error);
    write_frame(improv::TYPE_ERROR_STATE, &code, 1);
}

void send_rpc_response(improv::Command command, const std::vector<std::string>& data) {
    // No inner checksum: the serial framing already carries one, and the browser parses the payload as a plain
    // list of length-prefixed strings (improv-wifi/sdk-serial-js).
    const std::vector<uint8_t> body = improv::build_rpc_response(command, data, false);
    if (!body.empty()) write_frame(improv::TYPE_RPC_RESPONSE, body.data(), body.size());
}

// AUTHORIZED while the board has no network, PROVISIONED once Wi-Fi is up (the browser reads this after a flash).
uint8_t current_state() {
    if (wifi_is_connected()) return improv::STATE_PROVISIONED;
    return s_state == improv::STATE_PROVISIONING ? improv::STATE_PROVISIONING : improv::STATE_AUTHORIZED;
}

void handle_wifi_settings(const improv::ImprovCommand& command) {
    // Same limits as wifi_manager (wifi_config_t's ssid[32] / password[64], no NUL when full; WPA passphrase is
    // 8..63, a 64-char hex PSK is 64, an empty password is an open network).
    const size_t password_length = command.password.size();
    if (command.ssid.empty() || command.ssid.size() > 32 || password_length > 64 ||
        (password_length > 0 && password_length < 8)) {
        Serial.printf("[IMPROV] Rejected Wi-Fi settings (ssid %u B, password %u B)\n",
                      static_cast<unsigned>(command.ssid.size()), static_cast<unsigned>(password_length));
        send_error(improv::ERROR_INVALID_RPC);
        return;
    }
    Serial.printf("[IMPROV] Wi-Fi settings for \"%s\"\n", command.ssid.c_str());
    s_state = improv::STATE_PROVISIONING;
    send_state(s_state);

    if (wifi_connect_new_credentials(command.ssid.c_str(), command.password.c_str(), kImprovWifiTimeoutMs)) {
        s_state = improv::STATE_PROVISIONED;
        send_state(s_state);
        send_rpc_response(improv::WIFI_SETTINGS, std::vector<std::string>());  // no redirect URL
        Serial.printf("[IMPROV] Connected to \"%s\"\n", command.ssid.c_str());
    } else {
        s_state = improv::STATE_AUTHORIZED;
        send_error(improv::ERROR_UNABLE_TO_CONNECT);
        send_state(s_state);  // the browser drops PROVISIONING again
        Serial.printf("[IMPROV] Could not connect to \"%s\"\n", command.ssid.c_str());
    }
}

bool handle_command(improv::ImprovCommand command) {
    s_packet_handled = true;  // the parser found the checksum of a complete RPC: this byte ends the packet
    // A new command clears the error the previous one left (the browser keeps it until then).
    send_error(improv::ERROR_NONE);
    switch (command.command) {
        case improv::GET_CURRENT_STATE: {
            // State first, then the result for this command: the browser settles the state event either way and
            // reads the next URL from the result when PROVISIONED (an empty result means "no URL").
            const uint8_t state = current_state();
            send_state(state);
            send_rpc_response(improv::GET_CURRENT_STATE, std::vector<std::string>());
            return true;
        }
        case improv::GET_DEVICE_INFO: {
            const std::vector<std::string> info = {kDeviceName, FIRMWARE_VERSION_STRING, kChipName, kDeviceName};
            send_rpc_response(improv::GET_DEVICE_INFO, info);
            return true;
        }
        case improv::GET_WIFI_NETWORKS: {
            // The browser shows this list instead of only "Join other...": one result packet per network, then an
            // empty result for the same command ends the list (improv-wifi/sdk-serial-js, serial.ts).
            WifiScanEntry entries[wifi_scan_policy::kMaxNetworks];
            const int32_t count = wifi_scan_networks(entries, wifi_scan_policy::kMaxNetworks);
            if (count < 0) {  // a connect/reconnect kept the radio busy: report it, do not show an empty list
                send_error(improv::ERROR_UNKNOWN);
                return true;
            }
            for (int32_t i = 0; i < count; ++i) {
                char rssi[12];
                snprintf(rssi, sizeof(rssi), "%ld", static_cast<long>(entries[i].rssi));
                const std::vector<std::string> network = {entries[i].ssid, rssi,
                                                          entries[i].secured ? "YES" : "NO"};
                send_rpc_response(improv::GET_WIFI_NETWORKS, network);
            }
            send_rpc_response(improv::GET_WIFI_NETWORKS, std::vector<std::string>());  // end of the list
            return true;
        }
        case improv::WIFI_SETTINGS:
            handle_wifi_settings(command);
            return true;
        default:
            send_error(improv::ERROR_UNKNOWN_RPC);
            return true;
    }
}

void handle_parser_error(improv::Error error) {
    // A frame with a bad checksum: tell the browser instead of dropping it silently.
    Serial.printf("[IMPROV] Bad packet (error 0x%02X)\n", static_cast<unsigned>(error));
    s_packet_corrupt = true;
    send_error(error == improv::ERROR_INVALID_RPC ? improv::ERROR_INVALID_RPC : error);
}

}  // namespace

bool improv_serial_feed_byte(uint8_t byte) {
    if (improv::parse_improv_serial_byte(s_position, byte, s_packet, handle_command, handle_parser_error)) {
        if (s_position < sizeof(s_packet)) s_packet[s_position] = byte;
        ++s_position;
        if (s_packet_handled) {
            // A complete packet was answered: the next byte starts a new one (it may follow within microseconds).
            s_packet_handled = false;
            s_packet_finished = true;
            s_position = 0;
        } else if (s_position >= sizeof(s_packet)) {
            s_position = 0;  // malformed length: start over
        }
        return true;
    }
    s_position = 0;
    if (s_packet_corrupt) {
        s_packet_corrupt = false;
        s_packet_finished = true;  // the caller drops its held header bytes: they were part of a bad packet
        return true;  // a corrupt Improv frame: Improv traffic, not a JSON command byte
    }
    return false;  // not (or no longer) an Improv packet: the byte belongs to the JSON line handling
}

bool improv_serial_packet_finished() {
    const bool finished = s_packet_finished;
    s_packet_finished = false;
    return finished;
}
