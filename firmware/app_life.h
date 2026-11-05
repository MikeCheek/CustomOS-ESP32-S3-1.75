/*
 * app_life.h
 * Life: the day's healthy habits on the wrist, planned in the companion
 * app's Life screen (inspired by Apple's Mindfulness, Headspace and Calm
 * for breathing, WaterMinder for water, Stretchly for breaks, Fabulous and
 * Streaks for routines, Yazio for meal ideas):
 *
 *   Water      glasses toward a goal; a nudge every N minutes in the day
 *   Stretch    a guided 3-minute routine (six moves, 30 s each); nudges
 *              every N minutes in working hours
 *   Mindful    Breathe sessions (in 4 s, out 6 s), minutes toward a goal;
 *              a nudge at the chosen time
 *   Train      on the chosen days at the chosen time, until the Exercise
 *              ring has the goal's minutes - starts the Workout app
 *   Meals      breakfast, lunch and dinner times with an idea for each from
 *              the phone ("Ate it")
 *   Social     on the chosen days: check in with someone
 *   Wind down  at bedtime: lights low, screens off - turns on DND
 *
 * The Life app shows today's score (the habits done out of those set up)
 * and each habit; nudges come as a card with "Do it" / "Later" (15 min),
 * never in DND, during a workout or while an alarm rings.
 *
 * Phone link (fw 3.5+):
 *   phone -> watch {"t":"life", ...plan...}       the plan (keys in app_life.cpp)
 *                  {"t":"meals","b":"...","l":"...","d":"..."}   today's ideas
 *   watch -> phone {"e":"life","d":20261104,"w":3,"st":1,"mi":2,"tr":1,"me":2,"so":0,"wd":0,"sc":42,
 *                   "h":[[date,w,st,mi,tr,me,so,wd,sc],...]}   today (and the last days on "hi")
 */
#pragma once
#include "ui.h"
#include <ArduinoJson.h>

void life_init();
void life_update();                        // loop(): day change, the nudges
void life_on_plan(JsonDocument &doc);      // phone link "life"
void life_on_meals(JsonDocument &doc);     // phone link "meals"
void life_send(bool history);

int life_score();                          // 0..100 today
// What comes next, for the Smart Stack ("Water in 25 min"); false: nothing planned.
bool life_next(char *out, int n);

extern Screen life_screen;
extern Screen breathe_screen;
extern Screen stretch_screen;
