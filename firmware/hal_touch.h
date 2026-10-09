/*
 * hal_touch.h
 * CST9217 capacitive touch controller over I2C.
 */

#pragma once
#include <stdint.h>

struct TouchPoint {
    bool touched;
    uint16_t x;
    uint16_t y;
};

void touch_init();
TouchPoint touch_read();
bool touch_is_currently_touched();

// Raw (untransformed) touch read for app_calibration.cpp's use only.
// Returns true if a finger is currently on the glass; raw_x/raw_y are
// the CST9217's native coordinate space, before the calibration
// transform touch_read() applies. Doesn't touch any of touch_read()'s
// debounce/swipe/pending state, so it's safe to poll independently.
bool touch_read_raw(uint16_t &raw_x, uint16_t &raw_y);

// Set gesture mode: EDGE = edge-only swipes, FREE = swipes from anywhere, NONE = disabled
// Must be called whenever the active screen changes.
void touch_set_gesture_mode(uint8_t mode);

// Quick repeated taps on the same spot (multi-tap keypads): a new press
// right after a release counts instead of being taken for a glitch.
void touch_set_fast_taps(bool on);

bool touch_swipe_right_detected();
bool touch_swipe_down_detected();
bool touch_swipe_left_detected();
bool touch_swipe_up_detected();

// Clear any pending swipe flags. Call this when a screen is actively
// scrolling/dragging to prevent gesture detection from firing.
void touch_cancel_swipes();
