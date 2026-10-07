/*
 * hal_sleep.h
 * Idle-timeout sleep with touch/motion wake, configurable in Settings
 * (see app_settings_state.h). The display is fully hardware-powered off
 * while asleep (hal_display.cpp's display_sleep()), not just dimmed.
 *
 * While asleep, loop() should skip the normal LVGL pump (display_lvgl_*,
 * and by extension touch_register_lvgl_indev()'s automatic polling)
 * entirely - sleep_update() does its own lightweight, rate-limited
 * touch/motion checks independently so a wake-tap never also reaches
 * LVGL as a click on whatever was on screen when it went to sleep.
 */

#pragma once

#include <stdint.h>

void sleep_init();

// Call once per loop() iteration, always (whether currently asleep or
// not). Handles the idle-timeout -> sleep transition, and, while
// asleep, checks for a configured wake trigger (touch/motion) and wakes
// if one fires. Returns true if the device is asleep *after* this call.
bool sleep_update();

// Call whenever the caller knows real user activity just happened
// (e.g. a physical button press) to reset the idle timer / immediately
// wake if asleep, without waiting for sleep_update()'s own detection.
void sleep_register_activity();

bool sleep_is_asleep();

// Milliseconds since the last registered activity (touch, button, or
// sleep_register_activity() called directly) - for screens that want
// to throttle their own render rate down during genuinely idle
// stretches without touching the sleep/wake mechanism itself. See
// Screen::idle_frame_ms in ui.h.
uint32_t sleep_ms_since_activity();

// User-requested immediate sleep (e.g. PWR button short-press while
// awake).
void sleep_force_sleep();
