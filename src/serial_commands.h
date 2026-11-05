#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#define SERIAL_CMD_QUEUE_SIZE 10
#define SERIAL_INPUT_BUFFER_SIZE 256

enum SerialCmdType {
    SERIAL_CMD_SET_STATE,
    SERIAL_CMD_CLICK_BUTTON,
    SERIAL_CMD_CLICK_INDEX,
    SERIAL_CMD_GET_STATE,
    SERIAL_CMD_PING,
    SERIAL_CMD_PLAY_TEST_TONE,
    SERIAL_CMD_SIMULATE_NO_SD_TILES,
};

struct SerialCommand {
    SerialCmdType type;
    char param1[32];  // state name, button ID, or screen name
    int param2;       // index for clickIndex
};

// Queue operations
bool serial_queue_command(const SerialCommand& cmd);
bool serial_dequeue_command(SerialCommand& cmd);
void serial_process_command_queue();

// Task entry point
void serial_command_task(void* parameter);
void serial_commands_init();
