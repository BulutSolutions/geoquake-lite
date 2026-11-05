#ifndef GEOQUAKE_MAP_STORAGE_TEST
#define GEOQUAKE_MAP_STORAGE_TEST 0
#endif

#include "serial_commands.h"
#include "audio_manager.h"
#include "improv_serial.h"
#include "sd_card.h"
#include "test_utils.h"
#include "ui_display.h"
#include "ui_state.h"
#if GEOQUAKE_MAP_STORAGE_TEST
#include "lv_fs_flash.h"
#endif
#include <esp32-hal-psram.h>

extern void map_tiles_refresh_availability(void);
extern bool map_tiles_available(void);
extern void map_tiles_set_mode(bool enabled);
extern void map_tiles_show(void);
extern void map_redraw(void);

// Priority queue for serial commands (separate from UI queue)
static SerialCommand serial_cmd_queue[SERIAL_CMD_QUEUE_SIZE];
static volatile int serial_queue_head = 0;
static volatile int serial_queue_tail = 0;
static SemaphoreHandle_t serial_queue_mutex = NULL;

// Input buffer
static char serial_input_buffer[SERIAL_INPUT_BUFFER_SIZE];
static int serial_buffer_pos = 0;

// Last command tracking
static char last_cmd[32] = "none";

// Queue operations (thread-safe)
bool serial_queue_command(const SerialCommand& cmd) {
    if (xSemaphoreTake(serial_queue_mutex, portMAX_DELAY)) {
        int next = (serial_queue_head + 1) % SERIAL_CMD_QUEUE_SIZE;
        if (next == serial_queue_tail) {
            xSemaphoreGive(serial_queue_mutex);
            return false;  // Queue full
        }

        serial_cmd_queue[serial_queue_head] = cmd;
        serial_queue_head = next;

        xSemaphoreGive(serial_queue_mutex);
        return true;
    }
    return false;
}

bool serial_dequeue_command(SerialCommand& cmd) {
    if (xSemaphoreTake(serial_queue_mutex, 0)) {  // Non-blocking
        if (serial_queue_head == serial_queue_tail) {
            xSemaphoreGive(serial_queue_mutex);
            return false;  // Queue empty
        }

        cmd = serial_cmd_queue[serial_queue_tail];
        serial_queue_tail = (serial_queue_tail + 1) % SERIAL_CMD_QUEUE_SIZE;

        xSemaphoreGive(serial_queue_mutex);
        return true;
    }
    return false;
}

