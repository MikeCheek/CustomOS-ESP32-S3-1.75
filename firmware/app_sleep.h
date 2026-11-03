/*
 * app_sleep.h
 * Sleep app: movement-based sleep tracking (sleep_track.h) between 20:00
 * and 12:00 - the last night as a chart with time asleep, how much of it
 * was still (deep) and the minutes awake - and a smart alarm that rings
 * up to 30 minutes early when you stir, at the set time at the latest
 * (snooze 9 min). Settings: sleep_track, alarm_on, alarm_min.
 */
#pragma once
#include "ui.h"

void sleeptrack_update();      // loop(), awake or asleep
bool sleep_alarm_ringing();
void sleep_load_demo();        // the simulator's screenshot only
extern Screen sleep_screen;
extern Screen sleep_alarm_screen;
