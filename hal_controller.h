/*
 * hal_controller.h
 * Generic BLE game-controller input: a 4-direction d-pad plus 4
 * buttons (A/B/X/Y), sent from the companion app over BLE (see
 * hal_ble.cpp's controller characteristic). Reusable by ANY game or
 * by the OS's own menu navigation - this module only mirrors raw
 * state, it doesn't know anything about what's consuming it.
 *
 * Edge detection (press vs held) is left to each caller, the same
 * way every game in this project already does its own touch
 * press/release edge tracking - no separate global "tick" hook
 * needed here, just a state mirror plus a connection timeout.
 */
#pragma once
#include <Arduino.h>

#define CTRL_UP    0x01
#define CTRL_DOWN  0x02
#define CTRL_LEFT  0x04
#define CTRL_RIGHT 0x08

#define CTRL_BTN_A 0x01
#define CTRL_BTN_B 0x02
#define CTRL_BTN_X 0x04
#define CTRL_BTN_Y 0x08

// Called by the BLE write callback whenever a new packet arrives.
void controller_set_state(uint8_t dpad, uint8_t buttons);

// True if a controller packet has arrived within the last 3 seconds -
// callers should treat "not connected" as "ignore controller input,
// touch still works normally", not as an error state.
bool controller_connected();

bool controller_dpad(uint8_t bit);
bool controller_button(uint8_t bit);

// Normalized movement vector from the current d-pad state (supports
// diagonals - e.g. up+left held together), (0,0) if nothing held.
void controller_get_direction(float *dx, float *dy);
