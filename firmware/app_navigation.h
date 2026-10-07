/*
 * app_navigation.h
 * Turn-by-turn directions mirrored from the phone (Google Maps' navigation
 * notification, read by the companion app). The screen opens by itself and
 * wakes the watch at every new maneuver.
 */
#pragma once
#include <ArduinoJson.h>
#include "ui.h"

// Phone link "nav" (icon_msg = false) and "navi" (icon_msg = true).
void navigation_on_link(JsonDocument &doc, bool icon_msg);
bool navigation_active();
const char *navigation_distance();   // "300 m" to the next turn, "" if none

extern Screen navigation_screen;
