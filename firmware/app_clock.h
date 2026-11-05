/*
 * app_clock.h
 * The everyday clock apps:
 *   Timer       presets (1 min ... 2 h) or a custom length; pause, cancel.
 *               The same timer the assistant sets (app_assistant.h).
 *   Stopwatch   start / stop, laps (the best and worst marked); it keeps
 *               running with the app closed.
 *   Alarms      up to 8, each once or on chosen weekdays; snooze 9 min.
 *               Saved in NVS, they ring awake or asleep.
 *   World clock the time in chosen cities (30 built in, daylight saving
 *               worked out on the watch); the watch's own offset from UTC
 *               comes from the phone ({"t":"tz","o":<minutes>}).
 */
#pragma once
#include "ui.h"
#include <stdint.h>

void clock_init();                 // setup(): alarms and cities from NVS
void clock_update();               // loop(), awake or asleep: rings alarms

void clock_set_utc_offset(int minutes);   // phone link "tz"
int  clock_utc_offset();                  // minutes east of UTC (-1440 = unknown)

bool stopwatch_running();
uint32_t stopwatch_ms();
// The next alarm that will ring: minutes from now (false: none on).
bool clock_next_alarm(uint32_t &minutes_from_now, int &hour, int &minute);
bool clock_alarm_ringing();
int  clock_world_times(const char **names, int *hour, int *minute, int max);   // the cities shown, time there now

extern Screen timer_screen;
extern Screen stopwatch_screen;
extern Screen alarms_screen;
extern Screen alarm_ring_screen;
extern Screen worldclock_screen;