// Command execution (called from lvgl_task inside mutex)
void serial_execute_command(const SerialCommand& cmd) {
    switch (cmd.type) {
        case SERIAL_CMD_SET_STATE: {
            UiState state = test_parse_state(cmd.param1);
            if (state != UI_STATE_BOOT_SPLASH || strcmp(cmd.param1, "BOOT_SPLASH") == 0) {
                ui_set_state(state);
                Serial.printf("[SERIAL] State changed: %s\n", cmd.param1);
                strncpy(last_cmd, "setState", sizeof(last_cmd));
            } else {
                Serial.printf("[SERIAL] ERROR: Invalid state '%s'\n", cmd.param1);
            }
            break;
        }

        case SERIAL_CMD_CLICK_BUTTON: {
            lv_obj_t* btn = test_get_button_by_id(cmd.param1);
            if (btn && test_click_button(btn)) {
                Serial.printf("[SERIAL] Button clicked: %s\n", cmd.param1);
                strncpy(last_cmd, "click", sizeof(last_cmd));
            } else {
                Serial.printf("[SERIAL] ERROR: Button '%s' not found\n", cmd.param1);
                Serial.println("[SERIAL] Safe mode: Returning to MAIN");
                ui_set_state(UI_STATE_MAIN);
            }
            break;
        }

        case SERIAL_CMD_CLICK_INDEX: {
            lv_obj_t* btn = test_get_button_by_index(cmd.param1, cmd.param2);
            if (btn && test_click_button(btn)) {
                Serial.printf("[SERIAL] Button clicked: screen=%s, index=%d\n",
                              cmd.param1, cmd.param2);
                strncpy(last_cmd, "clickIndex", sizeof(last_cmd));
            } else {
                Serial.printf("[SERIAL] ERROR: Button not found: screen=%s, index=%d\n",
                              cmd.param1, cmd.param2);
                Serial.println("[SERIAL] Safe mode: Returning to MAIN");
                ui_set_state(UI_STATE_MAIN);
            }
            break;
        }

        case SERIAL_CMD_GET_STATE: {
            const char* state_str = test_get_state_string();
            int heap = ESP.getFreeHeap();
            int psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            unsigned long uptime = millis();

            // Send JSON response
            Serial.printf("{\"state\":\"%s\",\"heap\":%d,\"psram\":%d,\"uptime\":%lu,\"lastCmd\":\"%s\"}\n",
                          state_str, heap, psram, uptime, last_cmd);
            strncpy(last_cmd, "getState", sizeof(last_cmd));
            break;
        }

        case SERIAL_CMD_PING: {
            Serial.println("[SERIAL] PONG");
            strncpy(last_cmd, "ping", sizeof(last_cmd));
            break;
        }

        case SERIAL_CMD_PLAY_TEST_TONE: {
            if (audio_play_test_tone()) {
                Serial.println("[SERIAL] Audio test tone queued");
                strncpy(last_cmd, "play_test_tone", sizeof(last_cmd));
            } else {
                Serial.println("[SERIAL] ERROR: Audio test tone failed");
            }
            break;
        }

        case SERIAL_CMD_SIMULATE_NO_SD_TILES: {
            sd_set_simulate_no_tiles(true);
            map_tiles_set_mode(false);  // the tile source changes only while the tile cache is down
            map_tiles_refresh_availability();
            if (map_tiles_available()) {
                map_tiles_set_mode(true);
                map_tiles_show();
                map_redraw();
                Serial.println("[SERIAL] DEBUG: simulate_no_sd_tiles active; using fallback tiles if present");
            } else {
                map_tiles_set_mode(false);
                Serial.println("[SERIAL] DEBUG: simulate_no_sd_tiles active; no fallback tiles found");
            }
            strncpy(last_cmd, "simulate_no_sd_tiles", sizeof(last_cmd));
            break;
        }

    }
}

// Process queue (called from lvgl_task)
void serial_process_command_queue() {
    SerialCommand cmd;
    while (serial_dequeue_command(cmd)) {
        serial_execute_command(cmd);
    }
}

// JSON parser
void serial_parse_and_queue_command(const char* json_str) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json_str);

    if (error) {
        Serial.printf("[SERIAL] ERROR: Invalid JSON - %s\n", error.c_str());
        return;
    }

    const char* cmd_str = doc["cmd"];
    if (!cmd_str) {
        Serial.println("[SERIAL] ERROR: Missing 'cmd' field");
        return;
    }

    SerialCommand cmd;

    if (strcmp(cmd_str, "setState") == 0) {
        cmd.type = SERIAL_CMD_SET_STATE;
        const char* state = doc["state"];
        if (!state) {
            Serial.println("[SERIAL] ERROR: Missing 'state' field");
            return;
        }
        strncpy(cmd.param1, state, sizeof(cmd.param1));
    }
    else if (strcmp(cmd_str, "click") == 0) {
        cmd.type = SERIAL_CMD_CLICK_BUTTON;
        const char* button = doc["button"];
        if (!button) {
            Serial.println("[SERIAL] ERROR: Missing 'button' field");
            return;
        }
        strncpy(cmd.param1, button, sizeof(cmd.param1));
    }
    else if (strcmp(cmd_str, "clickIndex") == 0) {
        cmd.type = SERIAL_CMD_CLICK_INDEX;
        const char* screen = doc["screen"];
        int index = doc["index"] | -1;
        if (!screen || index < 0) {
            Serial.println("[SERIAL] ERROR: Missing 'screen' or 'index' field");
            return;
        }
        strncpy(cmd.param1, screen, sizeof(cmd.param1));
        cmd.param2 = index;
    }
    else if (strcmp(cmd_str, "getState") == 0) {
        cmd.type = SERIAL_CMD_GET_STATE;
    }
    else if (strcmp(cmd_str, "ping") == 0) {
        cmd.type = SERIAL_CMD_PING;
    }
    else if (strcmp(cmd_str, "play_test_tone") == 0) {
        cmd.type = SERIAL_CMD_PLAY_TEST_TONE;
    }
    else if (strcmp(cmd_str, "simulate_no_sd_tiles") == 0) {
        cmd.type = SERIAL_CMD_SIMULATE_NO_SD_TILES;
    }
