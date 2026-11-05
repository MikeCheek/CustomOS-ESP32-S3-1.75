/*
 * app_workout.h
 * Workout app: pick a type (outdoor / indoor walk and run, cycling, hiking,
 * strength, HIIT, other), a 3-2-1, then live time, calories, distance,
 * pace and steps; tap for Pause / End, then a summary.
 *
 *   distance   GPS on the -G board for outdoor types (fixes with a good
 *              HDOP, jumps ignored), else steps x stride
 *   calories   the type's MET for the active time (~70 kg), and every
 *              minute counts toward the Exercise ring (fitness.h)
 *   detection  10 minutes of brisk walking or running without a workout:
 *              "Record a workout?" (counting from when it started);
 *              15 still minutes in one: "Still working out?"
 *
 * The last 20 are kept in NVS and sent to the phone:
 *   {"e":"wo","ty":"Outdoor Run","st":<local epoch>,"du":1800,"m":5230,"kc":310,"stp":4800,"gps":1}
 * which answers {"t":"woack","st":<same>}; unsent ones go again on connect.
 */
#pragma once
#include "ui.h"
#include "motion_detect.h"

void workout_init();
void workout_update();                         // loop()
void workout_feed(Activity a, int cadence);    // every 5 s window (motion_update)
void workout_on_ack(uint32_t start);           // phone link "woack"
void workout_send_pending();                   // phone link "hi"

bool workout_active();
bool workout_paused();
uint32_t workout_elapsed_s();
float workout_km();
const char *workout_type_name();

extern Screen workout_screen;        // the types (or the live screen when one is running)
extern Screen workout_live_screen;
