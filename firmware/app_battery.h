#pragma once
#include "ui.h"
extern Screen battery_screen;

// How long the battery lasts at the recent pace (screen on and off
// together); < 0 while charging or before there's enough to tell.
float battery_hours_left();
bool  battery_left_text(char *out, int n);   // "~3.2 days left"; false: not known yet
