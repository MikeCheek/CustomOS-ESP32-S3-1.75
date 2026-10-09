/*
 * aod.h
 * Always-on display: while the watch "sleeps", the panel stays on at a
 * very low brightness showing a sparse clock (a few % of pixels lit,
 * nudged a few pixels every minute against burn-in), redrawn once a
 * minute. The AMOLED holds the image in its own RAM, so the CPU still
 * sleeps in between.
 *
 * Setting: Settings > Always-on display (off by default). Not below 20 %
 * battery unless charging - then the screen just turns off as before.
 */
#pragma once

// hal_sleep calls this instead of display_sleep(): AOD if enabled and
// allowed, otherwise the panel off.
void aod_sleep_display();

// Leaving sleep: back to the user's brightness (hal_sleep, before
// display_wakeup()).
void aod_wake();

bool aod_active();

// A frame went to the panel in the last moment: don't light-sleep in the
// middle of its QSPI transfer.
bool aod_sending();

// Asleep loop: redraws on the minute, drops to panel-off on low battery.
void aod_update();
