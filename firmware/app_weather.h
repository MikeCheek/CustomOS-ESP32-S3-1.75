/*
 * app_weather.h
 * Weather app: now (temperature, condition, today's high and low,
 * humidity), the next 12 hours as a strip, and the coming days.
 * The current conditions and days come from the phone's weather push
 * (hal_ble ble_get_weather); the hours, high/low and place name from the
 * phone link: {"t":"wxh","pl":"Milan","hi":21,"lo":12,"h":[[14,19.5,61],...]}
 * (hour, °C, WMO weather code).
 */
#pragma once
#include "ui.h"
#include <ArduinoJson.h>

void weather_on_hourly(JsonDocument &doc);   // phone link "wxh"
void weather_init();      // setup(): the last weather from before a restart (store.h)
void weather_update();    // loop(): keeps a new push from the phone
int  weather_age_min();   // how old the current weather is; -1: not known

// A weather symbol for a condition ("Rain", "Partly cloudy"...), size r.
void weather_draw_icon(int cx, int cy, int r, const char *condition, bool night = false);

extern Screen weather_screen;
