/*
 * app_activity.h
 * Motion, all from the accelerometer's FIFO (every sample, awake or
 * asleep) - one reader for everything that needs it:
 *   fall detection (app_fall.h), activity tracking (still / walking /
 *   running / moving, minutes today), the rep counter, and "flick to go
 *   back" - the rules are in motion_detect.h.
 * Activity app (Tools): what you're doing now, today's minutes, and
 * "Count reps" for exercises.
 */
#pragma once
#include "ui.h"

void motion_update();          // loop(), awake or asleep
extern Screen activity_screen;
