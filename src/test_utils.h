#pragma once
#include <lvgl.h>
#include "ui_state.h"

// Button lookup functions
lv_obj_t* test_get_button_by_id(const char* button_id);
lv_obj_t* test_get_button_by_index(const char* screen, int index);

// Button click simulation
bool test_click_button(lv_obj_t* btn);

// State utilities
const char* test_get_state_string();
UiState test_parse_state(const char* state_str);
