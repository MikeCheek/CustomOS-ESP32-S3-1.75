/*
 * fitness.h
 * Activity rings, like the Apple Watch's three:
 *   Move      active calories (from what the accelerometer says you're
 *             doing every 5 s - walking, running, moving - or a workout)
 *   Exercise  minutes of brisk walking (>= 100 steps/min), running, or
 *             a workout
 *   Stand     hours with at least a minute up and moving
 * Daily goals (Settings > Activity), a celebration when one closes, a
 * reminder at ten to the hour when you haven't moved in it yet, and the
 * last 7 days kept in NVS (sent to the phone with the status report).
 *
 * The movement itself comes from motion_update() (app_activity.cpp).
 */
#pragma once
#include <stdint.h>
#include "motion_detect.h"
#include "ui.h"

struct FitDay {
    uint16_t move_kcal;
    uint16_t exercise_min;
    uint8_t  stand_hours;
    uint8_t  pad;
    uint32_t steps;
    uint32_t date;          // yyyymmdd
};

void fitness_init();
void fitness_update();                          // loop(): day change, saving, stand reminder
void fitness_feed(Activity a, int cadence);     // every 5 s window (motion_update)

// A workout running: every window counts as exercise, calories by its MET (x10).
void fitness_workout(bool on, int met_x10);

float    fitness_move_kcal();
// The wearer (Settings > Activity, or the app): weight for calories, height
// for the stride (x height/175 cm on each workout type's stride).
float    fitness_kg();
float    fitness_stride_scale();
uint32_t fitness_exercise_min();
int      fitness_stand_hours();
uint16_t fitness_move_goal();
uint16_t fitness_exercise_goal();
uint8_t  fitness_stand_goal();
// The days before today, newest first (n <= 7).
int fitness_history(FitDay *out, int max);   // newest first, up to 90 days
void fitness_send_history();                 // the days past the status report's 7, to the phone

// Three rings centred on (cx, cy): outer radius r, each `thick` wide.
void fitness_draw_rings(int cx, int cy, int r, int thick, float grow = 1.0f);
// The same for any day: each ring's fraction of its goal.
void fitness_draw_rings_frac(int cx, int cy, int r, int thick, float move, float exercise, float stand);

#define FIT_MOVE_COLOR     ((uint16_t)0xF8AA)
#define FIT_EXERCISE_COLOR ((uint16_t)0x9F45)
#define FIT_STAND_COLOR    ((uint16_t)0x1F5D)

extern Screen rings_celebrate_screen;