#if GEOQUAKE_MAP_STORAGE_TEST
    else if (strcmp(cmd_str, "map_test_reset") == 0) {
        // Map storage check: runs here, not queued. The tool resets the chip right after, so the tile manager never
        // reads the removed file.
        flash_map_test_remove_pack();
        return;
    }
#endif
    else {
        Serial.printf("[SERIAL] ERROR: Unknown command '%s'\n", cmd_str);
        return;
    }

    // Queue command
    if (!serial_queue_command(cmd)) {
        Serial.println("[SERIAL] ERROR: Command queue full");
    }
}

// One byte of a JSON command line (the line buffer the task keeps between reads).
static void serial_line_byte(uint8_t byte) {
    if (byte == '\n' || byte == '\r') {
        if (serial_buffer_pos > 0) {
            serial_input_buffer[serial_buffer_pos] = '\0';
            // Parse and queue command
            serial_parse_and_queue_command(serial_input_buffer);

            serial_buffer_pos = 0;
        }
    } else if (serial_buffer_pos < SERIAL_INPUT_BUFFER_SIZE - 1) {
        serial_input_buffer[serial_buffer_pos++] = static_cast<char>(byte);
    } else {
        // Buffer overflow
        Serial.println("[SERIAL] ERROR: Input buffer overflow");
        serial_buffer_pos = 0;
    }
}

// Serial input task (Core 0, low priority)
void serial_command_task(void* parameter) {
    Serial.println("[SERIAL] Command task started on Core 0");

    // Bytes offered to the Improv parser while its header is still undecided. A valid packet clears them; a
    // rejected prefix is replayed into the JSON line buffer in order, so a JSON command is never lost.
    static uint8_t improv_pending[272];
    static size_t improv_pending_len = 0;

    while (true) {
        while (Serial.available()) {
            const uint8_t byte = static_cast<uint8_t>(Serial.read());

            // Improv Wi-Fi first, but only at the start of a line ('I'): JSON commands start with '{', so a command
            // line is never cut in half, and the bytes of an undecided header are replayed when it was not Improv.
            if (improv_pending_len > 0 || improv_should_parse(serial_buffer_pos == 0, byte)) {
                if (improv_serial_feed_byte(byte)) {
                    if (improv_serial_packet_finished()) {
                        improv_pending_len = 0;  // a complete packet: those bytes were not JSON text
                    } else if (improv_pending_len < sizeof(improv_pending)) {
                        improv_pending[improv_pending_len++] = byte;
                    }
                    continue;
                }
                for (size_t i = 0; i < improv_pending_len; ++i) serial_line_byte(improv_pending[i]);
                improv_pending_len = 0;
            }
            serial_line_byte(byte);
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // Check every 10ms
    }
}

// Initialization
void serial_commands_init() {
    // Create mutex
    serial_queue_mutex = xSemaphoreCreateMutex();

    // Create task on Core 0 with low priority (0). The stack also covers the Improv serial SDK (std::string and
    // std::vector per RPC) and one WIFI_SETTINGS connect.
    xTaskCreatePinnedToCore(
        serial_command_task,
        "SerialCmd",
        8192,  // Stack size
        NULL,
        0,     // Priority 0 (lowest)
        NULL,
        0      // Core 0
    );

    Serial.println("[SERIAL] Command interface initialized");
    Serial.println("[SERIAL] Send JSON commands terminated by newline");
    Serial.println("[SERIAL] Example: {\"cmd\":\"ping\"}");
}
