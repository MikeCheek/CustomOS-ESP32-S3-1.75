/*
 * hal_vibrate.h
 * Vibration motor control via LEDC PWM. Feature-gated on FEATURE_VIBRATE
 * and PIN_VIBRATE - both must be enabled/valid for the motor to work.
 * All functions are safe to call when the feature is disabled (no-ops).
 */

#pragma once
#include <stdint.h>

// Call once from setup(). No-op if FEATURE_VIBRATE is 0 or PIN_VIBRATE < 0.
void vibrate_init();

// Non-blocking vibration for `duration_ms` milliseconds. Safe to call
// repeatedly (cancels any in-progress vibration).
void vibrate_ms(uint16_t duration_ms);

// Call from loop() to turn off vibration when time expires.
void vibrate_update();

// Blocking convenience for short buzzes (e.g. button feedback).
// Returns immediately if feature is disabled.
void vibrate_buzz();
